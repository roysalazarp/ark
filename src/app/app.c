#include <ark/platform.h>
#include <ark/memory.h>
#include <ark/text.h>
#include <ark/http.h>
#include <ark/postgres.h>
#include <ark/upstream.h>

/*
 * The app. It is loaded by a platform layer and only ever runs inside
 * ark_app_update. No globals: all state lives in memory the platform owns,
 * so it survives the app being reloaded.
 */

/*
 * The app has no password support, so it connects to a pooler that logs in to
 * Postgres for it. See compose.yml. The platform says where the "db" endpoint is.
 */
#define DB_ENDPOINT "db"
#define DB_USER "ark"
#define DB_NAME "ark"
/* Match default_pool_size in pgbouncer/pgbouncer.ini: more would only wait there. */
#define DB_MAX_CONNECTIONS 20
#define SLOW_QUERY "SELECT 'hello from postgres at ' || clock_timestamp()::text FROM pg_sleep(3)"
#define HELLO_QUERY "SELECT 'hello, ' || $1::text"
#define HELLO_PREFIX "/hello/"
/* Returns $1 rows of 3 columns: a number, ten times it, and NULL or empty text. */
#define ROWS_QUERY "SELECT n, n * 10, CASE WHEN n % 2 = 0 THEN NULL ELSE '' END FROM generate_series(1, $1::int) AS n"
#define ROWS_PREFIX "/rows/"
#define HEADER_PREFIX "/header/"

/* Who owns each outbound id. See upstream.h. */
enum {
    OWNER_POSTGRES = 1
};

typedef struct {
    Memory permanent;
    Memory transient;
    ArkHttp *http;
    ArkPostgres *postgres;
    /* The client waiting for each running query, indexed by query id. */
    ArkHttpTicket query_tickets[ARK_PG_MAX_QUERIES];
    ArkUpstreams upstreams;
} AppState;

/* Everything the app works with during one update. */
typedef struct {
    AppState *state;
    ArkAppMemory *memory;
    ArkHttpContext http;
    ArkPgContext pg;
} Tick;

static void app_log(ArkAppMemory *memory, const char *message) {
    memory->platform.log(message, ark_cstr_length(message));
}

static void respond(Tick *tick, const ArkHttpRequest *request, const char *status, const char *text) {
    ark_http_respond_text(tick->state->http, &tick->http, request, status, text);
}

static int path_is(const ArkHttpRequest *request, const char *path) {
    return ark_bytes_equal_cstr(request->path, path);
}

/* The rest of the path after prefix, if the path starts with it and has more. */
static int path_after(const ArkHttpRequest *request, const char *prefix, size_t prefix_length, ArkBytes *rest) {
    ArkBytes start;

    start.data = request->path.data;
    start.length = prefix_length;

    if (request->path.length <= prefix_length || !ark_bytes_equal_cstr(start, prefix)) {
        return 0;
    }

    rest->data = request->path.data + prefix_length;
    rest->length = request->path.length - prefix_length;
    return 1;
}

/* ---- Queries ----------------------------------------------------------- */

/* The client is answered when the result arrives. */
static void start_query(Tick *tick, const ArkHttpRequest *request, const char *sql,
                        const ArkBytes *params, size_t param_count) {
    AppState *state = tick->state;
    unsigned long query_id;
    ArkPgStart started;

    if (state->postgres == NULL) {
        respond(tick, request, "503 Service Unavailable", "No Database\n");
        return;
    }

    /* The query copies the params, so the request can be parked after. */
    started = ark_pg_start(state->postgres, &tick->pg, sql, params, param_count, &query_id);

    if (started == ARK_PG_TOO_LARGE) {
        respond(tick, request, "414 URI Too Long", "Too Long\n");
        return;
    }

    if (started == ARK_PG_BUSY) {
        respond(tick, request, "503 Service Unavailable", "Too Many Queries\n");
        return;
    }

    state->query_tickets[query_id] = ark_http_park(state->http, request);

    app_log(tick->memory, "app: query started");
}

/* ---- Requests ---------------------------------------------------------- */

