/*
 * Adapted for Ark from redbean's HTTP parser, part of Cosmopolitan:
 * https://github.com/jart/cosmopolitan (net/http/)
 *
 * Copyright 2020 Justine Alexandra Roberts Tunney
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <ark/memory.h>

#include <ark/http_parser.h>

/* Offsets are shorts, so this is the most a message can use. */
#define HTTP_MAX_MESSAGE 32767

#define HTTP_STATE_START   0
#define HTTP_STATE_METHOD  1
#define HTTP_STATE_URI     2
#define HTTP_STATE_VERSION 3
#define HTTP_STATE_STATUS  4
#define HTTP_STATE_MESSAGE 5
#define HTTP_STATE_NAME    6
#define HTTP_STATE_COLON   7
#define HTTP_STATE_VALUE   8
#define HTTP_STATE_CR      9
#define HTTP_STATE_LF1     10
#define HTTP_STATE_LF2     11

/*
 * Characters allowed in a token, like a method or header name (RFC 2616 § 2.2):
 * any ASCII character except controls and the separators ()<>@,;:\"/[]?={}
 * plus space and tab.
 */
static const char http_token[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x00 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x10 */
    0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0,  /* 0x20 */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 0x30 */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 0x40 */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,  /* 0x50 */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 0x60 */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0,  /* 0x70 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x80 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x90 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0xa0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0xb0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0xc0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0xd0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0xe0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0xf0 */
};

/*
 * Standard headers that are comma-separated lists, so they may appear more
 * than once (RFC 7230 § 4.2): "Allow: GET" and "Allow: POST" mean
 * "Allow: GET, POST". The first goes in headers, the rest in extra. Any other
 * known header appearing twice is rejected.
 */
static int http_is_repeatable(int h) {
    switch (h) {
    case ARK_HTTP_HEADER_ACCEPT_CHARSET:
    case ARK_HTTP_HEADER_ACCEPT_ENCODING:
    case ARK_HTTP_HEADER_ACCEPT_LANGUAGE:
    case ARK_HTTP_HEADER_ACCEPT:
    case ARK_HTTP_HEADER_ALLOW:
    case ARK_HTTP_HEADER_CACHE_CONTROL:
    case ARK_HTTP_HEADER_CONTENT_ENCODING:
    case ARK_HTTP_HEADER_CONTENT_LANGUAGE:
    case ARK_HTTP_HEADER_EXPECT:
    case ARK_HTTP_HEADER_IF_MATCH:
    case ARK_HTTP_HEADER_IF_NONE_MATCH:
    case ARK_HTTP_HEADER_PRAGMA:
    case ARK_HTTP_HEADER_PROXY_AUTHENTICATE:
    case ARK_HTTP_HEADER_PUBLIC:
    case ARK_HTTP_HEADER_TE:
    case ARK_HTTP_HEADER_TRAILER:
    case ARK_HTTP_HEADER_TRANSFER_ENCODING:
    case ARK_HTTP_HEADER_UPGRADE:
    case ARK_HTTP_HEADER_VARY:
    case ARK_HTTP_HEADER_VIA:
    case ARK_HTTP_HEADER_WARNING:
    case ARK_HTTP_HEADER_WWW_AUTHENTICATE:
    case ARK_HTTP_HEADER_X_FORWARDED_FOR:
    case ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_HEADERS:
    case ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_METHODS:
    case ARK_HTTP_HEADER_ACCESS_CONTROL_REQUEST_HEADERS:
    case ARK_HTTP_HEADER_ACCESS_CONTROL_REQUEST_METHODS:
        return 1;
    default:
        return 0;
    }
}

/* C0 and C1 control characters. */
static int http_is_control(int ch) {
    return ch < 0x20 || (0x7F <= ch && ch < 0xA0);
}

static int http_is_digit(int ch) {
    return ch >= '0' && ch <= '9';
}

void ark_http_message_init(ArkHttpMessage *r, int type) {
    ark_memory_zero(r, sizeof(*r));
    r->type = (unsigned char)type;
}

/*
 * Saves a header. Known ones go in headers by id, the rest in extra.
 * Returns 0 if the message must be rejected.
 */
