#include <ark/postgres.h>
#include <ark/text.h>

/*
 * Every message is a type byte, then a big-endian 32-bit length that counts
 * itself but not the type byte, then the body. The startup message has no type.
 */

#define PG_NAME_SIZE 64
#define PG_REQUEST_SIZE 4096
/* Give up on a query that has not finished after this long. */
#define PG_TIMEOUT_MS 15000
#define PG_NULL_LENGTH 0xffffffffUL
#define PG_PROTOCOL_VERSION 196608UL /* 3.0 */

/* ---- Protocol tier ----------------------------------------------------- */

static unsigned long pg_read_u16(const unsigned char *data) {
    return ((unsigned long)data[0] << 8) | (unsigned long)data[1];
}

static unsigned long pg_read_u32(const unsigned char *data) {
    return ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) |
           ((unsigned long)data[2] << 8) | (unsigned long)data[3];
}

static void pg_put_u16(ArkBuffer *buffer, unsigned long value) {
    unsigned char bytes[2];

    bytes[0] = (unsigned char)((value >> 8) & 0xff);
    bytes[1] = (unsigned char)(value & 0xff);
    ark_buffer_append(buffer, bytes, 2);
}

static void pg_put_u32(ArkBuffer *buffer, unsigned long value) {
    unsigned char bytes[4];

    bytes[0] = (unsigned char)((value >> 24) & 0xff);
    bytes[1] = (unsigned char)((value >> 16) & 0xff);
    bytes[2] = (unsigned char)((value >> 8) & 0xff);
    bytes[3] = (unsigned char)(value & 0xff);
    ark_buffer_append(buffer, bytes, 4);
}

static void pg_put_cstr(ArkBuffer *buffer, const char *s) {
    ark_buffer_append(buffer, s, ark_cstr_length(s) + 1);
}

/* Writes the type and a length to fill in later. Returns where the length is. */
static size_t pg_begin_message(ArkBuffer *buffer, char type) {
    size_t length_at;

    ark_buffer_append(buffer, &type, 1);
    length_at = buffer->length;
    pg_put_u32(buffer, 0);

    return length_at;
}

static void pg_end_message(ArkBuffer *buffer, size_t length_at) {
    unsigned long length = (unsigned long)(buffer->length - length_at);

    if (!buffer->overflowed) {
        buffer->data[length_at] = (unsigned char)((length >> 24) & 0xff);
        buffer->data[length_at + 1] = (unsigned char)((length >> 16) & 0xff);
        buffer->data[length_at + 2] = (unsigned char)((length >> 8) & 0xff);
        buffer->data[length_at + 3] = (unsigned char)(length & 0xff);
    }
}

int ark_pg_write_startup(ArkBuffer *buffer, const char *user, const char *database) {
    /* No type byte: the length comes first, then the version and name/value pairs. */
    size_t length_at = buffer->length;

    pg_put_u32(buffer, 0);
    pg_put_u32(buffer, PG_PROTOCOL_VERSION);
    pg_put_cstr(buffer, "user");
    pg_put_cstr(buffer, user);
    pg_put_cstr(buffer, "database");
    pg_put_cstr(buffer, database);
    pg_put_cstr(buffer, "");
    pg_end_message(buffer, length_at);

    return !buffer->overflowed;
}

/*
 * The extended query messages: Parse sends the sql, Bind sends the values,
 * Describe asks for the column count, Execute runs it and Sync ends it. Empty
 * names mean the unnamed statement.
 */
int ark_pg_write_query(ArkBuffer *buffer, const char *sql, const ArkBytes *params, size_t param_count) {
    size_t length_at;
    size_t i;

    length_at = pg_begin_message(buffer, 'P');
    pg_put_cstr(buffer, "");
    pg_put_cstr(buffer, sql);
    pg_put_u16(buffer, 0);
    pg_end_message(buffer, length_at);

    length_at = pg_begin_message(buffer, 'B');
    pg_put_cstr(buffer, "");
    pg_put_cstr(buffer, "");
    pg_put_u16(buffer, 0);
    pg_put_u16(buffer, (unsigned long)param_count);
    for (i = 0; i < param_count; ++i) {
        if (params[i].data == NULL) {
            pg_put_u32(buffer, PG_NULL_LENGTH);
        } else {
            pg_put_u32(buffer, (unsigned long)params[i].length);
            ark_buffer_append(buffer, params[i].data, params[i].length);
        }
    }
    pg_put_u16(buffer, 0);
    pg_end_message(buffer, length_at);

    length_at = pg_begin_message(buffer, 'D');
    ark_buffer_append(buffer, "P", 1);
    pg_put_cstr(buffer, "");
    pg_end_message(buffer, length_at);

    length_at = pg_begin_message(buffer, 'E');
    pg_put_cstr(buffer, "");
    pg_put_u32(buffer, 0);
    pg_end_message(buffer, length_at);

    length_at = pg_begin_message(buffer, 'S');
    pg_end_message(buffer, length_at);

    return !buffer->overflowed;
}