/* Answers with the value of the header named in the path, like /header/User-Agent. */
static void respond_with_header(Tick *tick, const ArkHttpRequest *request, ArkBytes name_bytes) {
    char name[64];
    ArkBytes value;
    ArkBuffer body;

    if (name_bytes.length >= sizeof(name)) {
        respond(tick, request, "414 URI Too Long", "Too Long\n");
        return;
    }

    ark_memory_copy(name, name_bytes.data, name_bytes.length);
    name[name_bytes.length] = '\0';

    if (!ark_http_header(request, name, &value)) {
        respond(tick, request, "404 Not Found", "No Such Header\n");
        return;
    }

    if (!ark_buffer_push(&body, &tick->state->transient, value.length + 1)) {
        respond(tick, request, "500 Internal Server Error", "Out Of Memory\n");
        return;
    }

    ark_buffer_append(&body, value.data, value.length);
    ark_buffer_append_cstr(&body, "\n");

    ark_http_respond(tick->state->http, &tick->http, request, "200 OK", "text/plain",
                     ark_buffer_bytes(&body));
}

static void handle_request(Tick *tick, const ArkHttpRequest *request) {
    ArkBytes rest;

    if (!ark_bytes_equal_cstr(request->method, "GET")) {
        respond(tick, request, "405 Method Not Allowed", "Method Not Allowed\n");
    } else if (path_is(request, "/slow")) {
        start_query(tick, request, SLOW_QUERY, NULL, 0);
    } else if (path_after(request, HELLO_PREFIX, sizeof(HELLO_PREFIX) - 1, &rest)) {
        start_query(tick, request, HELLO_QUERY, &rest, 1);
    } else if (path_after(request, ROWS_PREFIX, sizeof(ROWS_PREFIX) - 1, &rest)) {
        start_query(tick, request, ROWS_QUERY, &rest, 1);
    } else if (path_after(request, HEADER_PREFIX, sizeof(HEADER_PREFIX) - 1, &rest)) {
        respond_with_header(tick, request, rest);
    } else if (path_is(request, "/")) {
        respond(tick, request, "200 OK", "Hello from Ark\n");
    } else {
        respond(tick, request, "404 Not Found", "Not Found\n");
    }
}

/* Answers every request that is ready. */
static void handle_requests(Tick *tick) {
    ArkHttpRequest request;

    while (ark_http_next_request(tick->state->http, &tick->http, &request)) {
        handle_request(tick, &request);
    }
}

/* ---- Results ----------------------------------------------------------- */

static void log_query_result(Tick *tick, const ArkPgResult *result) {
    ArkBuffer buffer;

    if (!ark_buffer_push(&buffer, &tick->state->transient, ARK_PG_MESSAGE_SIZE + 128)) {
        return;
    }

    if (result->status == ARK_PG_OK) {
        ark_buffer_append_cstr(&buffer, "app: query finished after ");
        ark_buffer_append_unsigned(&buffer, result->elapsed_ms);
        ark_buffer_append_cstr(&buffer, " ms: ");
        ark_buffer_append_unsigned(&buffer, (unsigned long)result->row_count);
        ark_buffer_append_cstr(&buffer, " rows, ");
        ark_buffer_append_unsigned(&buffer, (unsigned long)result->column_count);
        ark_buffer_append_cstr(&buffer, " columns, count ");
        ark_buffer_append_unsigned(&buffer, result->rows_affected);
    } else {
        ark_buffer_append_cstr(&buffer, "app: query failed after ");
        ark_buffer_append_unsigned(&buffer, result->elapsed_ms);
        ark_buffer_append_cstr(&buffer, " ms: ");
        if (result->error_code[0] != '\0') {
            ark_buffer_append_cstr(&buffer, "[");
            ark_buffer_append_cstr(&buffer, result->error_code);
            ark_buffer_append_cstr(&buffer, "] ");
        }
        ark_buffer_append_cstr(&buffer, result->error);
    }

    tick->memory->platform.log((const char *)buffer.data, buffer.length);
}

/* One line per row, values separated by tabs, NULL written as NULL. Returns 0 if out of memory. */
static int format_rows(Tick *tick, const ArkPgResult *result, ArkBytes *text) {
    ArkBytes value;
    ArkBuffer buffer;
    size_t size = 0;
    size_t row;
    size_t column;

    for (row = 0; row < result->row_count; ++row) {
        for (column = 0; column < result->column_count; ++column) {
            ark_pg_value(result, row, column, &value);
            size += (value.data == NULL ? 4 : value.length) + 1;
        }
        size += 1;
    }

    if (!ark_buffer_push(&buffer, &tick->state->transient, size + 1)) {
        return 0;
    }

    for (row = 0; row < result->row_count; ++row) {
        for (column = 0; column < result->column_count; ++column) {
            ark_pg_value(result, row, column, &value);

            if (column > 0) {
                ark_buffer_append_cstr(&buffer, "\t");
            }

            if (value.data == NULL) {
                ark_buffer_append_cstr(&buffer, "NULL");
            } else {
                ark_buffer_append(&buffer, value.data, value.length);
            }
        }
        ark_buffer_append_cstr(&buffer, "\n");
    }

    text->data = buffer.data;
    text->length = buffer.length;
    return 1;
}

