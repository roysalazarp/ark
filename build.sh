#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

DEBUG_DIR="debug"
LIB_DIR="$DEBUG_DIR/ArkSDK/lib"

mkdir -p "$LIB_DIR"

mapfile -t SOURCES < <(find src -type f -name '*.cpp' | sort)

if ((${#SOURCES[@]} == 0)); then
    echo "ERROR: No .cpp files found in src/"
    exit 1
fi

if [[ ! -f test/main.cpp ]]; then
    echo "ERROR: test/main.cpp not found"
    exit 1
fi

echo "Building libark.so..."
g++ -std=c++17 -Wall -Wextra -Wpedantic \
    -g -O0 -fPIC -shared -Iinclude \
    "${SOURCES[@]}" \
    -o "$LIB_DIR/libark.so"

echo "Building server..."
g++ -std=c++17 -Wall -Wextra -Wpedantic \
    -g -O0 -Iinclude test/main.cpp \
    -L"$LIB_DIR" -lark \
    -Wl,-rpath,'$ORIGIN/ArkSDK/lib' \
    -o "$DEBUG_DIR/server"

echo "Build successful!"