int ark_pg_write_terminate(ArkBuffer *buffer) {
    pg_end_message(buffer, pg_begin_message(buffer, 'X'));
    return !buffer->overflowed;
}

long ark_pg_read_message(ArkBytes data, ArkPgMessage *message) {
    unsigned long length;

    if (data.length < 5) {
        return 0;
    }

    length = pg_read_u32(data.data + 1);
    if (length < 4) {
        return -1;
    }

    if (data.length - 1 < length) {
        return 0;
    }

    message->type = data.data[0];
    message->body.data = data.data + 5;
    message->body.length = (size_t)length - 4;

    return (long)length + 1;
}

int ark_pg_auth_ok(ArkBytes body) {
    /* 0 means no password is needed. */
    return body.length >= 4 && pg_read_u32(body.data) == 0;
}

int ark_pg_read_column_count(ArkBytes body, size_t *count) {
    if (body.length < 2) {
        return 0;
    }

    *count = (size_t)pg_read_u16(body.data);
    return 1;
}

/* A row is a 16-bit column count, then each value as a 32-bit length (all ones for NULL) and its bytes. */
int ark_pg_check_row(ArkBytes body, size_t *column_count) {
    size_t columns;
    size_t offset = 2;
    size_t i;

    if (!ark_pg_read_column_count(body, &columns)) {
        return 0;
    }

    for (i = 0; i < columns; ++i) {
        unsigned long length;

        if (body.length - offset < 4) {
            return 0;
        }

        length = pg_read_u32(body.data + offset);
        offset += 4;

        if (length != PG_NULL_LENGTH) {
            if (length > body.length - offset) {
                return 0;
            }
            offset += (size_t)length;
        }
    }

    if (offset != body.length) {
        return 0;
    }

    *column_count = columns;
    return 1;
}

int ark_pg_read_row_value(ArkBytes body, size_t column, ArkBytes *value) {
    size_t columns;
    size_t offset = 2;
    size_t i;

    if (!ark_pg_read_column_count(body, &columns) || column >= columns) {
        return 0;
    }

    for (i = 0; i <= column; ++i) {
        unsigned long length;

        if (body.length - offset < 4) {
            return 0;
        }

        length = pg_read_u32(body.data + offset);
        offset += 4;

        if (length != PG_NULL_LENGTH && length > body.length - offset) {
            return 0;
        }

        if (i == column) {
            value->data = length == PG_NULL_LENGTH ? NULL : body.data + offset;
            value->length = length == PG_NULL_LENGTH ? 0 : (size_t)length;
            return 1;
        }

        if (length != PG_NULL_LENGTH) {
            offset += (size_t)length;
        }
    }

    return 0;
}

/* The command tag is like "UPDATE 3" or "INSERT 0 3": the count is the last word. */
unsigned long ark_pg_read_command_count(ArkBytes body) {
    size_t end = 0;
    size_t start;
    unsigned long count = 0;

    while (end < body.length && body.data[end] != '\0') {
        ++end;
    }

    start = end;
    while (start > 0 && body.data[start - 1] >= '0' && body.data[start - 1] <= '9') {
        --start;
    }

    if (start == end || start == 0 || body.data[start - 1] != ' ') {
        return 0;
    }

    for (; start < end; ++start) {
        count = count * 10 + (unsigned long)(body.data[start] - '0');
    }

    return count;
}

