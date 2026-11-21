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
    /* In the ready list, waiting for ark_http_next_request to look at it. */
    int ready;
    /*
     * A request was handed to the app and isn't answered yet. If it came from
     * ark_http_next_request, its bytes are still at the front of the buffer.
     */
    int answering;
    int answering_in_buffer;
    /*
     * Its answer is waiting on an outbound call (see ark_http_park). Requests
     * behind it are held back, and it isn't closed for being idle.
     */
    int waiting;
    unsigned long wait_serial;
    /* For the request being answered or waited on. */
    int keep_alive;
    unsigned long last_active_ms;
    unsigned long request_started_ms;

    size_t request_length;
    unsigned char request[ARK_HTTP_REQUEST_SIZE];
    /* Bytes from this tick's read that didn't fit yet. Only valid until the update returns. */
    ArkBytes leftover;

    /* The request at the front of the buffer, parsed as its bytes arrive. */
    ArkHttpMessage message;
    /* 0 until its start line and headers are complete. */
    size_t header_length;
    /* Headers plus body. */
    size_t total_length;
} HttpConnection;

/* The parser keeps offsets as shorts. */
typedef char http_request_size_fits_parser[ARK_HTTP_REQUEST_SIZE <= 32767 ? 1 : -1];

struct ArkHttp {
    HttpConnection connections[ARK_MAX_CONNECTIONS];
    /* Clients that may have a request ready, oldest first. */
    unsigned long ready[ARK_MAX_CONNECTIONS];
    size_t ready_start;
    size_t ready_count;
    unsigned long next_wait_serial;
};

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
    send->bytes.data = data;
    send->bytes.length = length;
    send->close_after = close_after;
}

void ark_http_write_head(ArkBuffer *buffer, const char *status, const char *content_type,
                         size_t content_length, int keep_alive) {
    ark_buffer_append_cstr(buffer, "HTTP/1.1 ");
    ark_buffer_append_cstr(buffer, status);
    ark_buffer_append_cstr(buffer, "\r\nContent-Type: ");
    ark_buffer_append_cstr(buffer, content_type);
    ark_buffer_append_cstr(buffer, "\r\nContent-Length: ");
    ark_buffer_append_unsigned(buffer, (unsigned long)content_length);
    ark_buffer_append_cstr(buffer, keep_alive ? "\r\nConnection: keep-alive\r\n\r\n"
                                              : "\r\nConnection: close\r\n\r\n");
}

static void http_send(const ArkHttpContext *context, unsigned long conn_id, const char *status,
                      const char *content_type, ArkBytes body, int keep_alive) {
    ArkBuffer buffer;
    size_t capacity = body.length + ark_cstr_length(status) + ark_cstr_length(content_type) + 128;

    /* Scratch memory lives until the next update, as ArkSend requires. */
    if (!ark_buffer_push(&buffer, context->scratch, capacity)) {
        http_log(context, "http: scratch memory is full, dropping response");
        return;
    }

    ark_http_write_head(&buffer, status, content_type, body.length, keep_alive);
    ark_buffer_append(&buffer, body.data, body.length);

    http_queue_send(context, conn_id, buffer.data, buffer.length, !keep_alive);
}

static void http_close(const ArkHttpContext *context, unsigned long conn_id, HttpConnection *conn) {
    http_queue_send(context, conn_id, NULL, 0, 1);
    conn->closing = 1;
}

/* Sends an error response, closes the connection and stops reading from it. */
static void http_fail(const ArkHttpContext *context, unsigned long conn_id, HttpConnection *conn,
                      const char *status) {
    http_send(context, conn_id, status, "text/plain", ark_cstr_bytes(status), 0);
    conn->closing = 1;
}

/* ---- Parsing ----------------------------------------------------------- */

