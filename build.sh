#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

DEBUG_DIR="debug"
LIB_DIR="$DEBUG_DIR/ArkSDK/lib"

mkdir -p "$LIB_DIR"

mapfile -t SOURCES < <(find src -type f -name '*.c' | sort)

if ((${#SOURCES[@]} == 0)); then
    echo "ERROR: No .c files found in src/"
    exit 1
fi

if [[ ! -f test/main.c ]]; then
    echo "ERROR: test/main.c not found"
    exit 1
fi

echo "Building libark.so..."
gcc -std=c89 -Wall -Wextra -Wpedantic \
    -g -O0 -fPIC -shared -Iinclude \
    "${SOURCES[@]}" \
    -o "$LIB_DIR/libark.so"

echo "Building server..."
gcc -std=c89 -Wall -Wextra -Wpedantic \
    -g -O0 -Iinclude test/main.c \
    -L"$LIB_DIR" -lark \
    -Wl,-rpath,'$ORIGIN/ArkSDK/lib' \
    -o "$DEBUG_DIR/server"

echo "Build successful!"
