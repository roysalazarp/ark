#ifndef ARK_PLATFORM_H
#define ARK_PLATFORM_H

/*
 * The contract between a platform layer and the app.
 *
 * The platform layer owns main(), the OS, the event loop and all memory.
 * Every tick it collects what happened into an ArkInput, calls the app's
 * update function, then carries out the work the app left in ArkOutput.
 *
 * The app never calls the OS; it only reaches back through ArkPlatformApi.
 * This header is shared by both sides, so it must stay freestanding.
 */

#include <stddef.h>

#define ARK_MAX_CONNECTIONS 1024
/* Outbound connections use the ids right after the inbound ones. */
#define ARK_MAX_UPSTREAMS 128

typedef struct {
    void (*log)(const char *message, size_t length);
} ArkPlatformApi;

#define ARK_MAX_ENDPOINTS 16
#define ARK_ENDPOINT_NAME_SIZE 32
/* Room for "255.255.255.255". */
#define ARK_ADDRESS_SIZE 16

/*
 * Somewhere the app can connect to, like a database or an API, found by name.
 * The platform has already looked up any host name, so host is always an IPv4
 * address: looking up a name can block, so the app never has to.
 */
typedef struct {
    char name[ARK_ENDPOINT_NAME_SIZE];
    char host[ARK_ADDRESS_SIZE];
    unsigned short port;
} ArkEndpoint;

/* Set up by the platform before the first update. Each platform decides how. */
typedef struct {
    ArkEndpoint endpoints[ARK_MAX_ENDPOINTS];
    size_t endpoint_count;
} ArkConfig;

typedef struct {
    int is_initialized;

    /* Survives across ticks and app reloads. Zeroed by the platform at startup. */
    void *permanent;
    size_t permanent_size;

    /* Scratch space the app may throw away every tick. Zeroed by the platform at startup. */
    void *transient;
    size_t transient_size;

    ArkPlatformApi platform;
    ArkConfig config;
} ArkAppMemory;

typedef enum {
    ARK_EVENT_OPEN,
    ARK_EVENT_DATA,
    ARK_EVENT_CLOSE
} ArkEventType;

/*
 * conn_id is in [0, ARK_MAX_CONNECTIONS) for clients, and in
 * [ARK_MAX_CONNECTIONS, ARK_MAX_CONNECTIONS + ARK_MAX_UPSTREAMS) for outbound
 * connections the app asked for with ArkConnect. An id is never reused before
 * the app has received the CLOSE event for it.
 *
 * For an outbound connection, OPEN means it connected. A connection that
 * could not be made gets CLOSE without an OPEN.
 */
typedef struct {
    ArkEventType type;
    unsigned long conn_id;

    /* ARK_EVENT_DATA only. Valid until the update call returns. */
    const unsigned char *data;
    size_t length;
} ArkEvent;

typedef struct {
    /* Monotonic milliseconds. Only differences are meaningful. */
    unsigned long now_ms;

    const ArkEvent *events;
    size_t event_count;
} ArkInput;

typedef struct {
    unsigned long conn_id;

    /* Must stay valid until the next update call; the platform copies it. */
    const unsigned char *data;
    size_t length;

    /*
     * Close the connection once everything queued for it has been sent.
     * A send with length 0 and close_after set just closes the connection.
     */
    int close_after;
} ArkSend;

/* Open an outbound connection. It never blocks: the result arrives as an event. */
typedef struct {
    /* A free outbound id, chosen by the app. */
    unsigned long conn_id;

    /* An IPv4 address, not a name, because looking up a name can block. */
    const char *host;
    unsigned short port;
} ArkConnect;

typedef struct {
    ArkSend *sends;
    size_t send_count;
    size_t send_capacity;

    ArkConnect *connects;
    size_t connect_count;
    size_t connect_capacity;
} ArkOutput;

#define ARK_APP_UPDATE(name) void name(ArkAppMemory *memory, const ArkInput *input, ArkOutput *output)
typedef ARK_APP_UPDATE(ArkAppUpdate);

#endif
