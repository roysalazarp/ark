#include <ark/postgres.h>
#include <ark/text.h>

/*
 * Each query is a state machine moved along by events: connect, send the
 * startup message, send the query, then report the result. Each running query
 * uses its own outbound connection. When none is free, a query waits, and
 * waiting queries start in the order they were asked for. A query id is its
 * index in queries.
 *
 * Every message is a type byte, then a big-endian 32-bit length that counts
 * itself but not the type byte, then the body. The startup message has no type.
 *
 * Everything kept between ticks is copied into this module's memory. A pointer
 * to the caller's strings would dangle after the app is reloaded.
 */

#define PG_NAME_SIZE 64
#define PG_REQUEST_SIZE 4096
/* Give up on a query that has not finished after this long. */
#define PG_TIMEOUT_MS 15000
#define PG_NULL_LENGTH 0xffffffffUL

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
 *
 * A row is kept as Postgres sends it: a 16-bit column count, then each value as
 * a 32-bit length (all ones for NULL) and its bytes.
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

static unsigned long pg_read_u16(const unsigned char *data) {
    return ((unsigned long)data[0] << 8) | (unsigned long)data[1];
}

static unsigned long pg_read_u32(const unsigned char *data) {
    return ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) |
           ((unsigned long)data[2] << 8) | (unsigned long)data[3];
}

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

/* ---- Message fields ---------------------------------------------------- */

static void pg_put_u16(ArkBuffer *writer, unsigned long value) {
    unsigned char bytes[2];

    bytes[0] = (unsigned char)((value >> 8) & 0xff);
    bytes[1] = (unsigned char)(value & 0xff);
    ark_buffer_append(writer, bytes, 2);
}

static void pg_put_u32(ArkBuffer *writer, unsigned long value) {
    unsigned char bytes[4];

    bytes[0] = (unsigned char)((value >> 24) & 0xff);
    bytes[1] = (unsigned char)((value >> 16) & 0xff);
    bytes[2] = (unsigned char)((value >> 8) & 0xff);
    bytes[3] = (unsigned char)(value & 0xff);
    ark_buffer_append(writer, bytes, 4);
}

/* Writes the type and a length to fill in later. Returns where the length is. */
static size_t pg_begin_message(ArkBuffer *writer, char type) {
    size_t length_at;

    ark_buffer_append(writer, &type, 1);
    length_at = writer->length;
    pg_put_u32(writer, 0);

    return length_at;
}

static void pg_end_message(ArkBuffer *writer, size_t length_at) {
    unsigned long length = (unsigned long)(writer->length - length_at);

    if (!writer->overflowed) {
        writer->data[length_at] = (unsigned char)((length >> 24) & 0xff);
        writer->data[length_at + 1] = (unsigned char)((length >> 16) & 0xff);
        writer->data[length_at + 2] = (unsigned char)((length >> 8) & 0xff);
        writer->data[length_at + 3] = (unsigned char)(length & 0xff);
    }
}

/* ---- Sending ----------------------------------------------------------- */

static int pg_queue(const ArkPgContext *context, const PgQuery *query,
                    const unsigned char *data, size_t length, int close_after) {
    ArkOutput *output = context->output;
    ArkSend *send;

    if (output->send_count >= output->send_capacity) {
        return 0;
    }

    send = &output->sends[output->send_count++];
    send->conn_id = query->conn_id;
    send->data = data;
    send->length = length;
    send->close_after = close_after;

    return 1;
}

static int pg_send(const ArkPgContext *context, const PgQuery *query,
                   char type, const void *body, size_t body_length, int close_after) {
    ArkBuffer message;

    if (!ark_buffer_push(&message, context->scratch, body_length + 5)) {
        return 0;
    }

    if (type != '\0') {
        ark_buffer_append(&message, &type, 1);
    }
    pg_put_u32(&message, (unsigned long)body_length + 4);
    ark_buffer_append(&message, body, body_length);

    return pg_queue(context, query, message.data, message.length, close_after);
}

/* ---- Finishing --------------------------------------------------------- */

/* The message must already be set. */
static void pg_finish(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query, ArkPgStatus status) {
    query->status = status;
    query->elapsed_ms = context->now_ms - query->started_ms;

    pg->finished[pg->finished_count++] = (size_t)(query - pg->queries);

    /* A query that never got a connection has nothing to close. */
    if (query->state == PG_WAITING) {
        query->state = PG_DONE;
        return;
    }

    query->state = PG_CLOSING;
    pg_send(context, query, 'X', NULL, 0, 1);
}

static void pg_fail(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query,
                    ArkPgStatus status, const char *reason) {
    pg_set_message(query, reason, ark_cstr_length(reason));
    pg_finish(pg, context, query, status);
}

/* ---- Receiving --------------------------------------------------------- */