static int http_save_header(ArkHttpMessage *r, const unsigned char *p, int value_end) {
    int h = ark_http_header_id(p + r->k.a, (size_t)(r->k.b - r->k.a));

    if (h != -1 && !r->headers[h].a) {
        r->headers[h].a = (short)r->a;
        r->headers[h].b = (short)value_end;
        return 1;
    }

    /* Ark: a header that can't repeat appearing twice is rejected. */
    if (h != -1 && !http_is_repeatable(h)) {
        return 0;
    }

    if (r->extra_count == ARK_HTTP_MAX_EXTRA_HEADERS) {
        r->too_many_headers = 1;
        return 0;
    }

    r->extra[r->extra_count].k = r->k;
    r->extra[r->extra_count].v.a = (short)r->a;
    r->extra[r->extra_count].v.b = (short)value_end;
    ++r->extra_count;

    return 1;
}

/*
 * Each byte is looked at once, even when the message arrives in pieces: the
 * state is kept in r between calls.
 *
 * The inner loops read ahead within a piece. Ark fix: they stop on the last
 * byte that has arrived rather than one past it. In redbean they step past
 * it, and the loop's own ++ then skips the first byte of the next piece, so
 * a message split in the middle of a word could lose a byte.
 */
#define HTTP_NEXT_OR_STOP()               \
    if ((size_t)r->i + 1 == n) {          \
        break;                            \
    }                                     \
    ch = p[++r->i]

int ark_http_parse(ArkHttpMessage *r, const unsigned char *p, size_t n, size_t c) {
    int ch;
    int i;

    if (n > c) {
        return -1;
    }

    n = n > HTTP_MAX_MESSAGE ? HTTP_MAX_MESSAGE : n;
    c = c > HTTP_MAX_MESSAGE ? HTTP_MAX_MESSAGE : c;

    for (; (size_t)r->i < n; ++r->i) {
        ch = p[r->i];

        switch (r->t) {
        case HTTP_STATE_START:
            /* Empty lines before the start line are skipped (RFC 7230 § 3.5). */
            if (ch == '\r' || ch == '\n') {
                break;
            }
            if (!http_token[ch]) {
                return -1;
            }
            if (r->type == ARK_HTTP_PARSE_REQUEST) {
                r->t = HTTP_STATE_METHOD;
                r->method.a = (short)r->i;
            } else {
                r->t = HTTP_STATE_VERSION;
                r->a = r->i;
            }
            break;

        case HTTP_STATE_METHOD:
            for (;;) {
                if (ch == ' ') {
                    r->method.b = (short)r->i;
                    r->a = r->i + 1;
                    r->t = HTTP_STATE_URI;
                    break;
                } else if (!http_token[ch]) {
                    return -1;
                }
                HTTP_NEXT_OR_STOP();
            }
            break;

        case HTTP_STATE_URI:
            for (;;) {
                if (ch == ' ' || ch == '\r' || ch == '\n') {
                    if (r->i == r->a || ch == '\n') {
                        return -1;
                    }
                    r->uri.a = (short)r->a;
                    r->uri.b = (short)r->i;
                    if (ch == ' ') {
                        r->a = r->i + 1;
                        r->t = HTTP_STATE_VERSION;
                    } else {
                        r->version = 9;
                        r->t = HTTP_STATE_CR;
                    }
                    break;
                } else if (http_is_control(ch)) {
                    return -1;
                }
                HTTP_NEXT_OR_STOP();
            }
            break;

        case HTTP_STATE_VERSION:
            if (ch == ' ' || ch == '\r' || ch == '\n') {
                /* "HTTP/" then digit, dot, digit. */
                if (ch == '\n' || r->i - r->a != 8 ||
                    p[r->a] != 'H' || p[r->a + 1] != 'T' || p[r->a + 2] != 'T' || p[r->a + 3] != 'P' ||
                    p[r->a + 4] != '/' || !http_is_digit(p[r->a + 5]) || p[r->a + 6] != '.' ||
                    !http_is_digit(p[r->a + 7])) {
                    return -1;
                }
                r->version = (unsigned char)((p[r->a + 5] - '0') * 10 + (p[r->a + 7] - '0'));
                if (r->type == ARK_HTTP_PARSE_REQUEST) {
                    /* A request line ends right after the version. */
                    if (ch != '\r') {
                        return -1;
                    }
                    r->t = HTTP_STATE_CR;
                } else {
                    if (ch != ' ') {
                        return -1;
                    }
                    r->t = HTTP_STATE_STATUS;
                }
            }
            break;

        case HTTP_STATE_STATUS:
            for (;;) {
                if (ch == ' ' || ch == '\r' || ch == '\n') {
                    if (r->status < 100 || ch == '\n') {
                        return -1;
                    }
                    if (ch == ' ') {
                        r->a = r->i + 1;
                        r->t = HTTP_STATE_MESSAGE;
                    } else {
                        r->t = HTTP_STATE_CR;
                    }
                    break;
                } else if (http_is_digit(ch)) {
                    r->status = r->status * 10 + (ch - '0');
                    if (r->status > 999) {
                        return -1;
                    }
                } else {
                    return -1;
                }
                HTTP_NEXT_OR_STOP();
            }
            break;

        case HTTP_STATE_MESSAGE:
            for (;;) {
                if (ch == '\r' || ch == '\n') {
                    if (ch == '\n') {
                        return -1;
                    }
                    r->message.a = (short)r->a;
                    r->message.b = (short)r->i;
                    r->t = HTTP_STATE_CR;
                    break;
                } else if (http_is_control(ch)) {
                    return -1;
                }
                HTTP_NEXT_OR_STOP();
            }
            break;

        case HTTP_STATE_CR:
            if (ch != '\n') {
                return -1;
            }
            r->t = HTTP_STATE_LF1;
            break;

        case HTTP_STATE_LF1:
            if (ch == '\r') {
                r->t = HTTP_STATE_LF2;
                break;
            }
            /*
             * No bare LF (Ark), no empty header name (RFC 2616 § 2.2) and no
             * line folding (RFC 7230 § 3.2.4).
             */
            if (ch == '\n' || !http_token[ch]) {
                return -1;
            }
            r->k.a = (short)r->i;
            r->t = HTTP_STATE_NAME;
            break;

        case HTTP_STATE_NAME:
            for (;;) {
                if (ch == ':') {
                    r->k.b = (short)r->i;
                    r->t = HTTP_STATE_COLON;
                    break;
                } else if (!http_token[ch]) {
                    return -1;
                }
                HTTP_NEXT_OR_STOP();
            }
            break;

        case HTTP_STATE_COLON:
            if (ch == ' ' || ch == '\t') {
                break;
            }
            r->a = r->i;
            r->t = HTTP_STATE_VALUE;
            /* fall through */

        case HTTP_STATE_VALUE:
            for (;;) {
                if (ch == '\r' || ch == '\n') {
                    if (ch == '\n') {
                        return -1;
                    }
                    /* Trailing spaces and tabs aren't part of the value. */
                    i = r->i;
                    while (i > r->a && (p[i - 1] == ' ' || p[i - 1] == '\t')) {
                        --i;
                    }
                    if (!http_save_header(r, p, i)) {
                        return -1;
                    }
                    r->t = HTTP_STATE_CR;
                    break;
                } else if ((ch < 0x20 && ch != '\t') || (0x7F <= ch && ch < 0xA0)) {
                    return -1;
                }
                HTTP_NEXT_OR_STOP();
            }
            break;

        case HTTP_STATE_LF2:
            if (ch == '\n') {
                return ++r->i;
            }
            return -1;

        default:
            return -1;
        }
    }

    /* Not complete yet. If the buffer is already full, it never will be. */
    if ((size_t)r->i < c) {
        return 0;
    }
    return -1;
}

