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

#ifndef ARK_HTTP_PARSER_H
#define ARK_HTTP_PARSER_H

/*
 * The HTTP message parser. The http module uses it for requests; use it
 * directly for anything else, like reading responses from an API. Everything
 * in a message is plain data you can read.
 *
 * Changes from redbean:
 *   - C89 and freestanding: no libc, no malloc, no 64-bit types.
 *   - Extra headers go in a fixed list instead of a growing one. More than
 *     ARK_HTTP_MAX_EXTRA_HEADERS fails, with too_many_headers set.
 *   - Stricter, because a server behind a proxy must read requests exactly
 *     as the proxy does, or one request can hide inside another:
 *       - lines must end with CRLF; a bare LF is rejected
 *       - a header that can't repeat (like Content-Length) appearing
 *         twice is rejected, instead of the last one winning
 *   - The method is a slice of the buffer instead of a packed number.
 */

#include <stddef.h>

#include <ark/platform.h>

#define ARK_HTTP_PARSE_REQUEST  0
#define ARK_HTTP_PARSE_RESPONSE 1

/* Extra headers: unknown ones, and repeats of the repeatable ones. */
#define ARK_HTTP_MAX_EXTRA_HEADERS 32

#define ARK_HTTP_HEADER_HOST                          0
#define ARK_HTTP_HEADER_CACHE_CONTROL                 1
#define ARK_HTTP_HEADER_CONNECTION                    2
#define ARK_HTTP_HEADER_ACCEPT                        3
#define ARK_HTTP_HEADER_ACCEPT_LANGUAGE               4
#define ARK_HTTP_HEADER_ACCEPT_ENCODING               5
#define ARK_HTTP_HEADER_USER_AGENT                    6
#define ARK_HTTP_HEADER_REFERER                       7
#define ARK_HTTP_HEADER_X_FORWARDED_FOR               8
#define ARK_HTTP_HEADER_ORIGIN                        9
#define ARK_HTTP_HEADER_UPGRADE_INSECURE_REQUESTS     10
#define ARK_HTTP_HEADER_PRAGMA                        11
#define ARK_HTTP_HEADER_COOKIE                        12
#define ARK_HTTP_HEADER_DNT                           13
#define ARK_HTTP_HEADER_SEC_GPC                       14
#define ARK_HTTP_HEADER_FROM                          15
#define ARK_HTTP_HEADER_IF_MODIFIED_SINCE             16
#define ARK_HTTP_HEADER_X_REQUESTED_WITH              17
#define ARK_HTTP_HEADER_X_FORWARDED_HOST              18
#define ARK_HTTP_HEADER_X_FORWARDED_PROTO             19
#define ARK_HTTP_HEADER_X_CSRF_TOKEN                  20
#define ARK_HTTP_HEADER_SAVE_DATA                     21
#define ARK_HTTP_HEADER_RANGE                         22
#define ARK_HTTP_HEADER_CONTENT_LENGTH                23
#define ARK_HTTP_HEADER_CONTENT_TYPE                  24
#define ARK_HTTP_HEADER_VARY                          25
#define ARK_HTTP_HEADER_DATE                          26
#define ARK_HTTP_HEADER_SERVER                        27
#define ARK_HTTP_HEADER_EXPIRES                       28
#define ARK_HTTP_HEADER_CONTENT_ENCODING              29
#define ARK_HTTP_HEADER_LAST_MODIFIED                 30
#define ARK_HTTP_HEADER_ETAG                          31
#define ARK_HTTP_HEADER_ALLOW                         32
#define ARK_HTTP_HEADER_CONTENT_RANGE                 33
#define ARK_HTTP_HEADER_ACCEPT_CHARSET                34
#define ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_CREDENTIALS 35
#define ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_HEADERS  36
#define ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_METHODS  37
#define ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN   38
#define ARK_HTTP_HEADER_ACCESS_CONTROL_MAX_AGE        39
#define ARK_HTTP_HEADER_ACCESS_CONTROL_METHOD         40
#define ARK_HTTP_HEADER_ACCESS_CONTROL_REQUEST_HEADERS 41
#define ARK_HTTP_HEADER_ACCESS_CONTROL_REQUEST_METHOD 42
#define ARK_HTTP_HEADER_ACCESS_CONTROL_REQUEST_METHODS 43
#define ARK_HTTP_HEADER_AGE                           44
#define ARK_HTTP_HEADER_AUTHORIZATION                 45
#define ARK_HTTP_HEADER_CONTENT_BASE                  46
#define ARK_HTTP_HEADER_CONTENT_DESCRIPTION           47
#define ARK_HTTP_HEADER_CONTENT_DISPOSITION           48
#define ARK_HTTP_HEADER_CONTENT_LANGUAGE              49
#define ARK_HTTP_HEADER_CONTENT_LOCATION              50
#define ARK_HTTP_HEADER_CONTENT_MD5                   51
#define ARK_HTTP_HEADER_EXPECT                        52
#define ARK_HTTP_HEADER_IF_MATCH                      53
#define ARK_HTTP_HEADER_IF_NONE_MATCH                 54
#define ARK_HTTP_HEADER_IF_RANGE                      55
#define ARK_HTTP_HEADER_IF_UNMODIFIED_SINCE           56
#define ARK_HTTP_HEADER_KEEP_ALIVE                    57
#define ARK_HTTP_HEADER_LINK                          58
#define ARK_HTTP_HEADER_LOCATION                      59
#define ARK_HTTP_HEADER_MAX_FORWARDS                  60
#define ARK_HTTP_HEADER_PROXY_AUTHENTICATE            61
#define ARK_HTTP_HEADER_PROXY_AUTHORIZATION           62
#define ARK_HTTP_HEADER_PROXY_CONNECTION              63
#define ARK_HTTP_HEADER_PUBLIC                        64
#define ARK_HTTP_HEADER_RETRY_AFTER                   65
#define ARK_HTTP_HEADER_TE                            66
#define ARK_HTTP_HEADER_TRAILER                       67
#define ARK_HTTP_HEADER_TRANSFER_ENCODING             68
#define ARK_HTTP_HEADER_UPGRADE                       69
#define ARK_HTTP_HEADER_WARNING                       70
#define ARK_HTTP_HEADER_WWW_AUTHENTICATE              71
#define ARK_HTTP_HEADER_VIA                           72
#define ARK_HTTP_HEADER_STRICT_TRANSPORT_SECURITY     73
#define ARK_HTTP_HEADER_X_FRAME_OPTIONS               74
#define ARK_HTTP_HEADER_X_CONTENT_TYPE_OPTIONS        75
#define ARK_HTTP_HEADER_ALT_SVC                       76
#define ARK_HTTP_HEADER_REFERRER_POLICY               77
#define ARK_HTTP_HEADER_X_XSS_PROTECTION              78
#define ARK_HTTP_HEADER_ACCEPT_RANGES                 79
#define ARK_HTTP_HEADER_SET_COOKIE                    80
#define ARK_HTTP_HEADER_SEC_CH_UA                     81
#define ARK_HTTP_HEADER_SEC_CH_UA_MOBILE              82
#define ARK_HTTP_HEADER_SEC_FETCH_SITE                83
#define ARK_HTTP_HEADER_SEC_FETCH_MODE                84
#define ARK_HTTP_HEADER_SEC_FETCH_USER                85
#define ARK_HTTP_HEADER_SEC_FETCH_DEST                86
#define ARK_HTTP_HEADER_CF_RAY                        87
#define ARK_HTTP_HEADER_CF_VISITOR                    88
#define ARK_HTTP_HEADER_CF_CONNECTING_IP              89
#define ARK_HTTP_HEADER_CF_IPCOUNTRY                  90
#define ARK_HTTP_HEADER_SEC_CH_UA_PLATFORM            91
#define ARK_HTTP_HEADER_CDN_LOOP                      92
#define ARK_HTTP_HEADER_COUNT                         93