/* Fields of a type byte and a null-terminated string. 'C' is the code, 'M' the message. */
int ark_pg_read_error(ArkBytes body, ArkBytes *code, ArkBytes *message) {
    size_t i = 0;

    code->data = NULL;
    code->length = 0;
    message->data = NULL;
    message->length = 0;

    while (i < body.length && body.data[i] != '\0') {
        size_t start = i + 1;
        size_t end = start;

        while (end < body.length && body.data[end] != '\0') {
            ++end;
        }

        if (body.data[i] == 'M') {
            message->data = body.data + start;
            message->length = end - start;
        } else if (body.data[i] == 'C') {
            code->data = body.data + start;
            code->length = end - start;
        }

        i = end + 1;
    }

    return 1;
}

/* ---- Query tier: state ------------------------------------------------- */

typedef enum {
    PG_FREE,
    PG_WAITING,     /* has no connection yet */
    PG_CONNECTING,
    PG_STARTING,
    PG_QUERYING,
    PG_CLOSING,
    PG_DONE         /* finished while waiting; free once reported */
} PgState;

/*
 * Where a running query keeps its result. Rows are stored from the start, the
 * bytes still being read sit right after them, and the start of each row is
 * listed backwards from the end:
 *
 *   [row 0][row 1]...[bytes being read]...free...[start of row 1][start of row 0]
 */
typedef struct {
    int in_use;
    unsigned char *data;
} PgSpace;

typedef struct {
    PgState state;
    unsigned long conn_id;
    /* Waiting queries start lowest first. */
    unsigned long order;
    ArkPgStatus status;
    unsigned long started_ms;
    unsigned long elapsed_ms;
    size_t request_length;
    unsigned char request[PG_REQUEST_SIZE];

    /* Only while it has a connection, until the result is reported. */
    PgSpace *space;
    size_t rows_end;
    size_t pending_length;
    size_t row_count;
    size_t column_count;
    int has_column_count;
    unsigned long rows_affected;

    char message[ARK_PG_MESSAGE_SIZE];
    char error_code[6];
} PgQuery;

struct ArkPostgres {
    char user[PG_NAME_SIZE];
    char database[PG_NAME_SIZE];
    /* Owned by the platform, so it survives the app being reloaded. */
    const ArkEndpoint *endpoint;
    int owner;
    size_t max_connections;
    /* max_connections of them, one per running query. */
    PgSpace *spaces;
    PgQuery queries[ARK_PG_MAX_QUERIES];
    unsigned long next_order;
    size_t finished[ARK_PG_MAX_QUERIES];
    size_t finished_count;
};

static int pg_is_active(const PgQuery *query) {
    return query->state == PG_CONNECTING || query->state == PG_STARTING || query->state == PG_QUERYING;
}

static int pg_has_connection(const PgQuery *query) {
    return pg_is_active(query) || query->state == PG_CLOSING;
}

static void pg_set_message(PgQuery *query, const void *text, size_t length) {
    if (length > ARK_PG_MESSAGE_SIZE - 1) {
        length = ARK_PG_MESSAGE_SIZE - 1;
    }

    ark_memory_copy(query->message, text, length);
    query->message[length] = '\0';
}

static size_t *pg_row_starts_end(const PgQuery *query) {
    return (size_t *)(query->space->data + ARK_PG_RESULT_SIZE);
}

static size_t pg_free_bytes(const PgQuery *query) {
    return ARK_PG_RESULT_SIZE - query->rows_end - query->pending_length - query->row_count * sizeof(size_t);
}

/* ---- Query tier: sending ----------------------------------------------- */

static int pg_queue(const ArkPgContext *context, const PgQuery *query, const unsigned char *data, size_t length,
                    int close_after) {
    ArkOutput *output = context->output;
    ArkSend *send;

    if (output->send_count >= output->send_capacity) {
        return 0;
    }

    send = &output->sends[output->send_count++];
    send->conn_id = query->conn_id;
    send->bytes.data = data;
    send->bytes.length = length;
    send->close_after = close_after;

    return 1;
}

/* The message must already be set. */
static void pg_finish(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query, ArkPgStatus status) {
    ArkBuffer terminate;

    query->status = status;
    query->elapsed_ms = context->now_ms - query->started_ms;

    pg->finished[pg->finished_count++] = (size_t)(query - pg->queries);

    /* A query that never got a connection has nothing to close. */
    if (query->state == PG_WAITING) {
        query->state = PG_DONE;
        return;
    }

    query->state = PG_CLOSING;

    if (ark_buffer_push(&terminate, context->scratch, 5) && ark_pg_write_terminate(&terminate)) {
        pg_queue(context, query, terminate.data, terminate.length, 1);
    }
}

