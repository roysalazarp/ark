#ifndef ARK_HTTP_H
#define ARK_HTTP_H

/*
 * HTTP/1.1 for the clients connected to the server: reading requests,
 * keep-alive and pipelining, timeouts, and sending responses.
 *
 * The app's request handler is passed in on every call that may run it, and
 * is never stored: a stored function address would not survive the app being
 * reloaded.
 */

#include <ark/memory.h>
#include <ark/platform.h>

/* A whole request (headers and body) must fit in this many bytes. */
#define ARK_HTTP_REQUEST_SIZE 8192

typedef struct ArkHttp ArkHttp;

/* What the module needs from the current tick. */
typedef struct {
    ArkOutput *output;
    /* Responses are built here, so it must last until the next update. */
    Memory *scratch;
    const ArkPlatformApi *platform;
    unsigned long now_ms;
} ArkHttpContext;

/* The parts of a request a handler needs. Valid until the handler returns. */
typedef struct {
    unsigned long conn_id;
    const unsigned char *method;
    size_t method_length;
    const unsigned char *path;
    size_t path_length;
} ArkHttpRequest;

/*
 * Says which client to answer when an outbound call finishes. It stops
 * matching if that client leaves, even if a new client gets the same id.
 */
typedef struct {
    unsigned long conn_id;
    unsigned long serial;
} ArkHttpTicket;

/*
 * Handles one request. Before returning, it must answer with ark_http_respond,
 * or park the client with ark_http_park and answer later with ark_http_resume.
 * A request it leaves unanswered gets a 500.
 */
typedef void ArkHttpHandler(void *app, const ArkHttpRequest *request);

/* Returns NULL if the arena is full. */
ArkHttp *ark_http_create(Memory *permanent);

/* Pass every event for a client, that is a conn_id below ARK_MAX_CONNECTIONS. */
void ark_http_handle_event(ArkHttp *, const ArkHttpContext *, const ArkEvent *,
                           ArkHttpHandler *handler, void *app);

/* Call once per tick. Closes idle clients, and requests too slow to arrive. */
void ark_http_update(ArkHttp *, const ArkHttpContext *);

/* From a handler: answers the request with a text body. */
void ark_http_respond(ArkHttp *, const ArkHttpContext *, unsigned long conn_id,
                      const char *status, const char *body);

/*
 * From a handler: the answer depends on an outbound call, like a database
 * query or an API request. Keep the ticket with the call. Requests the client
 * sends meanwhile are held back, and it isn't closed for being idle.
 */
ArkHttpTicket ark_http_park(ArkHttp *, unsigned long conn_id);

/* Whether the client is still waiting on this ticket. */
int ark_http_is_parked(const ArkHttp *, ArkHttpTicket);

/*
 * Answers a parked client, then handles the requests it sent meanwhile. Does
 * nothing if the client left.
 */
void ark_http_resume(ArkHttp *, const ArkHttpContext *, ArkHttpTicket, const char *status, const char *body,
                     ArkHttpHandler *handler, void *app);

#endif
