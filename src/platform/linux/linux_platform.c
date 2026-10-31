/*
 * Linux platform layer.
 *
 * Owns main(), memory, sockets and the epoll loop. Loads the app
 * (libark.so) at runtime, calls it once per tick, and reloads it whenever
 * the file changes. Nothing outside this file knows it is running on Linux.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ark/platform.h>

#define KB(value) ((value) * 1024UL)
#define MB(value) (KB(value) * 1024UL)

#define PERMANENT_MEMORY_SIZE MB(64)
#define TRANSIENT_MEMORY_SIZE MB(64)
#define READ_BUFFER_SIZE MB(1)
#define READ_CHUNK_SIZE KB(16)
#define PENDING_OUTPUT_SIZE KB(64)

#define MAX_EPOLL_EVENTS 256
#define MAX_EVENTS_PER_TICK 1024
#define MAX_SENDS_PER_TICK 1024
#define TICK_TIMEOUT_MS 1000

#define DEFAULT_PORT 8080
#define LISTEN_BACKLOG 128
#define LISTEN_ID ARK_MAX_CONNECTIONS

#define PATH_SIZE 4096
#define APP_FILE_NAME "ArkSDK/lib/libark.so"
#define APP_UPDATE_SYMBOL "ark_app_update"

typedef enum {
    SLOT_FREE,
    SLOT_OPEN,
    SLOT_CLOSED,   /* socket closed, app not told yet */
    SLOT_NOTIFIED  /* app got the CLOSE event this tick */
} SlotState;

typedef struct {
    SlotState state;
    int fd;
    int close_after_flush;
    int want_write;
    unsigned char *pending;
    size_t pending_start;
    size_t pending_length;
} Slot;

typedef struct {
    char source_path[PATH_SIZE];
    char copy_dir[PATH_SIZE];
    void *handle;
    ArkAppUpdate *update;
    ino_t ino;
    struct timespec mtime;
    int next_copy;
} LinuxApp;

static volatile sig_atomic_t g_running = 1;
static int g_epoll_fd = -1;
static Slot g_slots[ARK_MAX_CONNECTIONS];

static void linux_log(const char *message, size_t length) {
    fwrite(message, 1, length, stderr);
    fputc('\n', stderr);
}

static void linux_handle_signal(int signal_number) {
    (void)signal_number;
    g_running = 0;
}

/* dest = a + b. Returns 0 if it does not fit. */
static int linux_join(char *dest, size_t dest_size, const char *a, const char *b) {
    size_t a_length = strlen(a);
    size_t b_length = strlen(b);

    if (a_length + b_length + 1 > dest_size) {
        return 0;
    }

    memcpy(dest, a, a_length);
    memcpy(dest + a_length, b, b_length + 1);
    return 1;
}

static void *linux_allocate(size_t size) {
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
}

/* ---- App loading ------------------------------------------------------- */

static int linux_copy_file(const char *from, const char *to) {
    char buffer[KB(16)];
    FILE *in;
    FILE *out;
    size_t count;
    int ok = 1;

    /* Unlink first so the copy gets a fresh inode the loader has never seen. */
    unlink(to);

    in = fopen(from, "rb");
    if (in == NULL) {
        return 0;
    }

    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return 0;
    }

    while ((count = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, count, out) != count) {
            ok = 0;
            break;
        }
    }

    if (ferror(in)) {
        ok = 0;
    }

    fclose(in);
    if (fclose(out) != 0) {
        ok = 0;
    }

    return ok;
}

/*
 * The app is loaded from a copy, alternating between two names, so the
 * build can replace libark.so while the old copy is still in use.
 */
