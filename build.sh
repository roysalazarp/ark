#!/usr/bin/env bash
# Usage: ./build.sh          debug build (-O0) into debug/
#        ./build.sh app      rebuild only the debug app (a running server hot-reloads it)
#        ./build.sh release  optimized build (-O2) into release/
# Every build also writes the assembly for each source to <out dir>/asm/.
set -euo pipefail

cd "$(dirname "$0")"

PLATFORM="linux"
TARGET="${1:-all}"

if [[ "$TARGET" == "release" ]]; then
    OUT_DIR="release"
    OPTIMIZE=(-O2)
else
    OUT_DIR="debug"
    OPTIMIZE=(-O0)
fi

LIB_DIR="$OUT_DIR/ArkSDK/lib"

mkdir -p "$LIB_DIR"

CFLAGS=(-std=c89 -Wall -Wextra -Wpedantic -g "${OPTIMIZE[@]}" -Iinclude)

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

# Assembly for reading: one .s per source, in $OUT_DIR/asm/, mirroring src/.
# Same flags as the build, minus debug info (which would bury the code), plus
# comments naming the variables. Warnings were already shown by the build.
ASM_DIR="$OUT_DIR/asm"
ASM_FLAGS=(-std=c89 "${OPTIMIZE[@]}" -Iinclude -S -fverbose-asm -w)
APP_ASM_FLAGS=(-ffreestanding -nostdinc -isystem "$(gcc -print-file-name=include)" -fPIC)

# write_asm <source> [extra flags...]
write_asm() {
    local source="$1"
    local out="$ASM_DIR/${source#src/}"
    shift

    mkdir -p "$(dirname "$out")"
    gcc "${ASM_FLAGS[@]}" "$@" "$source" -o "${out%.c}.s"
}

echo "Building libark.so..."
gcc "${CFLAGS[@]}" "${APP_FLAGS[@]}" "${APP_SOURCES[@]}" -o "$LIB_DIR/libark.so.tmp"
# Rename so a running server never sees a half-written library.
mv -f "$LIB_DIR/libark.so.tmp" "$LIB_DIR/libark.so"

for source in "${APP_SOURCES[@]}"; do
    write_asm "$source" "${APP_ASM_FLAGS[@]}"
done

if [[ "$TARGET" != "app" ]]; then
    echo "Building server ($PLATFORM)..."
    gcc "${CFLAGS[@]}" "${PLATFORM_SOURCES[@]}" -ldl -o "$OUT_DIR/server"

    for source in "${PLATFORM_SOURCES[@]}"; do
        write_asm "$source"
    done
fi

echo "Assembly written to $ASM_DIR/"

echo "Build successful!"
