#!/usr/bin/env bash
# Usage: ./build_os.sh        build the Ark OS disk image into debug/arkos/
#        ./build_os.sh run    build it, then boot it in QEMU (quit: Ctrl-A then X)
#        ./build_os.sh debug  build it, then start QEMU paused, waiting for GDB
#                             on port 1234. Connect from VS Code ("Debug Ark OS")
#                             or from a terminal:
#                               gdb debug/arkos/kernel.elf -ex "target remote :1234"
set -euo pipefail

cd "$(dirname "$0")"

SRC="src/platform/arkos"
OUT="debug/arkos"
mkdir -p "$OUT"

# Freestanding 64-bit kernel: no C library, no OS underneath.
#   -O0                  no optimizing, so a debugger steps line by line
#                        (like build.sh's debug build)
#   -mno-red-zone        interrupts will write below the stack pointer, so the
#                        compiler must not keep data there
#   -mgeneral-regs-only  no SSE/float registers: they are off until we enable them
#   -fno-pic, -static    fixed addresses, set by kernel.ld
#   -fno-tree-loop-distribute-patterns
#                        don't turn loops into memset/memcpy calls; our own
#                        memset is such a loop and would end up calling itself
KERNEL_FLAGS=(-std=c89 -Wall -Wextra -Wpedantic -g -O0 -Iinclude
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
# Same idea as the kernel: build an ELF file with debug info (-g -F dwarf),
# so GDB can show boot.asm lines, then keep only the raw 512 bytes.
# The linker places it at 0x7C00, where the BIOS loads it.
nasm -f elf64 -g -F dwarf -D KERNEL_SECTORS="$KERNEL_SECTORS" "$SRC/boot.asm" -o "$OUT/boot.o"
ld -m elf_x86_64 -Ttext=0x7C00 -e 0x7C00 -z noexecstack "$OUT/boot.o" -o "$OUT/boot.elf"
objcopy -O binary -j .text "$OUT/boot.elf" "$OUT/boot.bin"

if (($(stat -c %s "$OUT/boot.bin") != 512)); then
    echo "ERROR: boot sector is $(stat -c %s "$OUT/boot.bin") bytes, it must be exactly 512"
    exit 1
fi

# Disk image: boot sector, then the kernel, padded to whole sectors.
cat "$OUT/boot.bin" "$OUT/kernel.bin" > "$OUT/ark.img"
truncate -s $(( (1 + KERNEL_SECTORS) * 512 )) "$OUT/ark.img"

echo "Built $OUT/ark.img (kernel: $KERNEL_SECTORS sectors)"

# -nographic: no window; the serial port prints to this terminal.
QEMU=(qemu-system-x86_64 -nographic -m 256M -drive file="$OUT/ark.img",format=raw)

case "${1:-}" in
    run)
        exec "${QEMU[@]}"
        ;;
    debug)
        # -s: listen for GDB on port 1234. -S: don't start the CPU until GDB says so.
        # VS Code waits for this exact line before it connects.
        echo "QEMU is waiting for the debugger on port 1234"
        exec "${QEMU[@]}" -s -S
        ;;
esac
