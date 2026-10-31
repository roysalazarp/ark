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

typedef struct {
    void (*log)(const char *message, size_t length);
} ArkPlatformApi;

typedef struct {
    int is_initialized;

    /* Survives across ticks and app reloads. Zeroed by the platform at startup. */
    void *permanent;
    size_t permanent_size;

    /* Scratch space the app may throw away every tick. Zeroed by the platform at startup. */
    void *transient;
    size_t transient_size;

    ArkPlatformApi platform;
} ArkAppMemory;

typedef enum {
    ARK_EVENT_OPEN,
    ARK_EVENT_DATA,
    ARK_EVENT_CLOSE
} ArkEventType;

/*
 * conn_id is in [0, ARK_MAX_CONNECTIONS). An id is never reused before the
 * app has received the CLOSE event for it.
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

typedef struct {
    ArkSend *sends;
    size_t send_count;
    size_t send_capacity;
} ArkOutput;

#define ARK_APP_UPDATE(name) void name(ArkAppMemory *memory, const ArkInput *input, ArkOutput *output)
typedef ARK_APP_UPDATE(ArkAppUpdate);

#endif