static int parse_content_length(ArkBytes value, size_t *result) {
    size_t number = 0;
    size_t i;

    if (value.length == 0) {
        return 0;
    }

    for (i = 0; i < value.length; ++i) {
        if (value.data[i] < '0' || value.data[i] > '9') {
            return 0;
        }

        /* Anything this large is rejected later anyway; just avoid overflow. */
        if (number > ARK_HTTP_REQUEST_SIZE) {
            number = ARK_HTTP_REQUEST_SIZE + 1;
            continue;
        }
        number = number * 10 + (size_t)(value.data[i] - '0');
    }

    *result = number;
    return 1;
}
static void parse_connection(ArkBytes value, int *wants_close, int *wants_keep_alive) {
    size_t start = 0;

    while (start < value.length) {
        size_t end = start;
        size_t token_end;
        ArkBytes token;

        while (end < value.length && value.data[end] != ',') {
            ++end;
        }

        token_end = end;
        while (start < token_end && ark_is_whitespace(value.data[start])) {
            ++start;
        }
        while (token_end > start && ark_is_whitespace(value.data[token_end - 1])) {
            --token_end;
        }

        token.data = value.data + start;
        token.length = token_end - start;

        if (ark_bytes_equal_cstr_ci(token, "close")) {
            *wants_close = 1;
        } else if (ark_bytes_equal_cstr_ci(token, "keep-alive")) {
            *wants_keep_alive = 1;
        }

        start = end + 1;
    }
}

/* A known header straight from its slot: no name lookup, as this runs for every request. */
static int http_known_header(const HttpConnection *conn, int id, ArkBytes *value) {
    ArkHttpSlice slice = conn->message.headers[id];

    if (!slice.a) {
        return 0;
    }

    value->data = conn->request + slice.a;
    value->length = (size_t)(slice.b - slice.a);
    return 1;
}

/*
 * Ark's rules on top of the parser, checked once the headers are complete.
 * Strict on purpose: anything a proxy might read differently is rejected,
 * because with keep-alive a disagreement lets one request hide inside
 * another. Returns NULL if the request is fine, or the status to fail with.
 */
static const char *http_check_request(HttpConnection *conn) {
    const ArkHttpMessage *message = &conn->message;
    ArkBytes value;
    size_t content_length = 0;
    int wants_close = 0;
    int wants_keep_alive = 0;

    if (message->version != 10 && message->version != 11) {
        return "400 Bad Request";
    }

    if (message->headers[ARK_HTTP_HEADER_TRANSFER_ENCODING].a) {
        return "501 Not Implemented";
    }

    if (http_known_header(conn, ARK_HTTP_HEADER_CONTENT_LENGTH, &value) &&
        !parse_content_length(value, &content_length)) {
        return "400 Bad Request";
    }

    if (content_length > ARK_HTTP_REQUEST_SIZE - conn->header_length) {
        return "413 Content Too Large";
    }

    if (http_known_header(conn, ARK_HTTP_HEADER_CONNECTION, &value)) {
        parse_connection(value, &wants_close, &wants_keep_alive);
    }

    conn->total_length = conn->header_length + content_length;

    /* HTTP/1.1 keeps the connection open by default; HTTP/1.0 only when asked. */
    conn->keep_alive = message->version == 10 ? (wants_keep_alive && !wants_close) : !wants_close;

    return NULL;
}

/* ---- Requests ---------------------------------------------------------- */

static void http_mark_ready(ArkHttp *http, unsigned long conn_id) {
    HttpConnection *conn = &http->connections[conn_id];

    if (conn->ready) {
        return;
    }

    conn->ready = 1;
    http->ready[(http->ready_start + http->ready_count) % ARK_MAX_CONNECTIONS] = conn_id;
    ++http->ready_count;
}

/* Moves as much of the leftover bytes into the buffer as fits. */
static void http_take_leftover(const ArkHttpContext *context, HttpConnection *conn) {
    size_t take = ARK_HTTP_REQUEST_SIZE - conn->request_length;

    if (take > conn->leftover.length) {
        take = conn->leftover.length;
    }

    if (take == 0) {
        return;
    }

    if (conn->request_length == 0) {
        conn->request_started_ms = context->now_ms;
    }

    ark_memory_copy(conn->request + conn->request_length, conn->leftover.data, take);
    conn->request_length += take;
    conn->leftover.data += take;
    conn->leftover.length -= take;
}

