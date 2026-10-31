#include <ark/platform.h>
#include <ark/memory.h>

/*
 * The app. It is loaded by a platform layer and only ever runs inside
 * ark_app_update. No globals: all state lives in memory the platform owns,
 * so it survives the app being reloaded.
 */

/* A whole request (headers and body) must fit in this buffer. */
#define REQUEST_BUFFER_SIZE 8192
#define RESPONSE_BUFFER_SIZE 4096

/* Close a connection that sends nothing between requests for this long. */
#define IDLE_TIMEOUT_MS 5000
/* Give up on a request that is still incomplete this long after its first byte. */
#define REQUEST_TIMEOUT_MS 10000

typedef struct {
    int is_open;
    /* The app has asked the platform to close it; ignore anything else it sends. */
    int closing;
    unsigned long last_active_ms;
    unsigned long request_started_ms;
    size_t request_length;
    unsigned char request[REQUEST_BUFFER_SIZE];
} Connection;

typedef struct {
    Memory permanent;
    Memory transient;
    Connection *connections;
} AppState;

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} Buffer;

typedef enum {
    PARSE_INCOMPLETE,
    PARSE_OK,
    PARSE_BAD_REQUEST,
    PARSE_NOT_IMPLEMENTED,
    PARSE_TOO_LARGE
} ParseResult;

typedef struct {
    const unsigned char *method;
    size_t method_length;
    const unsigned char *path;
    size_t path_length;
    int keep_alive;
    /* Headers plus body: how many bytes of the buffer this request uses. */
    size_t total_length;
} Request;

/* ---- Strings ----------------------------------------------------------- */

static size_t cstr_length(const char *s) {
    size_t length = 0;

    while (s[length] != '\0') {
        ++length;
    }

    return length;
}

static unsigned char ascii_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int bytes_equal_cstr(const unsigned char *bytes, size_t length, const char *s) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (s[i] == '\0' || bytes[i] != (unsigned char)s[i]) {
            return 0;
        }
    }

    return s[length] == '\0';
}

/* s must be lowercase. */
static int bytes_equal_cstr_ci(const unsigned char *bytes, size_t length, const char *s) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (s[i] == '\0' || ascii_lower(bytes[i]) != (unsigned char)s[i]) {
            return 0;
        }
    }

    return s[length] == '\0';
}

static int is_whitespace(unsigned char c) {
    return c == ' ' || c == '\t';
}

/* ---- Output ------------------------------------------------------------ */

static void app_log(ArkAppMemory *memory, const char *message) {
    memory->platform.log(message, cstr_length(message));
}

static void buffer_append(Buffer *buffer, const void *data, size_t length) {
    if (length > buffer->capacity - buffer->length) {
        length = buffer->capacity - buffer->length;
    }

    ark_memory_copy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}

static void buffer_append_cstr(Buffer *buffer, const char *s) {
    buffer_append(buffer, s, cstr_length(s));
}

static void buffer_append_unsigned(Buffer *buffer, unsigned long value) {
    char digits[20];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (count > 0) {
        --count;
        buffer_append(buffer, &digits[count], 1);
    }
}

static void queue_send(ArkAppMemory *memory, ArkOutput *output, unsigned long conn_id,
                       const unsigned char *data, size_t length, int close_after) {
    ArkSend *send;

    if (output->send_count >= output->send_capacity) {
        app_log(memory, "app: output is full, dropping response");
        return;
    }

    send = &output->sends[output->send_count++];
    send->conn_id = conn_id;
    send->data = data;
    send->length = length;
    send->close_after = close_after;
}

