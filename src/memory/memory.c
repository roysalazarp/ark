#include <ark/memory.h>

#define ARK_MEMORY_ALIGNMENT 16

void ark_memory_init(Memory *memory, void *mem, size_t mem_size) {
    ark_memory_zero(mem, mem_size);

    memory->size = (size_t)mem_size;
    memory->start = mem;
    memory->current = mem;
    memory->used = (unsigned char *)memory->current - (unsigned char *)memory->start;
}

void ark_memory_reset(Memory *memory, void *ptr) {
    size_t set_bytes = (unsigned char *)memory->current - (unsigned char *)ptr;
    ark_memory_zero(ptr, set_bytes);

    memory->current = ptr;
    memory->used = (unsigned char *)memory->current - (unsigned char *)memory->start;
}

/* Returns zeroed memory, or NULL if the arena is full. */
void *ark_memory_push(Memory *memory, size_t size) {
    size_t aligned = (memory->used + (ARK_MEMORY_ALIGNMENT - 1)) & ~(size_t)(ARK_MEMORY_ALIGNMENT - 1);
    void *result;

    if (aligned > memory->size || size > memory->size - aligned) {
        return NULL;
    }

    result = (unsigned char *)memory->start + aligned;
    memory->current = (unsigned char *)result + size;
    memory->used = aligned + size;

    return result;
}

void ark_memory_zero(void *dest, size_t size) {
    unsigned char *bytes = (unsigned char *)dest;
    size_t i;

    for (i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
}

void ark_memory_copy(void *dest, const void *src, size_t size) {
    unsigned char *to = (unsigned char *)dest;
    const unsigned char *from = (const unsigned char *)src;
    size_t i;

    for (i = 0; i < size; ++i) {
        to[i] = from[i];
    }
}