/* Drops the answered request's bytes from the front of the buffer. */
static void http_consume_request(HttpConnection *conn) {
    size_t remaining = conn->request_length - conn->total_length;

    /* dest < src, so a forward copy is safe. */
    ark_memory_copy(conn->request, conn->request + conn->total_length, remaining);
    conn->request_length = remaining;
    conn->answering_in_buffer = 0;

    ark_http_message_init(&conn->message, ARK_HTTP_PARSE_REQUEST);
    conn->header_length = 0;
    conn->total_length = 0;
}

/*
 * Tries to complete the request at the front of the buffer. Returns 1 when
 * it is complete, 0 when more bytes are needed, and -1 when it failed (the
 * client has been answered and is closing).
 */
static int http_read_request(const ArkHttpContext *context, unsigned long conn_id, HttpConnection *conn) {
    http_take_leftover(context, conn);

    if (conn->request_length == 0) {
        return 0;
    }

    if (conn->header_length == 0) {
        int result = ark_http_parse(&conn->message, conn->request, conn->request_length, ARK_HTTP_REQUEST_SIZE);
        const char *error;

        if (result == 0) {
            return 0;
        }

        if (result < 0) {
            http_fail(context, conn_id, conn,
                      conn->message.too_many_headers || conn->request_length == ARK_HTTP_REQUEST_SIZE
                          ? "431 Request Header Fields Too Large"
                          : "400 Bad Request");
            return -1;
        }

        conn->header_length = (size_t)result;

        error = http_check_request(conn);
        if (error != NULL) {
            http_fail(context, conn_id, conn, error);
            return -1;
        }
    }

    /* Wait for the whole body. */
    return conn->request_length >= conn->total_length ? 1 : 0;
}

/* After an answer: release the request and look for the next one. */
static void http_answered(ArkHttp *http, const ArkHttpContext *context, unsigned long conn_id, HttpConnection *conn) {
    conn->answering = 0;

    if (conn->answering_in_buffer) {
        http_consume_request(conn);
    }

    /* The next request's clocks start now. */
    conn->last_active_ms = context->now_ms;
    conn->request_started_ms = context->now_ms;

    if (!conn->keep_alive) {
        conn->closing = 1;
        return;
    }

    if (conn->request_length > 0 || conn->leftover.length > 0) {
        http_mark_ready(http, conn_id);
    }
}

/* Finds the client a request handed to the app belongs to, if it's still waiting for an answer. */
static HttpConnection *http_answering(ArkHttp *http, const ArkHttpRequest *request) {
    HttpConnection *conn;

    if (request->conn_id >= ARK_MAX_CONNECTIONS) {
        return NULL;
    }

    conn = &http->connections[request->conn_id];
    return conn->is_open && conn->answering ? conn : NULL;
}

/* ---- Interface --------------------------------------------------------- */

ArkHttp *ark_http_create(Memory *permanent) {
    return (ArkHttp *)ark_memory_push(permanent, sizeof(ArkHttp));
}

void ark_http_handle_event(ArkHttp *http, const ArkHttpContext *context, const ArkEvent *event) {
    HttpConnection *conn;

    if (event->conn_id >= ARK_MAX_CONNECTIONS) {
        return;
    }

    conn = &http->connections[event->conn_id];

    switch (event->type) {
    case ARK_EVENT_OPEN:
        ark_memory_zero(conn, sizeof(*conn));
        conn->is_open = 1;
        conn->last_active_ms = context->now_ms;
        ark_http_message_init(&conn->message, ARK_HTTP_PARSE_REQUEST);
        break;

    case ARK_EVENT_CLOSE:
        /* Stale entries in the ready list are skipped because it isn't open. */
        conn->is_open = 0;
        conn->answering = 0;
        conn->waiting = 0;
        conn->leftover.length = 0;
        break;

    case ARK_EVENT_DATA:
        if (!conn->is_open || conn->closing) {
            break;
        }

        conn->last_active_ms = context->now_ms;

        if (conn->leftover.length > 0) {
            /* The app didn't take the previous bytes; they can't be kept. */
            http_fail(context, event->conn_id, conn, "431 Request Header Fields Too Large");
            break;
        }

        conn->leftover = event->bytes;
        http_take_leftover(context, conn);
        http_mark_ready(http, event->conn_id);
        break;
    }
}

