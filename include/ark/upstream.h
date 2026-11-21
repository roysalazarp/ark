#ifndef ARK_UPSTREAM_H
#define ARK_UPSTREAM_H

/*
 * Shares the outbound connection ids between clients (databases, APIs...).
 * Each client asks for an id here, and the app uses ark_upstream_owner to pass
 * each outbound event to the client that owns it.
 *
 * Owners are small numbers the app picks, not function pointers, because a
 * function address would not survive the app being reloaded. 0 means free.
 */

#include <ark/platform.h>

typedef struct {
    int owners[ARK_MAX_UPSTREAMS];
} ArkUpstreams;

/* Returns NULL if no endpoint has that name. */
const ArkEndpoint *ark_find_endpoint(const ArkConfig *, const char *name);

/*
 * Asks the platform to connect to the endpoint. Returns 0 if every id is in
 * use. The id stays taken until ark_upstream_release.
 */
int ark_upstream_open(ArkUpstreams *, ArkOutput *, const ArkEndpoint *, int owner, unsigned long *conn_id);

/* Returns 0 for ids that are free or not outbound. */
int ark_upstream_owner(const ArkUpstreams *, unsigned long conn_id);

/*
 * Returns the owner of an outbound event's id, or 0. On a CLOSE it also frees
 * the id, so it can't be forgotten: pass the event to the owner right after.
 * The platform never hands a freed id to a new connection before this update
 * ends, so the owner still sees its own CLOSE.
 */
int ark_upstream_route(ArkUpstreams *, const ArkEvent *);

/* The same freeing, for apps that route events themselves. */
void ark_upstream_release(ArkUpstreams *, unsigned long conn_id);

#endif
