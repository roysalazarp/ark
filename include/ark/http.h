#ifndef ARK_HTTP_H
#define ARK_HTTP_H

/*
 * HTTP/1.1 for the clients connected to the server: reading requests,
 * keep-alive and pipelining, timeouts, and sending responses.
 *
 * The module never calls the app. The app passes in events, then asks for
 * requests one at a time and answers each:
 *
 *     ark_http_handle_event(http, &context, event);
 *     while (ark_http_next_request(http, &context, &request)) {
 *         ark_http_respond_text(http, &context, &request, "200 OK", "Hello\n");
 *     }
 *
 * Answers come in tiers, from simplest to most control: respond_text,
 * respond (any content type), and respond_raw (the app writes the whole
 * response, helped by ark_http_write_head). For lower still, the parser is
 * usable on its own: see http_parser.h.
 */

#include <ark/memory.h>
#include <ark/platform.h>
#include <ark/text.h>
#include <ark/http_parser.h>

/* A whole request (headers and body) must fit in this many bytes. */
#define ARK_HTTP_REQUEST_SIZE 8192

/*
 * Opaque on purpose: it is the module's per-client bookkeeping, and changing
 * it from outside would break requests in flight.
 */
typedef struct ArkHttp ArkHttp;

/* What the module needs from the current tick. */
typedef struct {
    ArkOutput *output;
    /* Responses are built here, so it must last until the next update. */
    Memory *scratch;
    const ArkPlatformApi *platform;
    unsigned long now_ms;
} ArkHttpContext;

/*
 * A request to answer. Everything in it stays valid until it is answered
 * (or parked), which must happen in the same update: a request still
 * unanswered at ark_http_update gets a 500.
 */
typedef struct {
    unsigned long conn_id;
    ArkBytes method;
    ArkBytes path;
    /* Empty unless the request sent a Content-Length. */
    ArkBytes body;
    /* Whether the connection stays open after the answer. */
    int keep_alive;

    /* The parsed request, and the buffer it points into. */
    const ArkHttpMessage *message;
    const unsigned char *data;
} ArkHttpRequest;

/*
 * Says which client to answer when an outbound call finishes. It stops
 * matching if that client leaves, even if a new client gets the same id.
 */
typedef struct {
    unsigned long conn_id;
    unsigned long serial;
} ArkHttpTicket;

/* Returns NULL if the arena is full. The module's memory comes from the app's arena. */
ArkHttp *ark_http_create(Memory *permanent);

/* Pass every event for a client, that is a conn_id below ARK_MAX_CONNECTIONS. */
void ark_http_handle_event(ArkHttp *, const ArkHttpContext *, const ArkEvent *);

/*
 * Returns 1 and fills in request when one is ready to answer. Call it until it
 * returns 0 after each event: bytes that didn't fit in a client's buffer yet
 * are only kept until the update returns.
 */
int ark_http_next_request(ArkHttp *, const ArkHttpContext *, ArkHttpRequest *request);

/*
 * Call once per tick, after the requests. Closes idle clients and requests too
 * slow to arrive, and answers forgotten requests with a 500.
 */
void ark_http_update(ArkHttp *, const ArkHttpContext *);

/* Finds a request header by name, ignoring case. Returns 0 if it isn't there. */
int ark_http_header(const ArkHttpRequest *, const char *name, ArkBytes *value);

/* Answers with a text/plain body. */
void ark_http_respond_text(ArkHttp *, const ArkHttpContext *, const ArkHttpRequest *,
                           const char *status, const char *text);

/* Answers with any body and content type. */
void ark_http_respond(ArkHttp *, const ArkHttpContext *, const ArkHttpRequest *,
                      const char *status, const char *content_type, ArkBytes body);

/*
 * Answers with a whole response the app wrote itself, status line and headers
 * included. It must stay valid until the next update. The connection is
 * closed afterwards unless request->keep_alive.
 */
void ark_http_respond_raw(ArkHttp *, const ArkHttpContext *, const ArkHttpRequest *, ArkBytes response);

/* Writes a status line and headers, for building a response by hand. */
void ark_http_write_head(ArkBuffer *, const char *status, const char *content_type,
                         size_t content_length, int keep_alive);

/*
 * Instead of answering now: the answer depends on an outbound call, like a
 * database query or an API request. Keep the ticket with the call. The
 * request's bytes are released, so copy anything needed first. Requests the
 * client sends meanwhile are held back, and it isn't closed for being idle.
 */
ArkHttpTicket ark_http_park(ArkHttp *, const ArkHttpRequest *);

/* Whether the client is still waiting on this ticket. */
int ark_http_is_parked(const ArkHttp *, ArkHttpTicket);

/*
 * Turns a parked client back into a request to answer, with the respond
 * functions. Only conn_id and keep_alive are set: the original bytes are
 * gone. Returns 0 if the client left.
 */
int ark_http_resume(ArkHttp *, ArkHttpTicket, ArkHttpRequest *request);

#endif
