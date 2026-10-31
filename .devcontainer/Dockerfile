# Start with the Debian 12 (bookworm) image
FROM mcr.microsoft.com/devcontainers/base:bookworm

RUN apt-get update && apt-get upgrade -y

RUN apt-get install -y \
    build-essential \
    libargon2-1 \
    libargon2-dev \
    uuid-dev \
    libssl-dev \
    sqlite3 \
    libsqlite3-dev \
    gdb \
    valgrind \
    wrk \
    nodejs \
    netcat-openbsd \
    && apt-get clean