static unsigned char http_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Whether length bytes equal the null-terminated name, ignoring ASCII case. */
static int http_name_equal(const unsigned char *bytes, size_t length, const char *name) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (name[i] == '\0' || http_lower(bytes[i]) != http_lower((unsigned char)name[i])) {
            return 0;
        }
    }

    return name[length] == '\0';
}

int ark_http_find_header(const ArkHttpMessage *message, const unsigned char *data, const char *name, ArkBytes *value) {
    size_t name_length = 0;
    int id;
    size_t i;

    while (name[name_length] != '\0') {
        ++name_length;
    }

    /* Known headers are found by id. The first of a repeated one is always there. */
    id = ark_http_header_id((const unsigned char *)name, name_length);
    if (id != -1) {
        if (!message->headers[id].a) {
            return 0;
        }

        value->data = data + message->headers[id].a;
        value->length = (size_t)(message->headers[id].b - message->headers[id].a);
        return 1;
    }

    for (i = 0; i < message->extra_count; ++i) {
        const ArkHttpHeaderEntry *header = &message->extra[i];

        if (http_name_equal(data + header->k.a, (size_t)(header->k.b - header->k.a), name)) {
            value->data = data + header->v.a;
            value->length = (size_t)(header->v.b - header->v.a);
            return 1;
        }
    }

    return 0;
}
