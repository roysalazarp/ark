#include <ark/http.h>
#include <ark/text.h>

/* Close a client that sends nothing between requests for this long. */
#define IDLE_TIMEOUT_MS 5000
/* Give up on a request that is still incomplete this long after its first byte. */
#define REQUEST_TIMEOUT_MS 10000

typedef struct {
    int is_open;
    /* The module has asked the platform to close it; ignore anything else it sends. */
    int closing;
    /* A handler is running for its request right now, and whether it answered. */
    int handling;
    int answered;
    /*
     * Its answer is waiting on an outbound call (see ark_http_park). Requests
     * behind it are held back, and it isn't closed for being idle.
     */
    int waiting;
    unsigned long wait_serial;
    /* For the request being handled or waited on. */
    int keep_alive;
    unsigned long last_active_ms;
    unsigned long request_started_ms;
    size_t request_length;
    unsigned char request[ARK_HTTP_REQUEST_SIZE];
} HttpConnection;

struct ArkHttp {
    HttpConnection connections[ARK_MAX_CONNECTIONS];
    unsigned long next_wait_serial;
};

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

/* ---- Output ------------------------------------------------------------ */

static void http_log(const ArkHttpContext *context, const char *message) {
    context->platform->log(message, ark_cstr_length(message));
}

static void http_queue_send(const ArkHttpContext *context, unsigned long conn_id,
                            const unsigned char *data, size_t length, int close_after) {
    ArkOutput *output = context->output;
    ArkSend *send;

    if (output->send_count >= output->send_capacity) {
        http_log(context, "http: output is full, dropping response");
        return;
    }

    send = &output->sends[output->send_count++];
    send->conn_id = conn_id;
    send->data = data;
    send->length = length;
    send->close_after = close_after;
}

static void http_send(const ArkHttpContext *context, unsigned long conn_id,
                      const char *status, const char *body, int keep_alive) {
    ArkBuffer buffer;
    size_t body_length = ark_cstr_length(body);
    size_t capacity = body_length + ark_cstr_length(status) + 128;

    /* Scratch memory lives until the next update, as ArkSend requires. */
    if (!ark_buffer_push(&buffer, context->scratch, capacity)) {
        http_log(context, "http: scratch memory is full, dropping response");
        return;
    }

    ark_buffer_append_cstr(&buffer, "HTTP/1.1 ");
    ark_buffer_append_cstr(&buffer, status);
    ark_buffer_append_cstr(&buffer, "\r\nContent-Type: text/plain\r\nContent-Length: ");
    ark_buffer_append_unsigned(&buffer, (unsigned long)body_length);
    ark_buffer_append_cstr(&buffer, keep_alive ? "\r\nConnection: keep-alive\r\n\r\n"
                                               : "\r\nConnection: close\r\n\r\n");
    ark_buffer_append(&buffer, body, body_length);

    http_queue_send(context, conn_id, buffer.data, buffer.length, !keep_alive);
}

static void http_close(const ArkHttpContext *context, unsigned long conn_id, HttpConnection *conn) {
    http_queue_send(context, conn_id, NULL, 0, 1);
    conn->closing = 1;
}

