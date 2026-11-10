#include <ark/upstream.h>
#include <ark/text.h>

static int is_outbound(unsigned long conn_id) {
    return conn_id >= ARK_MAX_CONNECTIONS && conn_id < ARK_MAX_CONNECTIONS + ARK_MAX_UPSTREAMS;
}

const ArkEndpoint *ark_find_endpoint(const ArkConfig *config, const char *name) {
    size_t i;

    for (i = 0; i < config->endpoint_count; ++i) {
        if (ark_cstr_equal(config->endpoints[i].name, name)) {
            return &config->endpoints[i];
        }
    }

    return NULL;
}

int ark_upstream_open(ArkUpstreams *upstreams, ArkOutput *output, const ArkEndpoint *endpoint,
                      int owner, unsigned long *conn_id) {
    size_t i;

    if (owner == 0 || output->connect_count >= output->connect_capacity) {
        return 0;
    }

    for (i = 0; i < ARK_MAX_UPSTREAMS; ++i) {
        if (upstreams->owners[i] == 0) {
            ArkConnect *connect = &output->connects[output->connect_count++];

            upstreams->owners[i] = owner;
            *conn_id = ARK_MAX_CONNECTIONS + (unsigned long)i;

            connect->conn_id = *conn_id;
            connect->host = endpoint->host;
            connect->port = endpoint->port;

            return 1;
        }
    }

    return 0;
}

int ark_upstream_owner(const ArkUpstreams *upstreams, unsigned long conn_id) {
    return is_outbound(conn_id) ? upstreams->owners[conn_id - ARK_MAX_CONNECTIONS] : 0;
}

void ark_upstream_release(ArkUpstreams *upstreams, unsigned long conn_id) {
    if (is_outbound(conn_id)) {
        upstreams->owners[conn_id - ARK_MAX_CONNECTIONS] = 0;
    }
}