static void pg_send_startup(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query) {
    /* Protocol version 3.0, then name/value pairs, then an empty name. */
    static const char version[] = "\0\3\0\0user";
    static const char database[] = "database";
    char body[sizeof(version) + sizeof(database) + PG_NAME_SIZE * 2 + 1];
    size_t user_length = ark_cstr_length(pg->user);
    size_t database_length = ark_cstr_length(pg->database);
    size_t length = 0;

    ark_memory_copy(body, version, sizeof(version));
    length += sizeof(version);
    ark_memory_copy(body + length, pg->user, user_length + 1);
    length += user_length + 1;
    ark_memory_copy(body + length, database, sizeof(database));
    length += sizeof(database);
    ark_memory_copy(body + length, pg->database, database_length + 1);
    length += database_length + 1;
    body[length++] = '\0';

    query->state = PG_STARTING;

    if (!pg_send(context, query, '\0', body, length, 0)) {
        pg_fail(pg, context, query, ARK_PG_ERROR, "could not send");
    }
}

static void pg_handle_error(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query,
                            const unsigned char *body, size_t length) {
    size_t i = 0;

    pg_set_message(query, "unknown error", 13);

    /* Fields of a type byte and a null-terminated string. 'C' is the code, 'M' the message. */
    while (i < length && body[i] != '\0') {
        size_t start = i + 1;
        size_t end = start;

        while (end < length && body[end] != '\0') {
            ++end;
        }

        if (body[i] == 'M') {
            pg_set_message(query, body + start, end - start);
        } else if (body[i] == 'C' && end - start == 5) {
            ark_memory_copy(query->error_code, body + start, 5);
            query->error_code[5] = '\0';
        }

        i = end + 1;
    }

    pg_finish(pg, context, query, ARK_PG_ERROR);
}

/* The command tag is like "UPDATE 3" or "INSERT 0 3": the count is the last word. */
static void pg_read_command_tag(PgQuery *query, const unsigned char *body, size_t length) {
    size_t end = 0;
    size_t start;
    unsigned long count = 0;

    while (end < length && body[end] != '\0') {
        ++end;
    }

    start = end;
    while (start > 0 && body[start - 1] >= '0' && body[start - 1] <= '9') {
        --start;
    }

    if (start == end || start == 0 || body[start - 1] != ' ') {
        return;
    }

    for (; start < end; ++start) {
        count = count * 10 + (unsigned long)(body[start] - '0');
    }

    query->rows_affected = count;
}

static void pg_handle_message(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query,
                              unsigned char type, const unsigned char *body, size_t length) {
    switch (type) {
    case 'R':
        /* 0 means no password is needed. Any other request can't be answered yet. */
        if (length < 4 || pg_read_u32(body) != 0) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "server wants a password");
        }
        break;

    case 'T':
        /* Row description: starts with the column count. */
        if (length >= 2) {
            query->column_count = pg_read_u16(body);
            query->has_column_count = 1;
        }
        break;

    case 'n':
        /* No data: the command returns no rows, like an UPDATE. */
        query->column_count = 0;
        query->has_column_count = 1;
        break;

    case 'C':
        pg_read_command_tag(query, body, length);
        break;

    case 'E':
        pg_handle_error(pg, context, query, body, length);
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

/*
 * Checks a data row, then keeps it: the body moves down over its own 5 header
 * bytes to sit right after the rows already stored.
 */
static void pg_store_row(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query,
                         unsigned char *body, size_t length) {
    unsigned long columns;
    size_t offset = 2;
    unsigned long i;

    if (length < 2) {
        pg_fail(pg, context, query, ARK_PG_ERROR, "bad row");
        return;
    }

    columns = pg_read_u16(body);
    if (query->has_column_count && columns != query->column_count) {
        pg_fail(pg, context, query, ARK_PG_ERROR, "bad row");
        return;
    }

    for (i = 0; i < columns; ++i) {
        unsigned long value_length;

        if (length - offset < 4) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "bad row");
            return;
        }

        value_length = pg_read_u32(body + offset);
        offset += 4;

        if (value_length != PG_NULL_LENGTH) {
            if (value_length > length - offset) {
                pg_fail(pg, context, query, ARK_PG_ERROR, "bad row");
                return;
            }
            offset += value_length;
        }
    }

    if (offset != length) {
        pg_fail(pg, context, query, ARK_PG_ERROR, "bad row");
        return;
    }

    /* Keeping it frees 5 header bytes but takes one more row start. */
    if (pg_free_bytes(query) + 5 < sizeof(size_t)) {
        pg_fail(pg, context, query, ARK_PG_ERROR, "result too large");
        return;
    }

    /* dest < src, so a forward copy is safe. */
    ark_memory_copy(query->space->data + query->rows_end, body, length);
    pg_row_starts_end(query)[-1 - (long)query->row_count] = query->rows_end;

    query->rows_end += length;
    query->row_count += 1;
    query->column_count = columns;
    query->has_column_count = 1;
}