static void respond(AppState *state, ArkAppMemory *memory, ArkOutput *output, unsigned long conn_id,
                    const char *status, const char *body, int keep_alive) {
    Buffer buffer;
    size_t body_length = cstr_length(body);

    /* Transient memory lives until the next update, as ArkSend requires. */
    buffer.data = (unsigned char *)ark_memory_push(&state->transient, RESPONSE_BUFFER_SIZE);
    buffer.length = 0;
    buffer.capacity = RESPONSE_BUFFER_SIZE;

    if (buffer.data == NULL) {
        app_log(memory, "app: transient memory is full, dropping response");
        return;
    }

    buffer_append_cstr(&buffer, "HTTP/1.1 ");
    buffer_append_cstr(&buffer, status);
    buffer_append_cstr(&buffer, "\r\nContent-Type: text/plain\r\nContent-Length: ");
    buffer_append_unsigned(&buffer, (unsigned long)body_length);
    buffer_append_cstr(&buffer, keep_alive ? "\r\nConnection: keep-alive\r\n\r\n"
                                           : "\r\nConnection: close\r\n\r\n");
    buffer_append(&buffer, body, body_length);

    queue_send(memory, output, conn_id, buffer.data, buffer.length, !keep_alive);
}

static void close_connection(ArkAppMemory *memory, ArkOutput *output, unsigned long conn_id,
                             Connection *conn) {
    queue_send(memory, output, conn_id, NULL, 0, 1);
    conn->closing = 1;
}

/* Sends an error response, closes the connection and stops reading from it. */
static void fail(AppState *state, ArkAppMemory *memory, ArkOutput *output, unsigned long conn_id,
                 Connection *conn, const char *status) {
    respond(state, memory, output, conn_id, status, status, 0);
    conn->closing = 1;
}

/* ---- Parsing ----------------------------------------------------------- */

/*
 * Finds the "\r\n" ending the line that starts at start. Rejects bare CR,
 * bare LF and NUL, which proxies and servers can disagree about.
 */
static int find_line_end(const unsigned char *data, size_t limit, size_t start, size_t *line_end) {
    size_t i = start;

    while (i < limit && data[i] != '\r') {
        if (data[i] == '\n' || data[i] == '\0') {
            return 0;
        }
        ++i;
    }

    if (i + 1 >= limit || data[i + 1] != '\n') {
        return 0;
    }

    *line_end = i;
    return 1;
}

static int parse_content_length(const unsigned char *value, size_t length, size_t *result) {
    size_t number = 0;
    size_t i;

    if (length == 0) {
        return 0;
    }

    for (i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return 0;
        }

        /* Anything this large is rejected later anyway; just avoid overflow. */
        if (number > REQUEST_BUFFER_SIZE) {
            number = REQUEST_BUFFER_SIZE + 1;
            continue;
        }
        number = number * 10 + (size_t)(value[i] - '0');
    }

    *result = number;
    return 1;
}

/* Looks for "close" and "keep-alive" in a comma-separated Connection value. */
static void parse_connection(const unsigned char *value, size_t length,
                             int *wants_close, int *wants_keep_alive) {
    size_t start = 0;

    while (start < length) {
        size_t end = start;
        size_t token_end;

        while (end < length && value[end] != ',') {
            ++end;
        }

        token_end = end;
        while (start < token_end && is_whitespace(value[start])) {
            ++start;
        }
        while (token_end > start && is_whitespace(value[token_end - 1])) {
            --token_end;
        }

        if (bytes_equal_cstr_ci(value + start, token_end - start, "close")) {
            *wants_close = 1;
        } else if (bytes_equal_cstr_ci(value + start, token_end - start, "keep-alive")) {
            *wants_keep_alive = 1;
        }

        start = end + 1;
    }
}

/*
 * Parses the request at the start of data. Strict on purpose: anything a
 * proxy might read differently (bare LF, folded or spaced header names,
 * duplicate Content-Length, Transfer-Encoding) is rejected, because with
 * keep-alive a framing disagreement lets one request hide inside another.
 */
