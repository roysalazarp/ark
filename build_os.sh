#!/usr/bin/env bash
# Usage: ./build_os.sh        build the Ark OS disk image into debug/arkos/
#        ./build_os.sh run    build it, then boot it in QEMU (quit: Ctrl-A then X)
set -euo pipefail

cd "$(dirname "$0")"

SRC="src/platform/arkos"
OUT="debug/arkos"
mkdir -p "$OUT"

# Freestanding 64-bit kernel: no C library, no OS underneath.
#   -mno-red-zone        interrupts will write below the stack pointer, so the
#                        compiler must not keep data there
#   -mgeneral-regs-only  no SSE/float registers: they are off until we enable them
#   -fno-pic, -static    fixed addresses, set by kernel.ld
#   -fno-tree-loop-distribute-patterns
#                        don't turn loops into memset/memcpy calls; our own
#                        memset is such a loop and would end up calling itself
KERNEL_FLAGS=(-std=c89 -Wall -Wextra -Wpedantic -g -O2 -Iinclude
              -fno-tree-loop-distribute-patterns
              -ffreestanding -nostdinc -isystem "$(gcc -print-file-name=include)"
              -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables
              -mno-red-zone -mgeneral-regs-only
              -nostdlib -static -no-pie -Wl,--build-id=none
              # The kernel is loaded as one raw block that is writable and
              # runnable. True, but known: our own page tables will fix it.
              -Wl,--no-warn-rwx-segments
              -T "$SRC/kernel.ld")

echo "Building kernel..."
gcc "${KERNEL_FLAGS[@]}" "$SRC/kernel.c" -o "$OUT/kernel.elf"
# The boot sector can't read ELF files, so keep only the raw bytes.
objcopy -O binary "$OUT/kernel.elf" "$OUT/kernel.bin"

KERNEL_SECTORS=$(( ($(stat -c %s "$OUT/kernel.bin") + 511) / 512 ))
# The kernel goes at 0x10000 and must stay below 0x80000 (BIOS data is above).
if ((KERNEL_SECTORS > 896)); then
    echo "ERROR: kernel is $KERNEL_SECTORS sectors, the boot sector loads at most 896"
    exit 1
fi

echo "Building boot sector..."
nasm -f bin -D KERNEL_SECTORS="$KERNEL_SECTORS" "$SRC/boot.asm" -o "$OUT/boot.bin"

# Disk image: boot sector, then the kernel, padded to whole sectors.
cat "$OUT/boot.bin" "$OUT/kernel.bin" > "$OUT/ark.img"
truncate -s $(( (1 + KERNEL_SECTORS) * 512 )) "$OUT/ark.img"

echo "Built $OUT/ark.img (kernel: $KERNEL_SECTORS sectors)"

if [[ "${1:-}" == "run" ]]; then
    # -nographic: no window; the serial port prints to this terminal.
    exec qemu-system-x86_64 -nographic -m 256M \
        -drive file="$OUT/ark.img",format=raw
fi
