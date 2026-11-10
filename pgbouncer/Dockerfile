FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends pgbouncer \
    && rm -rf /var/lib/apt/lists/*

USER postgres
CMD ["pgbouncer", "/etc/pgbouncer/pgbouncer.ini"]
