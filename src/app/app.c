#include <ark/platform.h>
#include <ark/memory.h>

/*
 * The app. It is loaded by a platform layer and only ever runs inside
 * ark_app_update. No globals: all state lives in memory the platform owns,
 * so it survives the app being reloaded.
 */

#define REQUEST_BUFFER_SIZE 4096
#define RESPONSE_BUFFER_SIZE 4096

typedef struct {
    size_t request_length;
    int responded;
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

static size_t cstr_length(const char *s) {
    size_t length = 0;

    while (s[length] != '\0') {
        ++length;
    }

    return length;
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

static void respond(AppState *state, ArkAppMemory *memory, ArkOutput *output,
                    unsigned long conn_id, const char *status, const char *body) {
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
    buffer_append_cstr(&buffer, "\r\nConnection: close\r\n\r\n");
    buffer_append(&buffer, body, body_length);

    queue_send(memory, output, conn_id, buffer.data, buffer.length, 1);
}

static int has_complete_headers(const unsigned char *data, size_t length) {
    size_t i;

    for (i = 0; i + 3 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            return 1;
        }
    }

    return 0;
}

static void handle_request(AppState *state, ArkAppMemory *memory, ArkOutput *output,
                           unsigned long conn_id, const Connection *conn) {
    const unsigned char *request = conn->request;
    size_t length = conn->request_length;
    const unsigned char *method = request;
    const unsigned char *path;
    size_t method_length;
    size_t path_length = 0;
    size_t i = 0;

    while (i < length && request[i] != ' ' && request[i] != '\r') {
        ++i;
    }
    method_length = i;

    if (i < length && request[i] == ' ') {
        ++i;
    }
    path = request + i;

    while (i < length && request[i] != ' ' && request[i] != '\r') {
        ++i;
        ++path_length;
    }

    if (!bytes_equal_cstr(method, method_length, "GET")) {
        respond(state, memory, output, conn_id, "405 Method Not Allowed", "Method Not Allowed\n");
    } else if (bytes_equal_cstr(path, path_length, "/")) {
        respond(state, memory, output, conn_id, "200 OK", "Hello from Ark\n");
    } else {
        respond(state, memory, output, conn_id, "404 Not Found", "Not Found\n");
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
        size_t space;

        switch (event->type) {
        case ARK_EVENT_OPEN:
        case ARK_EVENT_CLOSE:
            conn->request_length = 0;
            conn->responded = 0;
            break;

        case ARK_EVENT_DATA:
            if (conn->responded) {
                break;
            }

            space = REQUEST_BUFFER_SIZE - conn->request_length;
            if (event->length > space) {
                respond(state, memory, output, event->conn_id,
                        "431 Request Header Fields Too Large", "Request Too Large\n");
                conn->responded = 1;
                break;
            }

            ark_memory_copy(conn->request + conn->request_length, event->data, event->length);
            conn->request_length += event->length;

            if (has_complete_headers(conn->request, conn->request_length)) {
                handle_request(state, memory, output, event->conn_id, conn);
                conn->responded = 1;
            }
            break;
        }
    }
}