static void pg_fail(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query,
                    ArkPgStatus status, const char *reason) {
    pg_set_message(query, reason, ark_cstr_length(reason));
    pg_finish(pg, context, query, status);
}

static void pg_send_startup(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query) {
    ArkBuffer startup;

    query->state = PG_STARTING;

    if (!ark_buffer_push(&startup, context->scratch, PG_NAME_SIZE * 2 + 32) ||
        !ark_pg_write_startup(&startup, pg->user, pg->database) ||
        !pg_queue(context, query, startup.data, startup.length, 0)) {
        pg_fail(pg, context, query, ARK_PG_ERROR, "could not send");
    }
}

/* ---- Query tier: receiving --------------------------------------------- */

static void pg_handle_error(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query, ArkBytes body) {
    ArkBytes code;
    ArkBytes message;

    ark_pg_read_error(body, &code, &message);

    if (message.data != NULL) {
        pg_set_message(query, message.data, message.length);
    } else {
        pg_set_message(query, "unknown error", 13);
    }

    if (code.length == 5) {
        ark_memory_copy(query->error_code, code.data, 5);
        query->error_code[5] = '\0';
    }

    pg_finish(pg, context, query, ARK_PG_ERROR);
}

/*
 * Checks a data row, then keeps it: the body moves down over its own 5 header
 * bytes to sit right after the rows already stored.
 */
static void pg_store_row(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query, ArkBytes body) {
    size_t columns;

    if (!ark_pg_check_row(body, &columns) || (query->has_column_count && columns != query->column_count)) {
        pg_fail(pg, context, query, ARK_PG_ERROR, "bad row");
        return;
    }

    /* Keeping it frees 5 header bytes but takes one more row start. */
    if (pg_free_bytes(query) + 5 < sizeof(size_t)) {
        pg_fail(pg, context, query, ARK_PG_ERROR, "result too large");
        return;
    }

    /* dest < src, so a forward copy is safe. */
    ark_memory_copy(query->space->data + query->rows_end, body.data, body.length);
    pg_row_starts_end(query)[-1 - (long)query->row_count] = query->rows_end;

    query->rows_end += body.length;
    query->row_count += 1;
    query->column_count = columns;
    query->has_column_count = 1;
}

static void pg_handle_message(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query,
                              const ArkPgMessage *message) {
    switch (message->type) {
    case 'R':
        if (!ark_pg_auth_ok(message->body)) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "server wants a password");
        }
        break;

    case 'T':
        if (ark_pg_read_column_count(message->body, &query->column_count)) {
            query->has_column_count = 1;
        }
        break;

    case 'n':
        /* No data: the command returns no rows, like an UPDATE. */
        query->column_count = 0;
        query->has_column_count = 1;
        break;

    case 'D':
        pg_store_row(pg, context, query, message->body);
        break;

    case 'C':
        query->rows_affected = ark_pg_read_command_count(message->body);
        break;

    case 'E':
        pg_handle_error(pg, context, query, message->body);
        break;

    case 'Z':
        /* The server is ready: after startup that means send the query, after it we are done. */
        if (query->state == PG_STARTING) {
            query->state = PG_QUERYING;
            if (!pg_queue(context, query, query->request, query->request_length, 0)) {
                pg_fail(pg, context, query, ARK_PG_ERROR, "could not send");
            }
        } else {
            query->message[0] = '\0';
            pg_finish(pg, context, query, ARK_PG_OK);
        }
        break;

    default:
        break;
    }
}