static void linux_load_app(LinuxApp *app) {
    char copy_path[PATH_SIZE];
    struct stat info;
    void *handle;
    void *symbol;
    ArkAppUpdate *update;

    if (stat(app->source_path, &info) != 0) {
        fprintf(stderr, "platform: cannot find %s\n", app->source_path);
        return;
    }

    /* Remember this version even if loading fails, so a bad build is not retried every tick. */
    app->ino = info.st_ino;
    app->mtime = info.st_mtim;

    if (!linux_join(copy_path, sizeof(copy_path), app->copy_dir,
                    app->next_copy ? "/libark_loaded_1.so" : "/libark_loaded_0.so")) {
        fprintf(stderr, "platform: path too long\n");
        return;
    }

    if (!linux_copy_file(app->source_path, copy_path)) {
        fprintf(stderr, "platform: cannot copy %s to %s\n", app->source_path, copy_path);
        return;
    }

    handle = dlopen(copy_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "platform: %s\n", dlerror());
        return;
    }

    symbol = dlsym(handle, APP_UPDATE_SYMBOL);
    if (symbol == NULL) {
        fprintf(stderr, "platform: %s\n", dlerror());
        dlclose(handle);
        return;
    }

    /* ISO C has no cast from object pointer to function pointer; copy the bits. */
    memcpy(&update, &symbol, sizeof(update));

    if (app->handle != NULL) {
        dlclose(app->handle);
    }

    app->handle = handle;
    app->update = update;
    app->next_copy = !app->next_copy;

    fprintf(stderr, "platform: loaded %s\n", copy_path);
}

static void linux_reload_app_if_changed(LinuxApp *app) {
    struct stat info;

    if (stat(app->source_path, &info) != 0) {
        return;
    }

    if (info.st_ino != app->ino ||
        info.st_mtim.tv_sec != app->mtime.tv_sec ||
        info.st_mtim.tv_nsec != app->mtime.tv_nsec) {
        linux_load_app(app);
    }
}

static int linux_init_app_paths(LinuxApp *app) {
    ssize_t length = readlink("/proc/self/exe", app->copy_dir, sizeof(app->copy_dir) - 1);
    char *last_slash;

    if (length <= 0) {
        return 0;
    }
    app->copy_dir[length] = '\0';

    last_slash = strrchr(app->copy_dir, '/');
    if (last_slash == NULL) {
        return 0;
    }
    *last_slash = '\0';

    if (!linux_join(app->source_path, sizeof(app->source_path), app->copy_dir, "/" APP_FILE_NAME)) {
        return 0;
    }

    /* Keep the loaded copies next to libark.so. */
    last_slash = strrchr(app->source_path, '/');
    memcpy(app->copy_dir, app->source_path, (size_t)(last_slash - app->source_path));
    app->copy_dir[last_slash - app->source_path] = '\0';

    return 1;
}

/* ---- Sockets ----------------------------------------------------------- */

static int linux_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int linux_listen(unsigned short port) {
    struct sockaddr_in address;
    int enable = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("platform: socket");
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, LISTEN_BACKLOG) != 0 ||
        !linux_set_nonblocking(fd)) {
        perror("platform: listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void linux_close_slot(Slot *slot) {
    if (slot->state != SLOT_OPEN) {
        return;
    }

    /* Closing the fd also removes it from epoll. */
    close(slot->fd);
    slot->fd = -1;
    slot->state = SLOT_CLOSED;
}

static void linux_watch_slot(Slot *slot, int want_write) {
    struct epoll_event event;

    if (slot->want_write == want_write) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | (want_write ? EPOLLOUT : 0);
    event.data.u64 = (unsigned long)(slot - g_slots);

    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, slot->fd, &event) == 0) {
        slot->want_write = want_write;
    } else {
        linux_close_slot(slot);
    }
}

static void linux_flush_slot(Slot *slot) {
    while (slot->state == SLOT_OPEN && slot->pending_length > 0) {
        ssize_t sent = send(slot->fd, slot->pending + slot->pending_start,
                            slot->pending_length, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            linux_close_slot(slot);
            return;
        }

        slot->pending_start += (size_t)sent;
        slot->pending_length -= (size_t)sent;
    }

    if (slot->state != SLOT_OPEN) {
        return;
    }

    if (slot->pending_length == 0) {
        slot->pending_start = 0;
        if (slot->close_after_flush) {
            linux_close_slot(slot);
            return;
        }
    }

    linux_watch_slot(slot, slot->pending_length > 0);
}

