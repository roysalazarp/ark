#ifndef ARK_POSTGRES_H
#define ARK_POSTGRES_H

/*
 * A small non-blocking Postgres client, in two tiers.
 *
 * The protocol tier only builds and reads messages: no connections, no
 * memory of its own, nothing kept between calls. Use it to drive a
 * connection you manage yourself.
 *
 * The query tier is built on it and does everything: it opens connections
 * (through upstream.h), queues queries, keeps results and times out slow ones.
 * Starting a query returns an id, and the result shows up later in
 * ark_pg_next_result. There are no callbacks: a function address would not
 * survive the app being reloaded.
 *
 * Neither tier has password support, so the server must not ask for one.
 * Connect through a pooler that logs in for you. See compose.yml.
 */

#include <ark/memory.h>
#include <ark/platform.h>
#include <ark/text.h>
#include <ark/upstream.h>

/* ---- Protocol tier ----------------------------------------------------- */

/*
 * Writers append one message to the buffer and return 0 if it didn't fit.
 * Values are text; a NULL data pointer sends SQL NULL.
 */
int ark_pg_write_startup(ArkBuffer *, const char *user, const char *database);
int ark_pg_write_query(ArkBuffer *, const char *sql, const ArkBytes *params, size_t param_count);
int ark_pg_write_terminate(ArkBuffer *);

/* A message from the server: its type letter, and its body. */
typedef struct {
    unsigned char type;
    ArkBytes body;
} ArkPgMessage;

/*
 * Reads the message at the front of data. Returns how many bytes it takes,
 * 0 if it hasn't all arrived, or -1 if it is invalid.
 */
long ark_pg_read_message(ArkBytes data, ArkPgMessage *message);

/* 'R': whether the server let us in without a password. */
int ark_pg_auth_ok(ArkBytes body);

/* 'T' (row description) or 'D' (data row): how many columns. Returns 0 if invalid. */
int ark_pg_read_column_count(ArkBytes body, size_t *count);

/*
 * 'D': checks the row is well formed, and reads one value. A NULL value has a
 * NULL data pointer. Each returns 0 if the row is invalid or too short.
 */
int ark_pg_check_row(ArkBytes body, size_t *column_count);
int ark_pg_read_row_value(ArkBytes body, size_t column, ArkBytes *value);

/* 'C': the count at the end of the command tag, like 3 for "UPDATE 3". 0 if none. */
unsigned long ark_pg_read_command_count(ArkBytes body);

/* 'E': the five-character error code ("23505" means already exists) and the message. */
int ark_pg_read_error(ArkBytes body, ArkBytes *code, ArkBytes *message);

/* ---- Query tier -------------------------------------------------------- */

/*
 * Queries waiting or running at once. Each running query needs an outbound
 * connection; when none is free, queries wait and start in the order asked.
 */
#define ARK_PG_MAX_QUERIES 64

/*
 * Room for one query's whole result. A bigger result fails, so page through
 * big tables with LIMIT. Each running query gets this much, reserved up front
 * but only backed by real memory as it is used.
 */
#define ARK_PG_RESULT_SIZE (1024UL * 1024UL)

/* Longer error messages are cut to fit. */
#define ARK_PG_MESSAGE_SIZE 256

/*
 * Opaque on purpose: it is the queue and connection bookkeeping, and
 * changing it from outside would break queries in flight.
 */
typedef struct ArkPostgres ArkPostgres;

/* What the query tier needs from the current tick. */
typedef struct {
    ArkOutput *output;
    Memory *scratch;
    ArkUpstreams *upstreams;
    unsigned long now_ms;
} ArkPgContext;

typedef enum {
    ARK_PG_OK,
    ARK_PG_ERROR,
    ARK_PG_TIMEOUT
} ArkPgStatus;

typedef enum {
    ARK_PG_STARTED,
    ARK_PG_BUSY,      /* the queue is full */
    ARK_PG_TOO_LARGE  /* the sql and params do not fit */
} ArkPgStart;

/* Everything a result points to stays valid until the next call into this module. */
typedef struct {
    /* In [0, ARK_PG_MAX_QUERIES). */
    unsigned long query_id;
    ArkPgStatus status;
    unsigned long elapsed_ms;

    /* ARK_PG_OK only. Read the values with ark_pg_value. */
    size_t row_count;
    size_t column_count;

    /*
     * The count Postgres reports for the command: rows changed by INSERT,
     * UPDATE and DELETE, rows returned by SELECT. 0 if it reports none.
     */
    unsigned long rows_affected;

    /*
     * Otherwise: what went wrong, and Postgres's five-character error code.
     * The code is "" when the failure didn't come from Postgres, like a
     * timeout or a lost connection.
     */
    const char *error;
    const char *error_code;

    /*
     * The rows, each kept as the server sent its 'D' body, so the protocol
     * tier can read them too. Row i starts at rows + row_starts_end[-1 - i]
     * and ends where the next one starts, or at rows + rows_length.
     */
    const unsigned char *rows;
    size_t rows_length;
    const size_t *row_starts_end;
} ArkPgResult;

/*
 * Queries connect to the endpoint, and their outbound ids are recorded with
 * owner (see upstream.h). At most max_connections are open at once, so the
 * outbound ids are left for other clients too. Its memory comes from the
 * app's arena. Returns NULL if the arena is full or a name is too long.
 */
ArkPostgres *ark_pg_create(Memory *permanent, const ArkEndpoint *endpoint, int owner, size_t max_connections,
                           const char *user, const char *database);

/*
 * Starts a query. Use $1, $2... in the sql and pass the values as params. Values
 * travel separately from the sql, so they can never run as sql: always use them
 * for user input. The sql and params are copied.
 */
ArkPgStart ark_pg_start(ArkPostgres *, const ArkPgContext *, const char *sql,
                        const ArkBytes *params, size_t param_count, unsigned long *query_id);

/* Pass every event for an outbound id this client owns. */
void ark_pg_handle_event(ArkPostgres *, const ArkPgContext *, const ArkEvent *);

/*
 * Call once per tick, after the events. Starts waiting queries when
 * connections are free, and fails queries that took too long (time spent
 * waiting counts).
 */
void ark_pg_update(ArkPostgres *, const ArkPgContext *);

/*
 * Returns 1 and fills in the result if a query has finished. Call it until it
 * returns 0, after ark_pg_handle_event and after ark_pg_update.
 */
int ark_pg_next_result(ArkPostgres *, ArkPgResult *);

/* The row of a result, as a 'D' body for the protocol tier. Returns 0 if out of range. */
int ark_pg_row(const ArkPgResult *, size_t row, ArkBytes *body);

/* Returns 0 if the row or column is out of range. A NULL value has a NULL data pointer. */
int ark_pg_value(const ArkPgResult *, size_t row, size_t column, ArkBytes *value);

#endif
