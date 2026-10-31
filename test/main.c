#define _DEFAULT_SOURCE

#include <stddef.h>
#include <sys/mman.h>

#include <ark/memory.h>

#define KB(value) ((value) * 1024)
#define MB(value) (KB(value) * 1024)
#define GB(value) (MB(value) * 1024)

#define PAGE_SIZE KB(4)

int main(void) {
    size_t mem_size = PAGE_SIZE * 15;
    void *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    Memory memory = {0};

    ark_memory_init(&memory, mem, mem_size);

    ark_memory_reset(&memory, memory.current);

    return 0;
}