static void linux_queue_send(const ArkSend *item) {
    Slot *slot;

    if (item->conn_id >= ARK_MAX_CONNECTIONS) {
        return;
    }

    slot = &g_slots[item->conn_id];
    if (slot->state != SLOT_OPEN) {
        return;
    }

    if (slot->pending_start + slot->pending_length + item->length > PENDING_OUTPUT_SIZE) {
        memmove(slot->pending, slot->pending + slot->pending_start, slot->pending_length);
        slot->pending_start = 0;
    }

    if (slot->pending_length + item->length > PENDING_OUTPUT_SIZE) {
        fprintf(stderr, "platform: output buffer full on connection %lu, closing\n", item->conn_id);
        linux_close_slot(slot);
        return;
    }

    memcpy(slot->pending + slot->pending_start + slot->pending_length, item->data, item->length);
    slot->pending_length += item->length;

    if (item->close_after) {
        slot->close_after_flush = 1;
    }
}

/* ---- Gathering input --------------------------------------------------- */

static void linux_accept(int listen_fd, ArkEvent *events, size_t *event_count) {
    while (*event_count < MAX_EVENTS_PER_TICK) {
        struct epoll_event watch;
        Slot *slot = NULL;
        unsigned long id;
        int fd = accept(listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
            if (g_slots[id].state == SLOT_FREE) {
                slot = &g_slots[id];
                break;
            }
        }

        memset(&watch, 0, sizeof(watch));
        watch.events = EPOLLIN;
        watch.data.u64 = id;

        if (slot == NULL || !linux_set_nonblocking(fd) ||
            epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &watch) != 0) {
            close(fd);
            continue;
        }

        slot->state = SLOT_OPEN;
        slot->fd = fd;
        slot->close_after_flush = 0;
        slot->want_write = 0;
        slot->pending_start = 0;
        slot->pending_length = 0;

        events[*event_count].type = ARK_EVENT_OPEN;
        events[*event_count].conn_id = id;
        events[*event_count].data = NULL;
        events[*event_count].length = 0;
        ++*event_count;
    }
}

/*
 * One read per connection per tick. epoll is level-triggered, so anything
 * left over (or skipped because a buffer was full) is reported again next tick.
 */
static void linux_read(Slot *slot, unsigned char *read_buffer, size_t *read_used,
                       ArkEvent *events, size_t *event_count) {
    size_t space = READ_BUFFER_SIZE - *read_used;
    ssize_t received;

    if (space > READ_CHUNK_SIZE) {
        space = READ_CHUNK_SIZE;
    }
    if (space == 0 || *event_count >= MAX_EVENTS_PER_TICK) {
        return;
    }

    received = recv(slot->fd, read_buffer + *read_used, space, 0);

    if (received > 0) {
        events[*event_count].type = ARK_EVENT_DATA;
        events[*event_count].conn_id = (unsigned long)(slot - g_slots);
        events[*event_count].data = read_buffer + *read_used;
        events[*event_count].length = (size_t)received;
        ++*event_count;
        *read_used += (size_t)received;
    } else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        linux_close_slot(slot);
    }
}

/* Returns whether closed connections are still waiting to be reported. */
static int linux_report_closes(ArkEvent *events, size_t *event_count) {
    unsigned long id;

    for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
        if (g_slots[id].state != SLOT_CLOSED) {
            continue;
        }
        if (*event_count >= MAX_EVENTS_PER_TICK) {
            return 1;
        }

        events[*event_count].type = ARK_EVENT_CLOSE;
        events[*event_count].conn_id = id;
        events[*event_count].data = NULL;
        events[*event_count].length = 0;
        ++*event_count;

        g_slots[id].state = SLOT_NOTIFIED;
    }

    return 0;
}

static int linux_has_unreported_closes(void) {
    unsigned long id;

    for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
        if (g_slots[id].state == SLOT_CLOSED) {
            return 1;
        }
    }

    return 0;
}

/* ---- Main loop --------------------------------------------------------- */

