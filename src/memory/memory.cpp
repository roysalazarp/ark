#include <ark/memory.h>

#include <stdlib.h>

void* ark_memory_allocate(size_t size) {
    return malloc(size);
}

void ark_memory_free(void* memory) {
    free(memory);
}