/* A piece of the buffer, [a, b). Offsets are shorts, so messages can't exceed 32767 bytes. */
typedef struct {
    short a;
    short b;
} ArkHttpSlice;

typedef struct {
    ArkHttpSlice k;
    ArkHttpSlice v;
} ArkHttpHeaderEntry;

typedef struct {
    /* Parser state: where it is, which state, and where the current piece started. */
    int i;
    int a;
    unsigned char t;

    unsigned char type;
    int too_many_headers;

    /* Requests. */
    ArkHttpSlice method;
    ArkHttpSlice uri;

    /* Responses. */
    int status;
    ArkHttpSlice message;

    /* 10 for HTTP/1.0, 11 for HTTP/1.1, 9 for HTTP/0.9 (no version at all). */
    unsigned char version;

    /* The name of the header being read. */
    ArkHttpSlice k;

    /* Known headers by id. A header is absent when its slice's a is 0. */
    ArkHttpSlice headers[ARK_HTTP_HEADER_COUNT];

    size_t extra_count;
    ArkHttpHeaderEntry extra[ARK_HTTP_MAX_EXTRA_HEADERS];
} ArkHttpMessage;

void ark_http_message_init(ArkHttpMessage *, int type);

/*
 * Parses as much of the message as has arrived. p holds n bytes so far, out
 * of a buffer that can hold c. Call it again with the same buffer when more
 * arrives: it carries on where it stopped. Returns the length of the start
 * line and headers once they're complete, 0 if more is needed, and -1 if the
 * message is invalid or can't fit in c bytes.
 */
int ark_http_parse(ArkHttpMessage *, const unsigned char *p, size_t n, size_t c);

/* The id of a known header name, ignoring case, or -1. Generated by gperf. */
int ark_http_header_id(const unsigned char *name, size_t length);

/*
 * Finds a header in a parsed message by name, ignoring case. data is the buffer
 * the message was parsed from. Returns 0 if it isn't there. For a header sent
 * more than once, it gives the first.
 */
int ark_http_find_header(const ArkHttpMessage *, const unsigned char *data, const char *name, ArkBytes *value);

#endif
