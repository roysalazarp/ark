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

/* Call after passing on an id's CLOSE event, never before: ids are only reused after it. */
void ark_upstream_release(ArkUpstreams *, unsigned long conn_id);

#endif