static void answer_query(Tick *tick, const ArkPgResult *result) {
    ArkHttp *http = tick->state->http;
    ArkHttpRequest request;
    ArkBytes body;

    /* Nobody to answer if the client left. */
    if (!ark_http_resume(http, tick->state->query_tickets[result->query_id], &request)) {
        return;
    }

    if (result->status == ARK_PG_OK) {
        if (format_rows(tick, result, &body)) {
            ark_http_respond(http, &tick->http, &request, "200 OK", "text/plain", body);
        } else {
            respond(tick, &request, "500 Internal Server Error", "Out Of Memory\n");
        }
    } else if (result->status == ARK_PG_TIMEOUT) {
        respond(tick, &request, "504 Gateway Timeout", "Database Timeout\n");
    } else if (ark_cstr_equal(result->error_code, "23505")) {
        respond(tick, &request, "409 Conflict", "Already Exists\n");
    } else {
        respond(tick, &request, "502 Bad Gateway", "Database Error\n");
    }
}

static void handle_query_results(Tick *tick) {
    ArkPgResult result;

    if (tick->state->postgres == NULL) {
        return;
    }

    while (ark_pg_next_result(tick->state->postgres, &result)) {
        log_query_result(tick, &result);
        answer_query(tick, &result);
    }
}

/* ---- Update ------------------------------------------------------------ */

static int init(AppState *state, ArkAppMemory *memory) {
    const ArkEndpoint *db_endpoint;

    ark_memory_init(&state->permanent,
                    (unsigned char *)memory->permanent + sizeof(AppState),
                    memory->permanent_size - sizeof(AppState));
    ark_memory_init(&state->transient, memory->transient, memory->transient_size);

    state->http = ark_http_create(&state->permanent);
    if (state->http == NULL) {
        app_log(memory, "app: permanent memory is too small");
        return 0;
    }

    db_endpoint = ark_find_endpoint(&memory->config, DB_ENDPOINT);
    if (db_endpoint == NULL) {
        app_log(memory, "app: no \"db\" endpoint, so database routes are off");
        return 1;
    }

    state->postgres = ark_pg_create(&state->permanent, db_endpoint, OWNER_POSTGRES, DB_MAX_CONNECTIONS,
                                    DB_USER, DB_NAME);
    if (state->postgres == NULL) {
        app_log(memory, "app: permanent memory is too small");
        return 0;
    }

    return 1;
}

ARK_APP_UPDATE(ark_app_update) {
    AppState *state = (AppState *)memory->permanent;
    Tick tick;
    size_t i;

    if (!memory->is_initialized) {
        if (!init(state, memory)) {
            return;
        }
        memory->is_initialized = 1;
    }

    ark_memory_reset(&state->transient, state->transient.start);

    tick.state = state;
    tick.memory = memory;
    tick.http.output = output;
    tick.http.scratch = &state->transient;
    tick.http.platform = &memory->platform;
    tick.http.now_ms = input->now_ms;
    tick.pg.output = output;
    tick.pg.scratch = &state->transient;
    tick.pg.upstreams = &state->upstreams;
    tick.pg.now_ms = input->now_ms;

    for (i = 0; i < input->event_count; ++i) {
        const ArkEvent *event = &input->events[i];

        if (event->conn_id < ARK_MAX_CONNECTIONS) {
            ark_http_handle_event(state->http, &tick.http, event);
            handle_requests(&tick);
            continue;
        }

        /* Outbound: pass it to whichever client owns the id. */
        if (ark_upstream_route(&state->upstreams, event) == OWNER_POSTGRES) {
            ark_pg_handle_event(state->postgres, &tick.pg, event);
            handle_query_results(&tick);
            /* Answered clients may have more requests waiting behind the ones just answered. */
            handle_requests(&tick);
        }
    }

    if (state->postgres != NULL) {
        ark_pg_update(state->postgres, &tick.pg);
    }
    handle_query_results(&tick);
    handle_requests(&tick);

    /*
     * It answers anything left unanswered and closes idle clients. Requests
     * queued behind those answers are handled right after, not next tick.
     */
    ark_http_update(state->http, &tick.http);
    handle_requests(&tick);
}
