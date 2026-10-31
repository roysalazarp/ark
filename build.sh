#!/usr/bin/env bash
# Usage: ./build.sh        build the app and the platform layer
#        ./build.sh app    rebuild only the app (a running server hot-reloads it)
set -euo pipefail

cd "$(dirname "$0")"

PLATFORM="linux"
DEBUG_DIR="debug"
LIB_DIR="$DEBUG_DIR/ArkSDK/lib"
TARGET="${1:-all}"

mkdir -p "$LIB_DIR"

CFLAGS=(-std=c89 -Wall -Wextra -Wpedantic -g -O0 -Iinclude)

# The app is freestanding: only the compiler's own headers (stddef.h etc.),
# no libc headers, and linking fails if it references any symbol it does not define.
APP_FLAGS=(-ffreestanding -nostdinc -isystem "$(gcc -print-file-name=include)"
           -fPIC -shared -nostdlib -Wl,--no-undefined)

mapfile -t APP_SOURCES < <(find src -path src/platform -prune -o -type f -name '*.c' -print | sort)
mapfile -t PLATFORM_SOURCES < <(find "src/platform/$PLATFORM" -type f -name '*.c' | sort)

if ((${#APP_SOURCES[@]} == 0)); then
    echo "ERROR: No app .c files found in src/"
    exit 1
fi

if ((${#PLATFORM_SOURCES[@]} == 0)); then
    echo "ERROR: No .c files found in src/platform/$PLATFORM/"
    exit 1
fi

echo "Building libark.so..."
gcc "${CFLAGS[@]}" "${APP_FLAGS[@]}" "${APP_SOURCES[@]}" -o "$LIB_DIR/libark.so.tmp"
# Rename so a running server never sees a half-written library.
mv -f "$LIB_DIR/libark.so.tmp" "$LIB_DIR/libark.so"

if [[ "$TARGET" != "app" ]]; then
    echo "Building server ($PLATFORM)..."
    gcc "${CFLAGS[@]}" "${PLATFORM_SOURCES[@]}" -ldl -o "$DEBUG_DIR/server"
fi

echo "Build successful!"