int main(int argc, char **argv) {
    static ArkEvent events[MAX_EVENTS_PER_TICK];
    static ArkSend sends[MAX_SENDS_PER_TICK];
    struct epoll_event epoll_events[MAX_EPOLL_EVENTS];
    struct epoll_event listen_watch;
    struct sigaction action;
    ArkAppMemory app_memory;
    LinuxApp app;
    unsigned char *read_buffer;
    unsigned char *pending_buffers;
    unsigned char *app_storage;
    int port = DEFAULT_PORT;
    int listen_fd;
    unsigned long id;

    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 1;
        }
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = linux_handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    /* All memory is reserved once, up front. */
    app_storage = (unsigned char *)linux_allocate(PERMANENT_MEMORY_SIZE + TRANSIENT_MEMORY_SIZE);
    read_buffer = (unsigned char *)linux_allocate(READ_BUFFER_SIZE);
    pending_buffers = (unsigned char *)linux_allocate(PENDING_OUTPUT_SIZE * ARK_MAX_CONNECTIONS);
    if (app_storage == NULL || read_buffer == NULL || pending_buffers == NULL) {
        fprintf(stderr, "platform: cannot allocate memory\n");
        return 1;
    }

    memset(&app_memory, 0, sizeof(app_memory));
    app_memory.permanent = app_storage;
    app_memory.permanent_size = PERMANENT_MEMORY_SIZE;
    app_memory.transient = app_storage + PERMANENT_MEMORY_SIZE;
    app_memory.transient_size = TRANSIENT_MEMORY_SIZE;
    app_memory.platform.log = linux_log;

    for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
        g_slots[id].state = SLOT_FREE;
        g_slots[id].fd = -1;
        g_slots[id].pending = pending_buffers + id * PENDING_OUTPUT_SIZE;
    }

    memset(&app, 0, sizeof(app));
    if (!linux_init_app_paths(&app)) {
        fprintf(stderr, "platform: cannot locate the executable directory\n");
        return 1;
    }
    linux_load_app(&app);
    if (app.update == NULL) {
        return 1;
    }

    listen_fd = linux_listen((unsigned short)port);
    if (listen_fd < 0) {
        return 1;
    }

    g_epoll_fd = epoll_create1(0);
    memset(&listen_watch, 0, sizeof(listen_watch));
    listen_watch.events = EPOLLIN;
    listen_watch.data.u64 = LISTEN_ID;
    if (g_epoll_fd < 0 || epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_watch) != 0) {
        perror("platform: epoll");
        return 1;
    }

    fprintf(stderr, "platform: listening on port %d\n", port);

    while (g_running) {
        ArkInput input;
        ArkOutput output;
        size_t event_count = 0;
        size_t read_used = 0;
        size_t i;
        int timeout = linux_has_unreported_closes() ? 0 : TICK_TIMEOUT_MS;
        int ready = epoll_wait(g_epoll_fd, epoll_events, MAX_EPOLL_EVENTS, timeout);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("platform: epoll_wait");
            break;
        }

        linux_reload_app_if_changed(&app);

        for (i = 0; i < (size_t)ready; ++i) {
            unsigned long ready_id = (unsigned long)epoll_events[i].data.u64;
            unsigned int flags = epoll_events[i].events;
            Slot *slot;

            if (ready_id == LISTEN_ID) {
                linux_accept(listen_fd, events, &event_count);
                continue;
            }

            slot = &g_slots[ready_id];
            if (slot->state != SLOT_OPEN) {
                continue;
            }

            if (flags & EPOLLIN) {
                linux_read(slot, read_buffer, &read_used, events, &event_count);
            } else if (flags & (EPOLLERR | EPOLLHUP)) {
                linux_close_slot(slot);
            }

            if ((flags & EPOLLOUT) && slot->state == SLOT_OPEN) {
                linux_flush_slot(slot);
            }
        }

        linux_report_closes(events, &event_count);

        input.events = events;
        input.event_count = event_count;
        output.sends = sends;
        output.send_count = 0;
        output.send_capacity = MAX_SENDS_PER_TICK;

        app.update(&app_memory, &input, &output);

        for (i = 0; i < output.send_count; ++i) {
            linux_queue_send(&output.sends[i]);
        }
        for (i = 0; i < output.send_count; ++i) {
            if (output.sends[i].conn_id < ARK_MAX_CONNECTIONS) {
                linux_flush_slot(&g_slots[output.sends[i].conn_id]);
            }
        }

        /* Ids are only reused once the app has seen the CLOSE. */
        for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
            if (g_slots[id].state == SLOT_NOTIFIED) {
                g_slots[id].state = SLOT_FREE;
            }
        }
    }

    fprintf(stderr, "platform: shutting down\n");

    for (id = 0; id < ARK_MAX_CONNECTIONS; ++id) {
        linux_close_slot(&g_slots[id]);
    }
    close(listen_fd);
    close(g_epoll_fd);
    dlclose(app.handle);

    return 0;
}
