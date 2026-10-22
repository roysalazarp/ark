#ifndef ARK_MEMORY_H
#define ARK_MEMORY_H

#include <stddef.h>

void* ark_memory_allocate(size_t size);
void ark_memory_free(void* memory);

#endif