int ark_http_next_request(ArkHttp *http, const ArkHttpContext *context, ArkHttpRequest *request) {
    while (http->ready_count > 0) {
        unsigned long conn_id = http->ready[http->ready_start];
        HttpConnection *conn = &http->connections[conn_id];

        http->ready_start = (http->ready_start + 1) % ARK_MAX_CONNECTIONS;
        --http->ready_count;
        conn->ready = 0;

        if (!conn->is_open || conn->closing || conn->waiting || conn->answering) {
            continue;
        }

        if (http_read_request(context, conn_id, conn) != 1) {
            continue;
        }

        conn->answering = 1;
        conn->answering_in_buffer = 1;

        request->conn_id = conn_id;
        request->method.data = conn->request + conn->message.method.a;
        request->method.length = (size_t)(conn->message.method.b - conn->message.method.a);
        request->path.data = conn->request + conn->message.uri.a;
        request->path.length = (size_t)(conn->message.uri.b - conn->message.uri.a);
        request->body.data = conn->request + conn->header_length;
        request->body.length = conn->total_length - conn->header_length;
        request->keep_alive = conn->keep_alive;
        request->message = &conn->message;
        request->data = conn->request;

        return 1;
    }

    return 0;
}

void ark_http_update(ArkHttp *http, const ArkHttpContext *context) {
    unsigned long id;

    for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
        HttpConnection *conn = &http->connections[id];

        if (!conn->is_open || conn->closing) {
            continue;
        }

        if (conn->answering) {
            ArkHttpRequest forgotten;

            http_log(context, "http: a request was not answered, sending 500");
            forgotten.conn_id = id;
            ark_http_respond_text(http, context, &forgotten, "500 Internal Server Error", "No Response\n");
            continue;
        }

        if (conn->leftover.length > 0) {
            /* Bytes the app never asked for; the platform reuses their memory after this update. */
            http_fail(context, id, conn, "431 Request Header Fields Too Large");
            continue;
        }

        if (conn->waiting) {
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

int ark_http_header(const ArkHttpRequest *request, const char *name, ArkBytes *value) {
    if (request->message == NULL) {
        return 0;
    }

    return ark_http_find_header(request->message, request->data, name, value);
}

void ark_http_respond(ArkHttp *http, const ArkHttpContext *context, const ArkHttpRequest *request,
                      const char *status, const char *content_type, ArkBytes body) {
    HttpConnection *conn = http_answering(http, request);

    if (conn == NULL) {
        return;
    }

    http_send(context, request->conn_id, status, content_type, body, conn->keep_alive);
    http_answered(http, context, request->conn_id, conn);
}

void ark_http_respond_text(ArkHttp *http, const ArkHttpContext *context, const ArkHttpRequest *request,
                           const char *status, const char *text) {
    ark_http_respond(http, context, request, status, "text/plain", ark_cstr_bytes(text));
}

void ark_http_respond_raw(ArkHttp *http, const ArkHttpContext *context, const ArkHttpRequest *request,
                          ArkBytes response) {
    HttpConnection *conn = http_answering(http, request);

    if (conn == NULL) {
        return;
    }

    http_queue_send(context, request->conn_id, response.data, response.length, !conn->keep_alive);
    http_answered(http, context, request->conn_id, conn);
}

ArkHttpTicket ark_http_park(ArkHttp *http, const ArkHttpRequest *request) {
    HttpConnection *conn = http_answering(http, request);
    ArkHttpTicket ticket;

    /* Serial 0 never matches, so a ticket that couldn't be given is harmless. */
    ticket.conn_id = request->conn_id;
    ticket.serial = 0;

    if (conn == NULL) {
        return ticket;
    }

    conn->answering = 0;
    if (conn->answering_in_buffer) {
        /* The request's bytes go now, so the app must have copied what it needs. */
        http_consume_request(conn);
    }

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

int ark_http_resume(ArkHttp *http, ArkHttpTicket ticket, ArkHttpRequest *request) {
    HttpConnection *conn;

    if (!ark_http_is_parked(http, ticket)) {
        return 0;
    }

    conn = &http->connections[ticket.conn_id];
    conn->waiting = 0;
    conn->answering = 1;

    ark_memory_zero(request, sizeof(*request));
    request->conn_id = ticket.conn_id;
    request->keep_alive = conn->keep_alive;

    return 1;
}
