#ifndef ARK_BYTES_H
#define ARK_BYTES_H

#include <stddef.h>

/*
 * A run of bytes: not null-terminated, and it does not own what it points to.
 * Every module uses this one type for bytes, so values pass between them as is.
 */
typedef struct {
    const unsigned char *data;
    size_t length;
} ArkBytes;

#endif
