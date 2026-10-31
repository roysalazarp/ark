#include <string.h>

#include <ark/memory.h>

void ark_memory_init(Memory *memory, void *mem, size_t mem_size) {
    memset(mem, 0, mem_size);

    memory->size = (size_t)mem_size;
    memory->start = mem;
    memory->current = mem;
    memory->used = (unsigned char *)memory->current - (unsigned char *)memory->start;
}

void ark_memory_reset(Memory *memory, void *ptr) {
    size_t set_bytes = (unsigned char *)memory->current - (unsigned char *)ptr;
    memset(ptr, 0, set_bytes);

    memory->current = ptr;
    memory->used = (unsigned char *)memory->current - (unsigned char *)memory->start;
}