static ParseResult parse_request(const unsigned char *data, size_t length, Request *request) {
    size_t header_length = 0;
    size_t content_length = 0;
    int has_content_length = 0;
    int wants_close = 0;
    int wants_keep_alive = 0;
    int is_http_1_0;
    size_t line_start;
    size_t line_end;
    size_t i;

    for (i = 0; i + 3 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            header_length = i + 4;
            break;
        }
    }

    if (header_length == 0) {
        return PARSE_INCOMPLETE;
    }

    /* Request line: METHOD SP PATH SP VERSION */
    if (!find_line_end(data, header_length, 0, &line_end)) {
        return PARSE_BAD_REQUEST;
    }

    i = 0;
    while (i < line_end && data[i] != ' ') {
        ++i;
    }
    if (i == 0 || i == line_end) {
        return PARSE_BAD_REQUEST;
    }
    request->method = data;
    request->method_length = i;

    line_start = ++i;
    while (i < line_end && data[i] != ' ') {
        ++i;
    }
    if (i == line_start || i == line_end) {
        return PARSE_BAD_REQUEST;
    }
    request->path = data + line_start;
    request->path_length = i - line_start;

    ++i;
    if (bytes_equal_cstr(data + i, line_end - i, "HTTP/1.1")) {
        is_http_1_0 = 0;
    } else if (bytes_equal_cstr(data + i, line_end - i, "HTTP/1.0")) {
        is_http_1_0 = 1;
    } else {
        return PARSE_BAD_REQUEST;
    }

    /* Header lines, up to the blank line that ends the headers. */
    for (line_start = line_end + 2; line_start < header_length - 2; line_start = line_end + 2) {
        size_t name_end;
        size_t value_start;
        size_t value_end;

        if (!find_line_end(data, header_length, line_start, &line_end)) {
            return PARSE_BAD_REQUEST;
        }

        /* Obsolete line folding. */
        if (is_whitespace(data[line_start])) {
            return PARSE_BAD_REQUEST;
        }

        name_end = line_start;
        while (name_end < line_end && data[name_end] != ':') {
            if (is_whitespace(data[name_end])) {
                return PARSE_BAD_REQUEST;
            }
            ++name_end;
        }
        if (name_end == line_start || name_end == line_end) {
            return PARSE_BAD_REQUEST;
        }

        value_start = name_end + 1;
        value_end = line_end;
        while (value_start < value_end && is_whitespace(data[value_start])) {
            ++value_start;
        }
        while (value_end > value_start && is_whitespace(data[value_end - 1])) {
            --value_end;
        }

        if (bytes_equal_cstr_ci(data + line_start, name_end - line_start, "content-length")) {
            if (has_content_length ||
                !parse_content_length(data + value_start, value_end - value_start, &content_length)) {
                return PARSE_BAD_REQUEST;
            }
            has_content_length = 1;
        } else if (bytes_equal_cstr_ci(data + line_start, name_end - line_start, "transfer-encoding")) {
            return PARSE_NOT_IMPLEMENTED;
        } else if (bytes_equal_cstr_ci(data + line_start, name_end - line_start, "connection")) {
            parse_connection(data + value_start, value_end - value_start, &wants_close, &wants_keep_alive);
        }
    }

    if (content_length > REQUEST_BUFFER_SIZE - header_length) {
        return PARSE_TOO_LARGE;
    }

    request->total_length = header_length + content_length;
    if (length < request->total_length) {
        return PARSE_INCOMPLETE;
    }

    /* HTTP/1.1 keeps the connection open by default; HTTP/1.0 only when asked. */
    request->keep_alive = is_http_1_0 ? (wants_keep_alive && !wants_close) : !wants_close;

    return PARSE_OK;
}

/* ---- Requests ---------------------------------------------------------- */

static void handle_request(AppState *state, ArkAppMemory *memory, ArkOutput *output,
                           unsigned long conn_id, const Request *request) {
    if (!bytes_equal_cstr(request->method, request->method_length, "GET")) {
        respond(state, memory, output, conn_id, "405 Method Not Allowed", "Method Not Allowed\n",
                request->keep_alive);
    } else if (bytes_equal_cstr(request->path, request->path_length, "/")) {
        respond(state, memory, output, conn_id, "200 OK", "Hello from Ark\n", request->keep_alive);
    } else {
        respond(state, memory, output, conn_id, "404 Not Found", "Not Found\n", request->keep_alive);
    }
}

