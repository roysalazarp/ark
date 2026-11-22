/*
 * The Ark OS kernel. For now it only prints "hello" on the serial port.
 *
 * Freestanding: no C standard library, only the compiler's own headers.
 * The boot sector loads this at 0x10000, already in 64-bit mode, and calls
 * kernel_start.
 */

#include <stddef.h>

/* --- Talking to hardware ports ---------------------------------------------
 * x86 devices like the serial port are reached through "ports", a separate
 * address space with its own instructions (in/out). C has no way to say that,
 * so these two lines of inline assembly do it. */

static void port_write(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned char port_read(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* --- Serial port (COM1) -----------------------------------------------------
 * The simplest way to get text out of the machine. QEMU shows it in the
 * terminal, and clouds show it in their "serial console". */

#define COM1 0x3F8

/* Register offsets from COM1. */
#define SERIAL_DATA        0  /* Write a byte to send it. */
#define SERIAL_IRQ_ENABLE  1
#define SERIAL_FIFO        2
#define SERIAL_LINE_CTRL   3
#define SERIAL_MODEM_CTRL  4
#define SERIAL_LINE_STATUS 5

#define SERIAL_TX_EMPTY 0x20  /* Line status bit: ready for the next byte. */

static void serial_init(void)
{
    port_write(COM1 + SERIAL_IRQ_ENABLE, 0x00); /* No interrupts, we poll. */
    port_write(COM1 + SERIAL_LINE_CTRL, 0x80);  /* Next two writes set the speed. */
    port_write(COM1 + 0, 0x01);                 /* Divisor 1 = 115200 baud... */
    port_write(COM1 + 1, 0x00);                 /* ...(high byte). */
    port_write(COM1 + SERIAL_LINE_CTRL, 0x03);  /* 8 bits, no parity, 1 stop bit. */
    port_write(COM1 + SERIAL_FIFO, 0xC7);       /* Turn on and clear the buffers. */
    port_write(COM1 + SERIAL_MODEM_CTRL, 0x03); /* Tell the other side we're ready. */
}

static void serial_write(const char *text, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++) {
        /* Wait until the port can take another byte. */
        while ((port_read(COM1 + SERIAL_LINE_STATUS) & SERIAL_TX_EMPTY) == 0) {
        }
        port_write(COM1 + SERIAL_DATA, (unsigned char)text[i]);
    }
}

/* Prints a number as 16 hex digits, like 000000000009FC00. */
static void serial_write_hex(unsigned long value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[16];
    int i;

    for (i = 15; i >= 0; i--) {
        text[i] = digits[value & 0xF];
        value >>= 4;
    }
    serial_write(text, sizeof(text));
}

/* --- Memory map --------------------------------------------------------------
 * The boot sector asked the BIOS which memory is free RAM and left the
 * answer here. Must match MEMORY_MAP_ADDRESS in boot.asm. */

#define MEMORY_MAP_ADDRESS 0x4000

/* One piece of memory, exactly as the BIOS writes it (24 bytes). */
typedef struct {
    unsigned long start;
    unsigned long length;
    unsigned int type;   /* MEMORY_FREE, or something we must not touch. */
    unsigned int extra;  /* Extra flags. Bit 0 clear means "ignore this entry". */
} MemoryMapEntry;

#define MEMORY_FREE 1

typedef struct {
    unsigned short count;
    unsigned short padding[3];
    MemoryMapEntry entries[1]; /* Really count entries. */
} MemoryMap;

static void memory_map_print(const MemoryMap *map)
{
    unsigned short i;

    serial_write("memory map:\r\n", 13);
    for (i = 0; i < map->count; i++) {
        const MemoryMapEntry *entry = &map->entries[i];

        if ((entry->extra & 1) == 0) {
            continue;
        }
        serial_write("  ", 2);
        serial_write_hex(entry->start);
        serial_write(" - ", 3);
        serial_write_hex(entry->start + entry->length - 1);
        if (entry->type == MEMORY_FREE) {
            serial_write("  free\r\n", 8);
        } else {
            serial_write("  taken\r\n", 9);
        }
    }
}

/* --- Start ------------------------------------------------------------------ */

/* Set by kernel.ld: where uninitialized globals ("BSS") start and end. */
extern char kernel_bss_start[];
extern char kernel_bss_end[];

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* The section name makes kernel.ld put this first, at 0x10000. */
__attribute__((section(".text.start"))) void kernel_start(void)
{
    static const char message[] = "hello from ark os\r\n";
    char *p;

    /* The disk image only holds code and data, not the zeroed globals.
     * C expects them to be zero, so clear that memory ourselves. */
    for (p = kernel_bss_start; p < kernel_bss_end; p++) {
        *p = 0;
    }

    serial_init();
    serial_write(message, sizeof(message) - 1);
    memory_map_print((const MemoryMap *)MEMORY_MAP_ADDRESS);

    halt_forever();
}
