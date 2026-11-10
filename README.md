# ARK

A C89 web server library, split into a platform layer and an app.

- **Platform layer** (`src/platform/<os>/`, built as `server`) owns `main`,
  memory, sockets and the event loop. It loads the app at runtime, calls it once
  per tick, and carries out what the app asks for.
- **App** (everything else in `src/`, built as `ArkSDK/lib/libark.so`) is
  freestanding C89: no libc, no OS calls. It talks to the platform only through
  the structs in `include/ark/platform.h`, and uses Ark's own modules
  (`memory`, `text`, `http`, `upstream`, `postgres`).

`include/ark/platform.h` is the whole contract. Porting to another OS means
writing one new platform layer that fills in those structs; the app stays the same.

The build keeps the app free of libc: it compiles without the system headers
and fails to link if the app calls any function it doesn't define itself.

## Build and run

    ./build.sh            # debug build (-O0) into debug/
    ./build.sh release    # optimized build (-O2) into release/
    ./debug/server        # listens on 8080 (or pass a port)
    ./build.sh app        # rebuild only the debug app; a running server hot-reloads it

Hot reload only works while the app's state structs keep the same layout.
After changing a struct stored in permanent memory, restart the server.

## Test and benchmark

    ./test/http_test.sh [--slow]   # HTTP behaviour; --slow adds the timeout tests
    ./bench/run.sh                 # wrk benchmark against an equivalent Node.js server

## Endpoints

The app connects to other services (databases, APIs) by name. The platform
says where each name is:

    ARK_ENDPOINTS="db=db.internal:6432,payments=127.0.0.1:9443" ./debug/server

The default is `db=127.0.0.1:6432`. Host names are looked up once, at startup,
because a lookup can block; if an address changes, restart the server.

The app does not speak TLS. To reach a service that needs it, such as most
third-party APIs, point the endpoint at a local TLS proxy (for example stunnel)
that connects to the real service, like `payments` above.

## Database

    docker compose up -d           # local Postgres behind PgBouncer (development only)
    curl localhost:8080/slow       # runs a 3 second query and logs the result
    curl localhost:8080/hello/roy  # runs a query with a parameter
    curl localhost:8080/rows/3     # returns several rows and columns
    ./test/db_test.sh              # checks that a slow query doesn't block other requests

The app connects to PgBouncer (the `db` endpoint), which logs in to Postgres
with the password.

### PgBouncer on another machine

PgBouncer doesn't ask the app for a password, so the network has to keep
everyone else out. That's easy on one machine, where it only listens on
`127.0.0.1`. When it runs on a separate machine (as it must when Ark runs on an
OS that can't run PgBouncer), keep it in a private subnet and:

1. **Make PgBouncer listen on its private IP.** In `compose.yml`, change the
   published port from `127.0.0.1:6432:6432` to, for example, `10.0.0.5:6432:6432`.
2. **Point the app at it:**

       ARK_ENDPOINTS="db=10.0.0.5:6432" ./release/server

3. **Allow only the app's machine to reach port 6432**, with a firewall rule
   (a cloud security group, or `ufw` / `nftables` on Linux).
4. **Make PgBouncer check too**, so one firewall mistake doesn't open the
   database. In `pgbouncer.ini`, set `auth_type = hba` and
   `auth_hba_file = /etc/pgbouncer/pg_hba.conf`, and in that file trust only the
   app's IP (mount it in `compose.yml` like `pgbouncer.ini`; anything not
   listed is refused):

       host  ark  ark  10.0.0.4/32  trust

The traffic between the app and PgBouncer is not encrypted, which is fine inside
a private subnet.

## Connections

All clients (databases, APIs) share 128 outbound connections
(`ARK_MAX_UPSTREAMS` in `include/ark/platform.h`). Each client has its own cap,
so one busy client can't take them all and leave the others waiting:

- **Postgres: 20** (`DB_MAX_CONNECTIONS` in `src/app/app.c`), the same as
  `default_pool_size` in `pgbouncer/pgbouncer.ini`. PgBouncer only runs that
  many queries at once, so more would just wait there. Change both together.
- **A third-party API:** start around 10, and stay under the API's own limits.

When Postgres is at its cap, queries wait in a queue of 64 and start in the
order they were asked for. A full queue gets a 503. A query that hasn't finished
15 seconds after it was asked for, waiting included, gets a 504. Keep these in
balance: if the queue holds more than the cap can finish in 15 seconds, the
extra queries just time out.

## The Postgres client

It supports query parameters, queueing, timeouts, and full results:

- **Rows and columns:** read any value with `ark_pg_value(result, row, column, &value)`.
  A NULL value has a NULL `data` pointer, so it's different from empty text.
- **The count:** `rows_affected` is what Postgres reports, like 1 for `UPDATE 1`.
  Use it to answer "not found" when an update or delete hits nothing.
- **Error codes:** `error_code` is Postgres's code, like `23505` (already
  exists). The demo app answers that one with 409.

Limits: a result can be up to 1 MB, so page through big tables with `LIMIT`.
A response the app sends can be up to 64 KB, the platform's output buffer per
connection.

What it can't do yet is transactions.

### Transactions

Transactions are planned for later. Every single query is already
all-or-nothing, and most CRUD routes are one query, so they don't need more. An
explicit transaction is for several changes that must all happen or none, like
an order and its items, or moving money between accounts.

Until the client supports them:

- **Use one query with `WITH`** where it fits. Each part sees the data as it was
  when the query started, and the same row can't be changed twice.

      WITH new_order AS (
          INSERT INTO orders (user_id) VALUES ($1) RETURNING id
      )
      INSERT INTO order_items (order_id, product_id)
      SELECT id, $2 FROM new_order;

- **Use a database function only if there's no other way.** A function runs
  all-or-nothing in one call, so it can do almost anything a transaction can.
  But it puts business logic in the database, and we want that logic in the
  app, where it's easier to read, test and change. Treat functions as a
  stopgap, and move the logic back once the client has transactions.