static void pg_process_messages(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query) {
    while (pg_is_active(query)) {
        size_t start = query->rows_end;
        ArkBytes pending;
        ArkPgMessage message;
        long consumed;

        pending.data = query->space->data + start;
        pending.length = query->pending_length;

        /* A message that could never fit fails now, rather than filling the space first. */
        if (pending.length >= 5 && pg_read_u32(pending.data + 1) + 1 > ARK_PG_RESULT_SIZE) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "result too large");
            return;
        }

        consumed = ark_pg_read_message(pending, &message);
        if (consumed == 0) {
            return;
        }

        if (consumed < 0) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "bad message length");
            return;
        }

        /* A stored row moves rows_end on, so this reads where the rest now goes. */
        pg_handle_message(pg, context, query, &message);

        /* Move the bytes after this message up to the end of the rows. */
        query->pending_length -= (size_t)consumed;
        ark_memory_copy(query->space->data + query->rows_end, query->space->data + start + consumed,
                        query->pending_length);
    }
}

static void pg_receive(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query, const ArkEvent *event) {
    const unsigned char *data = event->bytes.data;
    size_t remaining = event->bytes.length;

    while (remaining > 0 && pg_is_active(query)) {
        size_t take = pg_free_bytes(query);

        if (take == 0) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "result too large");
            return;
        }

        if (take > remaining) {
            take = remaining;
        }

        ark_memory_copy(query->space->data + query->rows_end + query->pending_length, data, take);
        query->pending_length += take;
        data += take;
        remaining -= take;

        pg_process_messages(pg, context, query);
    }
}

/* ---- Query tier: queue ------------------------------------------------- */

static PgSpace *pg_take_space(ArkPostgres *pg) {
    size_t i;

    for (i = 0; i < pg->max_connections; ++i) {
        if (!pg->spaces[i].in_use) {
            pg->spaces[i].in_use = 1;
            return &pg->spaces[i];
        }
    }

    return NULL;
}

/* Gives connections to waiting queries, oldest first, up to the cap and while ids are free. */
static void pg_start_waiting(ArkPostgres *pg, const ArkPgContext *context) {
    for (;;) {
        PgQuery *oldest = NULL;
        size_t open = 0;
        size_t i;

        for (i = 0; i < ARK_PG_MAX_QUERIES; ++i) {
            PgQuery *query = &pg->queries[i];

            if (pg_has_connection(query)) {
                ++open;
            } else if (query->state == PG_WAITING && (oldest == NULL || query->order < oldest->order)) {
                oldest = query;
            }
        }

        if (oldest == NULL || open >= pg->max_connections) {
            return;
        }

        /* Spaces are only held by queries with a connection, so one is free here. */
        oldest->space = pg_take_space(pg);
        if (oldest->space == NULL) {
            return;
        }

        if (!ark_upstream_open(context->upstreams, context->output, pg->endpoint, pg->owner, &oldest->conn_id)) {
            oldest->space->in_use = 0;
            oldest->space = NULL;
            return;
        }

        oldest->state = PG_CONNECTING;
        oldest->rows_end = 0;
        oldest->pending_length = 0;
        oldest->row_count = 0;
        oldest->column_count = 0;
        oldest->has_column_count = 0;
        oldest->rows_affected = 0;
    }
}

/* ---- Query tier: interface --------------------------------------------- */

ArkPostgres *ark_pg_create(Memory *permanent, const ArkEndpoint *endpoint, int owner, size_t max_connections,
                           const char *user, const char *database) {
    size_t user_length = ark_cstr_length(user);
    size_t database_length = ark_cstr_length(database);
    ArkPostgres *pg;
    size_t i;

    if (user_length >= PG_NAME_SIZE || database_length >= PG_NAME_SIZE || max_connections == 0) {
        return NULL;
    }

    pg = (ArkPostgres *)ark_memory_push(permanent, sizeof(ArkPostgres));
    if (pg == NULL) {
        return NULL;
    }

    pg->spaces = (PgSpace *)ark_memory_push(permanent, sizeof(PgSpace) * max_connections);
    if (pg->spaces == NULL) {
        return NULL;
    }

    for (i = 0; i < max_connections; ++i) {
        pg->spaces[i].data = (unsigned char *)ark_memory_push(permanent, ARK_PG_RESULT_SIZE);
        if (pg->spaces[i].data == NULL) {
            return NULL;
        }
    }

    ark_memory_copy(pg->user, user, user_length + 1);
    ark_memory_copy(pg->database, database, database_length + 1);
    pg->endpoint = endpoint;
    pg->owner = owner;
    pg->max_connections = max_connections;

    return pg;
}

