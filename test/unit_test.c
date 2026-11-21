/*
 * Unit tests for the lower tiers of Ark's modules, called directly, with no
 * server: the HTTP parser, the Postgres protocol functions, the response
 * writer, upstream routing and the text helpers.
 * Run with ./test/unit_test.sh
 */

#include <stdio.h>
#include <string.h>

#include <ark/http.h>
#include <ark/http_parser.h>
#include <ark/postgres.h>
#include <ark/text.h>
#include <ark/upstream.h>

static int failures = 0;

#define CHECK(name, condition)                                  \
    do {                                                        \
        if (condition) {                                        \
            printf("ok   %s\n", name);                          \
        } else {                                                \
            printf("FAIL %s (line %d)\n", name, __LINE__);      \
            ++failures;                                         \
        }                                                       \
    } while (0)

static int bytes_are(ArkBytes bytes, const char *expected) {
    return bytes.data != NULL && ark_bytes_equal_cstr(bytes, expected);
}

static void test_parser(void) {
    static const char request[] = "GET /items?x=1 HTTP/1.1\r\nHost: example.com\r\nX-Thing: a b\r\n\r\n";
    static const char response[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    ArkHttpMessage message;
    ArkBytes value;
    size_t n;
    int result = 0;

    /* Fed one byte at a time, as if it arrived in pieces. */
    ark_http_message_init(&message, ARK_HTTP_PARSE_REQUEST);
    for (n = 1; n <= sizeof(request) - 1 && result == 0; ++n) {
        result = ark_http_parse(&message, (const unsigned char *)request, n, sizeof(request));
    }
    CHECK("parser: request fed one byte at a time completes", result == (int)(sizeof(request) - 1));
    CHECK("parser: version", message.version == 11);
    CHECK("parser: uri", message.uri.b - message.uri.a == 10 && memcmp(request + message.uri.a, "/items?x=1", 10) == 0);
    CHECK("parser: known header by name", ark_http_find_header(&message, (const unsigned char *)request, "host", &value) &&
                                          bytes_are(value, "example.com"));
    CHECK("parser: unknown header by name", ark_http_find_header(&message, (const unsigned char *)request, "X-THING", &value) &&
                                            bytes_are(value, "a b"));
    CHECK("parser: missing header", !ark_http_find_header(&message, (const unsigned char *)request, "Cookie", &value));

    ark_http_message_init(&message, ARK_HTTP_PARSE_RESPONSE);
    result = ark_http_parse(&message, (const unsigned char *)response, sizeof(response) - 1, sizeof(response));
    CHECK("parser: response", result == (int)(sizeof(response) - 1) && message.status == 404 &&
                              memcmp(response + message.message.a, "Not Found", 9) == 0);

    ark_http_message_init(&message, ARK_HTTP_PARSE_REQUEST);
    CHECK("parser: bare LF rejected", ark_http_parse(&message, (const unsigned char *)"GET / HTTP/1.1\n\n", 16, 64) == -1);
}

static void test_http_write_head(void) {
    unsigned char space[256];
    ArkBuffer buffer;
    static const char expected[] = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n"
                                   "Content-Length: 2\r\nConnection: keep-alive\r\n\r\n";

    ark_buffer_init(&buffer, space, sizeof(space));
    ark_http_write_head(&buffer, "201 Created", "application/json", 2, 1);
    CHECK("http: write_head", buffer.length == sizeof(expected) - 1 && memcmp(space, expected, buffer.length) == 0);
}

/* A 'D' row with three values: "7", NULL, and "". */
static const unsigned char pg_row[] = {
    0, 3,
    0, 0, 0, 1, '7',
    0xff, 0xff, 0xff, 0xff,
    0, 0, 0, 0
};

static void test_postgres_protocol(void) {
    unsigned char space[512];
    ArkBuffer buffer;
    ArkBytes body;
    ArkBytes value;
    ArkBytes code;
    ArkBytes text;
    ArkPgMessage message;
    size_t columns;
    unsigned char framed[sizeof(pg_row) + 5];
    static const unsigned char error[] = "SERROR\0C23505\0Mduplicate key\0";

    ark_buffer_init(&buffer, space, sizeof(space));
    CHECK("pg: write_startup", ark_pg_write_startup(&buffer, "ark", "ark") &&
                               space[0] == 0 && space[3] == buffer.length);

    ark_buffer_init(&buffer, space, sizeof(space));
    value.data = (const unsigned char *)"42";
    value.length = 2;
    CHECK("pg: write_query", ark_pg_write_query(&buffer, "SELECT $1", &value, 1) && space[0] == 'P');

    ark_buffer_init(&buffer, space, 8);
    CHECK("pg: write_query reports when it doesn't fit", !ark_pg_write_query(&buffer, "SELECT 1", NULL, 0));

    framed[0] = 'D';
    framed[1] = 0;
    framed[2] = 0;
    framed[3] = 0;
    framed[4] = (unsigned char)(sizeof(pg_row) + 4);
    memcpy(framed + 5, pg_row, sizeof(pg_row));
    body.data = framed;
    body.length = sizeof(framed) - 1;
    CHECK("pg: read_message waits for the rest", ark_pg_read_message(body, &message) == 0);
    body.length = sizeof(framed);
    CHECK("pg: read_message", ark_pg_read_message(body, &message) == (long)sizeof(framed) && message.type == 'D');

    CHECK("pg: check_row", ark_pg_check_row(message.body, &columns) && columns == 3);
    CHECK("pg: row value", ark_pg_read_row_value(message.body, 0, &value) && bytes_are(value, "7"));
    CHECK("pg: NULL value", ark_pg_read_row_value(message.body, 1, &value) && value.data == NULL);
    CHECK("pg: empty value", ark_pg_read_row_value(message.body, 2, &value) && value.data != NULL && value.length == 0);
    CHECK("pg: column out of range", !ark_pg_read_row_value(message.body, 3, &value));
    body.data = pg_row;
    body.length = sizeof(pg_row) - 1;
    CHECK("pg: short row rejected", !ark_pg_check_row(body, &columns));

    CHECK("pg: command count UPDATE", ark_pg_read_command_count(ark_cstr_bytes("UPDATE 3")) == 3);
    CHECK("pg: command count INSERT", ark_pg_read_command_count(ark_cstr_bytes("INSERT 0 7")) == 7);
    CHECK("pg: command count none", ark_pg_read_command_count(ark_cstr_bytes("BEGIN")) == 0);

    body.data = error;
    body.length = sizeof(error) - 1;
    CHECK("pg: read_error", ark_pg_read_error(body, &code, &text) && bytes_are(code, "23505") &&
                            bytes_are(text, "duplicate key"));
}

static void test_upstream(void) {
    ArkUpstreams upstreams;
    ArkConnect connects[4];
    ArkSend sends[1];
    ArkOutput output;
    ArkEndpoint endpoint;
    ArkEvent event;
    unsigned long id;

    memset(&upstreams, 0, sizeof(upstreams));
    memset(&endpoint, 0, sizeof(endpoint));
    output.sends = sends;
    output.send_count = 0;
    output.send_capacity = 1;
    output.connects = connects;
    output.connect_count = 0;
    output.connect_capacity = 4;

    CHECK("upstream: open", ark_upstream_open(&upstreams, &output, &endpoint, 7, &id) && output.connect_count == 1);

    event.type = ARK_EVENT_DATA;
    event.conn_id = id;
    CHECK("upstream: route finds the owner", ark_upstream_route(&upstreams, &event) == 7);

    event.type = ARK_EVENT_CLOSE;
    CHECK("upstream: route on close still gives the owner", ark_upstream_route(&upstreams, &event) == 7);
    CHECK("upstream: ...and frees the id", ark_upstream_owner(&upstreams, id) == 0);
}

static void test_text(void) {
    unsigned char space[4];
    ArkBuffer buffer;

    CHECK("text: equal", ark_bytes_equal_cstr(ark_cstr_bytes("GET"), "GET"));
    CHECK("text: not equal", !ark_bytes_equal_cstr(ark_cstr_bytes("GET"), "GETS"));
    CHECK("text: ignoring case", ark_bytes_equal_cstr_ci(ark_cstr_bytes("Keep-Alive"), "keep-alive"));

    ark_buffer_init(&buffer, space, sizeof(space));
    ark_buffer_append_cstr(&buffer, "abc");
    ark_buffer_append_cstr(&buffer, "de");
    CHECK("text: a write that doesn't fit is dropped whole", buffer.length == 3 && buffer.overflowed);
}

int main(void) {
    test_parser();
    test_http_write_head();
    test_postgres_protocol();
    test_upstream();
    test_text();

    if (failures > 0) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }

    printf("All tests passed\n");
    return 0;
}
