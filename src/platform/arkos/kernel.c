/*
 * The Ark OS kernel. For now it only prints "hello" on the serial port.
 *
 * Freestanding: no C standard library, only the compiler's own headers.
 * The boot sector loads this at 0x10000, already in 64-bit mode, and calls
 * kernel_start.
 */

#include <stddef.h>

#include <ark/platform.h>

#define MB(n) ((unsigned long)(n) * 1024 * 1024)

/* Same sizes the Linux platform uses. */
#define PERMANENT_MEMORY_SIZE MB(64)
#define TRANSIENT_MEMORY_SIZE MB(64)

/* --- memset ------------------------------------------------------------------
 * There is no C library, but GCC may still turn loops like "set all these
 * bytes to 0" into a call to memset. So we must provide it ourselves.
 * (build_os.sh passes -fno-tree-loop-distribute-patterns, so the loop below
 * isn't itself turned into a call to memset, which would call itself forever.) */

void *memset(void *destination, int value, size_t size)
{
    unsigned char *p = destination;

    while (size--) {
        *p++ = (unsigned char)value;
    }
    return destination;
}

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

/* --- App memory --------------------------------------------------------------
 * Give the app its two blocks, permanent and transient, from free RAM above
 * 1 MiB. Below 1 MiB is small and full of BIOS leftovers, the kernel and
 * the page tables, so we leave it alone. */

/* The boot sector's page tables only cover the first 1 GiB. Memory above
 * that would crash us when touched, so the blocks must end below it. */
#define MAPPED_MEMORY_END MB(1024)

static ArkAppMemory app_memory;

/* Returns 1 on success, 0 if there isn't enough free memory. */
static int app_memory_setup(const MemoryMap *map)
{
    unsigned long needed = PERMANENT_MEMORY_SIZE + TRANSIENT_MEMORY_SIZE;
    unsigned short i;

    for (i = 0; i < map->count; i++) {
        const MemoryMapEntry *entry = &map->entries[i];
        unsigned long start = entry->start;
        unsigned long end = entry->start + entry->length;

        if (entry->type != MEMORY_FREE || (entry->extra & 1) == 0) {
            continue;
        }

        /* Use only the part between 1 MiB and the end of mapped memory. */
        if (start < MB(1)) {
            start = MB(1);
        }
        if (end > MAPPED_MEMORY_END) {
            end = MAPPED_MEMORY_END;
        }
        if (start >= end || end - start < needed) {
            continue; /* Not big enough. Try the next one. */
        }

        /* Draw the two blocks, one after the other. */
        app_memory.permanent = (void *)start;
        app_memory.permanent_size = PERMANENT_MEMORY_SIZE;
        app_memory.transient = (void *)(start + PERMANENT_MEMORY_SIZE);
        app_memory.transient_size = TRANSIENT_MEMORY_SIZE;

        /* The app expects both to start out as zeros. */
        memset(app_memory.permanent, 0, app_memory.permanent_size);
        memset(app_memory.transient, 0, app_memory.transient_size);
        return 1;
    }
    return 0;
}

static void app_memory_print(void)
{
    serial_write("permanent: ", 11);
    serial_write_hex((unsigned long)app_memory.permanent);
    serial_write(" (64 MiB)\r\n", 11);
    serial_write("transient: ", 11);
    serial_write_hex((unsigned long)app_memory.transient);
    serial_write(" (64 MiB)\r\n", 11);
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

    if (!app_memory_setup((const MemoryMap *)MEMORY_MAP_ADDRESS)) {
        serial_write("not enough free memory for the app\r\n", 36);
        halt_forever();
    }
    app_memory_print();

    halt_forever();
}
