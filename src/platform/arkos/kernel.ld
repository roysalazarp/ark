/*
 * Linker script: says where each part of the kernel goes in memory.
 * The boot sector loads the kernel at 0x10000 and jumps to its first byte,
 * so kernel_start (in .text.start) must come first.
 */

ENTRY(kernel_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text.start)
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    .bss : {
        kernel_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        kernel_bss_end = .;
    }

    /* Debug info and notes aren't loaded, so drop the parts we don't need. */
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame) }
}