/* Sends an error response, closes the connection and stops reading from it. */
static void http_fail(const ArkHttpContext *context, unsigned long conn_id, HttpConnection *conn,
                      const char *status) {
    http_send(context, conn_id, status, status, 0);
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
        if (number > ARK_HTTP_REQUEST_SIZE) {
            number = ARK_HTTP_REQUEST_SIZE + 1;
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
        while (start < token_end && ark_is_whitespace(value[start])) {
            ++start;
        }
        while (token_end > start && ark_is_whitespace(value[token_end - 1])) {
            --token_end;
        }

        if (ark_bytes_equal_cstr_ci(value + start, token_end - start, "close")) {
            *wants_close = 1;
        } else if (ark_bytes_equal_cstr_ci(value + start, token_end - start, "keep-alive")) {
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
    if (ark_bytes_equal_cstr(data + i, line_end - i, "HTTP/1.1")) {
        is_http_1_0 = 0;
    } else if (ark_bytes_equal_cstr(data + i, line_end - i, "HTTP/1.0")) {
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
        if (ark_is_whitespace(data[line_start])) {
            return PARSE_BAD_REQUEST;
        }

        name_end = line_start;
        while (name_end < line_end && data[name_end] != ':') {
            if (ark_is_whitespace(data[name_end])) {
                return PARSE_BAD_REQUEST;
            }
            ++name_end;
        }
        if (name_end == line_start || name_end == line_end) {
            return PARSE_BAD_REQUEST;
        }

        value_start = name_end + 1;
        value_end = line_end;
        while (value_start < value_end && ark_is_whitespace(data[value_start])) {
            ++value_start;
        }
        while (value_end > value_start && ark_is_whitespace(data[value_end - 1])) {
            --value_end;
        }

        if (ark_bytes_equal_cstr_ci(data + line_start, name_end - line_start, "content-length")) {
            if (has_content_length ||
                !parse_content_length(data + value_start, value_end - value_start, &content_length)) {
                return PARSE_BAD_REQUEST;
            }
            has_content_length = 1;
        } else if (ark_bytes_equal_cstr_ci(data + line_start, name_end - line_start, "transfer-encoding")) {
            return PARSE_NOT_IMPLEMENTED;
        } else if (ark_bytes_equal_cstr_ci(data + line_start, name_end - line_start, "connection")) {
            parse_connection(data + value_start, value_end - value_start, &wants_close, &wants_keep_alive);
        }
    }

    if (content_length > ARK_HTTP_REQUEST_SIZE - header_length) {
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

/* Handles every complete request in the buffer, in order. */
static void http_process_requests(const ArkHttpContext *context, unsigned long conn_id,
                                  HttpConnection *conn, ArkHttpHandler *handler, void *app) {
    while (!conn->closing && !conn->waiting && conn->request_length > 0) {
        Request parsed;
        ArkHttpRequest request;
        ParseResult result = parse_request(conn->request, conn->request_length, &parsed);
        size_t remaining;

        if (result == PARSE_INCOMPLETE) {
            if (conn->request_length == ARK_HTTP_REQUEST_SIZE) {
                http_fail(context, conn_id, conn, "431 Request Header Fields Too Large");
            }
            return;
        }

        if (result == PARSE_BAD_REQUEST) {
            http_fail(context, conn_id, conn, "400 Bad Request");
            return;
        }

        if (result == PARSE_NOT_IMPLEMENTED) {
            http_fail(context, conn_id, conn, "501 Not Implemented");
            return;
        }

        if (result == PARSE_TOO_LARGE) {
            http_fail(context, conn_id, conn, "413 Content Too Large");
            return;
        }

        request.conn_id = conn_id;
        request.method = parsed.method;
        request.method_length = parsed.method_length;
        request.path = parsed.path;
        request.path_length = parsed.path_length;

        /* The handler reads the request now, before its bytes are discarded. */
        conn->keep_alive = parsed.keep_alive;
        conn->handling = 1;
        conn->answered = 0;

        handler(app, &request);

        conn->handling = 0;
        if (!conn->answered) {
            http_log(context, "http: a handler did not answer, sending 500");
            http_send(context, conn_id, "500 Internal Server Error", "No Response\n", conn->keep_alive);
        }

        if (!parsed.keep_alive) {
            conn->closing = 1;
        }

        /* Move any pipelined bytes to the front. dest < src, so a forward copy is safe. */
        remaining = conn->request_length - parsed.total_length;
        ark_memory_copy(conn->request, conn->request + parsed.total_length, remaining);
        conn->request_length = remaining;
        conn->request_started_ms = context->now_ms;
    }
}

static void http_receive(const ArkHttpContext *context, const ArkEvent *event,
                         HttpConnection *conn, ArkHttpHandler *handler, void *app) {
    const unsigned char *data = event->data;
    size_t remaining = event->length;

    conn->last_active_ms = context->now_ms;

    /* A single read can hold more than the buffer when requests are pipelined. */
    while (remaining > 0 && !conn->closing) {
        size_t take = ARK_HTTP_REQUEST_SIZE - conn->request_length;

        if (take > remaining) {
            take = remaining;
        }

        if (take == 0) {
            /* Only possible while waiting on an outbound call with a full buffer. */
            conn->waiting = 0;
            http_fail(context, event->conn_id, conn, "431 Request Header Fields Too Large");
            break;
        }

        if (conn->request_length == 0) {
            conn->request_started_ms = context->now_ms;
        }

        ark_memory_copy(conn->request + conn->request_length, data, take);
        conn->request_length += take;
        data += take;
        remaining -= take;

        http_process_requests(context, event->conn_id, conn, handler, app);
    }
}

/* ---- Interface --------------------------------------------------------- */

ArkHttp *ark_http_create(Memory *permanent) {
    return (ArkHttp *)ark_memory_push(permanent, sizeof(ArkHttp));
}

void ark_http_handle_event(ArkHttp *http, const ArkHttpContext *context, const ArkEvent *event,
                           ArkHttpHandler *handler, void *app) {
    HttpConnection *conn;

    if (event->conn_id >= ARK_MAX_CONNECTIONS) {
        return;
    }

    conn = &http->connections[event->conn_id];

    switch (event->type) {
    case ARK_EVENT_OPEN:
        conn->is_open = 1;
        conn->closing = 0;
        conn->handling = 0;
        conn->waiting = 0;
        conn->request_length = 0;
        conn->last_active_ms = context->now_ms;
        break;

    case ARK_EVENT_CLOSE:
        conn->is_open = 0;
        conn->closing = 0;
        conn->waiting = 0;
        conn->request_length = 0;
        break;

    case ARK_EVENT_DATA:
        if (conn->is_open && !conn->closing) {
            http_receive(context, event, conn, handler, app);
        }
        break;
    }
}

void ark_http_update(ArkHttp *http, const ArkHttpContext *context) {
    unsigned long id;

    for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
        HttpConnection *conn = &http->connections[id];

        if (!conn->is_open || conn->closing || conn->waiting) {
            continue;
        }

        if (conn->request_length > 0) {
            if (context->now_ms - conn->request_started_ms >= REQUEST_TIMEOUT_MS) {
                http_fail(context, id, conn, "408 Request Timeout");
            }
        } else if (context->now_ms - conn->last_active_ms >= IDLE_TIMEOUT_MS) {
            http_close(context, id, conn);
        }
    }
}

void ark_http_respond(ArkHttp *http, const ArkHttpContext *context, unsigned long conn_id,
                      const char *status, const char *body) {
    HttpConnection *conn;

    if (conn_id >= ARK_MAX_CONNECTIONS) {
        return;
    }

    conn = &http->connections[conn_id];
    if (!conn->handling || conn->answered) {
        return;
    }

    conn->answered = 1;
    http_send(context, conn_id, status, body, conn->keep_alive);
}

ArkHttpTicket ark_http_park(ArkHttp *http, unsigned long conn_id) {
    ArkHttpTicket ticket;
    HttpConnection *conn;

    /* Serial 0 never matches, so a ticket that couldn't be given is harmless. */
    ticket.conn_id = conn_id;
    ticket.serial = 0;

    if (conn_id >= ARK_MAX_CONNECTIONS) {
        return ticket;
    }

    conn = &http->connections[conn_id];
    if (!conn->handling || conn->answered) {
        return ticket;
    }

    conn->answered = 1;
    conn->waiting = 1;
    conn->wait_serial = ++http->next_wait_serial;
    ticket.serial = conn->wait_serial;

    return ticket;
}

int ark_http_is_parked(const ArkHttp *http, ArkHttpTicket ticket) {
    const HttpConnection *conn;

    if (ticket.conn_id >= ARK_MAX_CONNECTIONS || ticket.serial == 0) {
        return 0;
    }

    conn = &http->connections[ticket.conn_id];
    return conn->is_open && conn->waiting && conn->wait_serial == ticket.serial;
}

void ark_http_resume(ArkHttp *http, const ArkHttpContext *context, ArkHttpTicket ticket,
                     const char *status, const char *body, ArkHttpHandler *handler, void *app) {
    HttpConnection *conn;

    if (!ark_http_is_parked(http, ticket)) {
        return;
    }

    conn = &http->connections[ticket.conn_id];
    conn->waiting = 0;
    conn->last_active_ms = context->now_ms;
    conn->request_started_ms = context->now_ms;

    http_send(context, ticket.conn_id, status, body, conn->keep_alive);

    /* Requests pipelined behind this one were held back until now. */
    http_process_requests(context, ticket.conn_id, conn, handler, app);
}