/* Handles every complete request in the buffer, in order. */
static void process_requests(AppState *state, ArkAppMemory *memory, ArkOutput *output,
                             unsigned long conn_id, Connection *conn, unsigned long now_ms) {
    while (!conn->closing && conn->request_length > 0) {
        Request request;
        ParseResult result = parse_request(conn->request, conn->request_length, &request);
        size_t remaining;

        if (result == PARSE_INCOMPLETE) {
            if (conn->request_length == REQUEST_BUFFER_SIZE) {
                fail(state, memory, output, conn_id, conn, "431 Request Header Fields Too Large");
            }
            return;
        }

        if (result == PARSE_BAD_REQUEST) {
            fail(state, memory, output, conn_id, conn, "400 Bad Request");
            return;
        }

        if (result == PARSE_NOT_IMPLEMENTED) {
            fail(state, memory, output, conn_id, conn, "501 Not Implemented");
            return;
        }

        if (result == PARSE_TOO_LARGE) {
            fail(state, memory, output, conn_id, conn, "413 Content Too Large");
            return;
        }

        /* The response is built from the request now, before its bytes are discarded. */
        handle_request(state, memory, output, conn_id, &request);
        if (!request.keep_alive) {
            conn->closing = 1;
        }

        /* Move any pipelined bytes to the front. dest < src, so a forward copy is safe. */
        remaining = conn->request_length - request.total_length;
        ark_memory_copy(conn->request, conn->request + request.total_length, remaining);
        conn->request_length = remaining;
        conn->request_started_ms = now_ms;
    }
}

static void receive(AppState *state, ArkAppMemory *memory, ArkOutput *output,
                    const ArkEvent *event, Connection *conn, unsigned long now_ms) {
    const unsigned char *data = event->data;
    size_t remaining = event->length;

    conn->last_active_ms = now_ms;

    /* A single read can hold more than the buffer when requests are pipelined. */
    while (remaining > 0 && !conn->closing) {
        size_t take = REQUEST_BUFFER_SIZE - conn->request_length;

        if (take > remaining) {
            take = remaining;
        }

        if (conn->request_length == 0) {
            conn->request_started_ms = now_ms;
        }

        ark_memory_copy(conn->request + conn->request_length, data, take);
        conn->request_length += take;
        data += take;
        remaining -= take;

        process_requests(state, memory, output, event->conn_id, conn, now_ms);
    }
}

static void check_timeouts(AppState *state, ArkAppMemory *memory, ArkOutput *output, unsigned long now_ms) {
    unsigned long id;

    for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
        Connection *conn = &state->connections[id];

        if (!conn->is_open || conn->closing) {
            continue;
        }

        if (conn->request_length > 0) {
            if (now_ms - conn->request_started_ms >= REQUEST_TIMEOUT_MS) {
                fail(state, memory, output, id, conn, "408 Request Timeout");
            }
        } else if (now_ms - conn->last_active_ms >= IDLE_TIMEOUT_MS) {
            close_connection(memory, output, id, conn);
        }
    }
}

ARK_APP_UPDATE(ark_app_update) {
    AppState *state = (AppState *)memory->permanent;
    size_t i;

    if (!memory->is_initialized) {
        ark_memory_init(&state->permanent,
                        (unsigned char *)memory->permanent + sizeof(AppState),
                        memory->permanent_size - sizeof(AppState));
        ark_memory_init(&state->transient, memory->transient, memory->transient_size);

        state->connections = (Connection *)ark_memory_push(&state->permanent,
                                                           sizeof(Connection) * ARK_MAX_CONNECTIONS);
        if (state->connections == NULL) {
            app_log(memory, "app: permanent memory is too small");
            return;
        }

        memory->is_initialized = 1;
    }

    ark_memory_reset(&state->transient, state->transient.start);

    for (i = 0; i < input->event_count; ++i) {
        const ArkEvent *event = &input->events[i];
        Connection *conn = &state->connections[event->conn_id];

        switch (event->type) {
        case ARK_EVENT_OPEN:
            conn->is_open = 1;
            conn->closing = 0;
            conn->request_length = 0;
            conn->last_active_ms = input->now_ms;
            break;

        case ARK_EVENT_CLOSE:
            conn->is_open = 0;
            conn->closing = 0;
            conn->request_length = 0;
            break;

        case ARK_EVENT_DATA:
            if (conn->is_open && !conn->closing) {
                receive(state, memory, output, event, conn, input->now_ms);
            }
            break;
        }
    }

    check_timeouts(state, memory, output, input->now_ms);
}
