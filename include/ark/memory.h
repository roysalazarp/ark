#ifndef ARK_MEMORY_H
#define ARK_MEMORY_H

#include <stddef.h>

typedef struct {
    size_t size;
    size_t used;
    void *start;
    void *current;
    int in_use;
} Memory;

void ark_memory_init(Memory *, void *, size_t);
void ark_memory_reset(Memory *, void *);
void *ark_memory_push(Memory *, size_t);

void ark_memory_zero(void *, size_t);
void ark_memory_copy(void *, const void *, size_t);

#endif