ArkPgStart ark_pg_start(ArkPostgres *pg, const ArkPgContext *context, const char *sql,
                        const ArkBytes *params, size_t param_count, unsigned long *query_id) {
    size_t i;

    for (i = 0; i < ARK_PG_MAX_QUERIES; ++i) {
        PgQuery *query = &pg->queries[i];

        if (query->state == PG_FREE) {
            ArkBuffer request;

            ark_buffer_init(&request, query->request, PG_REQUEST_SIZE);
            if (!ark_pg_write_query(&request, sql, params, param_count)) {
                return ARK_PG_TOO_LARGE;
            }

            /* Every query waits first, so none can jump ahead of older ones. */
            query->state = PG_WAITING;
            query->order = pg->next_order++;
            query->started_ms = context->now_ms;
            query->request_length = request.length;
            query->space = NULL;
            query->message[0] = '\0';
            query->error_code[0] = '\0';

            pg_start_waiting(pg, context);

            *query_id = (unsigned long)i;
            return ARK_PG_STARTED;
        }
    }

    return ARK_PG_BUSY;
}

void ark_pg_handle_event(ArkPostgres *pg, const ArkPgContext *context, const ArkEvent *event) {
    PgQuery *query = NULL;
    size_t i;

    for (i = 0; i < ARK_PG_MAX_QUERIES; ++i) {
        if (pg_has_connection(&pg->queries[i]) && pg->queries[i].conn_id == event->conn_id) {
            query = &pg->queries[i];
        }
    }

    if (query == NULL) {
        return;
    }

    switch (event->type) {
    case ARK_EVENT_OPEN:
        pg_send_startup(pg, context, query);
        break;

    case ARK_EVENT_DATA:
        pg_receive(pg, context, query, event);
        break;

    case ARK_EVENT_CLOSE:
        if (pg_is_active(query)) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "connection lost");
        }
        query->state = PG_FREE;
        break;
    }
}

void ark_pg_update(ArkPostgres *pg, const ArkPgContext *context) {
    size_t i;

    for (i = 0; i < ARK_PG_MAX_QUERIES; ++i) {
        PgQuery *query = &pg->queries[i];

        if ((pg_is_active(query) || query->state == PG_WAITING) &&
            context->now_ms - query->started_ms >= PG_TIMEOUT_MS) {
            pg_fail(pg, context, query, ARK_PG_TIMEOUT, "timed out");
        }
    }

    pg_start_waiting(pg, context);
}

int ark_pg_next_result(ArkPostgres *pg, ArkPgResult *result) {
    PgQuery *query;

    if (pg->finished_count == 0) {
        return 0;
    }

    query = &pg->queries[pg->finished[--pg->finished_count]];

    result->query_id = (unsigned long)(query - pg->queries);
    result->status = query->status;
    result->elapsed_ms = query->elapsed_ms;
    result->error = query->message;
    result->error_code = query->error_code;
    result->rows_affected = 0;
    result->row_count = 0;
    result->column_count = 0;
    result->rows = NULL;
    result->rows_length = 0;
    result->row_starts_end = NULL;

    if (query->status == ARK_PG_OK) {
        result->rows_affected = query->rows_affected;
        result->row_count = query->row_count;
        result->column_count = query->column_count;
        result->rows = query->space->data;
        result->rows_length = query->rows_end;
        result->row_starts_end = pg_row_starts_end(query);
    }

    /*
     * The result stays readable until the next call into this module: nothing
     * writes to a space or a free query before then.
     */
    if (query->space != NULL) {
        query->space->in_use = 0;
        query->space = NULL;
    }

    if (query->state == PG_DONE) {
        query->state = PG_FREE;
    }

    return 1;
}

int ark_pg_row(const ArkPgResult *result, size_t row, ArkBytes *body) {
    size_t start;
    size_t end;

    if (row >= result->row_count) {
        return 0;
    }

    start = result->row_starts_end[-1 - (long)row];
    end = row + 1 < result->row_count ? result->row_starts_end[-2 - (long)row] : result->rows_length;

    body->data = result->rows + start;
    body->length = end - start;

    return 1;
}

int ark_pg_value(const ArkPgResult *result, size_t row, size_t column, ArkBytes *value) {
    ArkBytes body;

    return ark_pg_row(result, row, &body) && ark_pg_read_row_value(body, column, value);
}