static void pg_process_messages(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query) {
    while (pg_is_active(query) && query->pending_length >= 5) {
        size_t start = query->rows_end;
        unsigned char *message = query->space->data + start;
        unsigned long length = pg_read_u32(message + 1);
        size_t consumed;

        if (length < 4) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "bad message length");
            return;
        }

        if (length + 1 > ARK_PG_RESULT_SIZE) {
            pg_fail(pg, context, query, ARK_PG_ERROR, "result too large");
            return;
        }

        if (query->pending_length < length + 1) {
            return;
        }

        consumed = (size_t)length + 1;

        if (message[0] == 'D') {
            pg_store_row(pg, context, query, message + 5, consumed - 5);
        } else {
            pg_handle_message(pg, context, query, message[0], message + 5, consumed - 5);
        }

        /* Move the bytes after this message up to the end of the rows. */
        query->pending_length -= consumed;
        ark_memory_copy(query->space->data + query->rows_end, query->space->data + start + consumed,
                        query->pending_length);
    }
}

static void pg_receive(ArkPostgres *pg, const ArkPgContext *context, PgQuery *query, const ArkEvent *event) {
    const unsigned char *data = event->data;
    size_t remaining = event->length;

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

/* ---- Writing queries --------------------------------------------------- */

/*
 * The extended query messages: Parse sends the sql, Bind sends the values,
 * Describe asks for the column count, Execute runs it and Sync ends it. Empty
 * names mean the unnamed statement.
 */
static void pg_write_query(ArkBuffer *writer, const char *sql, const ArkPgValue *params, size_t param_count) {
    size_t length_at;
    size_t i;

    length_at = pg_begin_message(writer, 'P');
    ark_buffer_append(writer, "", 1);
    ark_buffer_append(writer, sql, ark_cstr_length(sql) + 1);
    pg_put_u16(writer, 0);
    pg_end_message(writer, length_at);

    length_at = pg_begin_message(writer, 'B');
    ark_buffer_append(writer, "", 1);
    ark_buffer_append(writer, "", 1);
    pg_put_u16(writer, 0);
    pg_put_u16(writer, (unsigned long)param_count);
    for (i = 0; i < param_count; ++i) {
        if (params[i].data == NULL) {
            pg_put_u32(writer, PG_NULL_LENGTH);
        } else {
            pg_put_u32(writer, (unsigned long)params[i].length);
            ark_buffer_append(writer, params[i].data, params[i].length);
        }
    }
    pg_put_u16(writer, 0);
    pg_end_message(writer, length_at);

    length_at = pg_begin_message(writer, 'D');
    ark_buffer_append(writer, "P", 1);
    ark_buffer_append(writer, "", 1);
    pg_end_message(writer, length_at);

    length_at = pg_begin_message(writer, 'E');
    ark_buffer_append(writer, "", 1);
    pg_put_u32(writer, 0);
    pg_end_message(writer, length_at);

    length_at = pg_begin_message(writer, 'S');
    pg_end_message(writer, length_at);
}

/* ---- Queue ------------------------------------------------------------- */

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

/* ---- Interface --------------------------------------------------------- */

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
                        const ArkPgValue *params, size_t param_count, unsigned long *query_id) {
    size_t i;

    for (i = 0; i < ARK_PG_MAX_QUERIES; ++i) {
        PgQuery *query = &pg->queries[i];

        if (query->state == PG_FREE) {
            ArkBuffer writer;

            ark_buffer_init(&writer, query->request, PG_REQUEST_SIZE);

            pg_write_query(&writer, sql, params, param_count);
            if (writer.overflowed) {
                return ARK_PG_TOO_LARGE;
            }

            /* Every query waits first, so none can jump ahead of older ones. */
            query->state = PG_WAITING;
            query->order = pg->next_order++;
            query->started_ms = context->now_ms;
            query->request_length = writer.length;
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
    result->row_starts_end = NULL;

    if (query->status == ARK_PG_OK) {
        result->rows_affected = query->rows_affected;
        result->row_count = query->row_count;
        result->column_count = query->column_count;
        result->rows = query->space->data;
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

int ark_pg_value(const ArkPgResult *result, size_t row, size_t column, ArkPgValue *value) {
    const unsigned char *data;
    size_t offset = 2;
    size_t i;

    if (row >= result->row_count || column >= result->column_count) {
        return 0;
    }

    /* Rows were checked when they were stored, so this walk stays inside them. */
    data = result->rows + result->row_starts_end[-1 - (long)row];

    for (i = 0;; ++i) {
        unsigned long length = pg_read_u32(data + offset);

        offset += 4;

        if (i == column) {
            value->data = length == PG_NULL_LENGTH ? NULL : data + offset;
            value->length = length == PG_NULL_LENGTH ? 0 : (size_t)length;
            return 1;
        }

        if (length != PG_NULL_LENGTH) {
            offset += length;
        }
    }
}
