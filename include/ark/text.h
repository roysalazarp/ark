#ifndef ARK_TEXT_H
#define ARK_TEXT_H

/* Small text helpers, so no module needs libc's string functions. */

#include <stddef.h>

#include <ark/memory.h>

size_t ark_cstr_length(const char *);
int ark_cstr_equal(const char *a, const char *b);

/* Whether length bytes equal the null-terminated s. */
int ark_bytes_equal_cstr(const unsigned char *bytes, size_t length, const char *s);

/* The same, ignoring ASCII case. s must be lowercase. */
int ark_bytes_equal_cstr_ci(const unsigned char *bytes, size_t length, const char *s);

/* Space or tab. */
int ark_is_whitespace(unsigned char);

/*
 * Bytes written into a fixed space. A write that doesn't fit is dropped whole
 * and sets overflowed, so a full buffer never holds half a value.
 */
typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
    int overflowed;
} ArkBuffer;

void ark_buffer_init(ArkBuffer *, void *data, size_t capacity);

/* Takes the space from an arena. Returns 0 if the arena is full. */
int ark_buffer_push(ArkBuffer *, Memory *, size_t capacity);

void ark_buffer_append(ArkBuffer *, const void *data, size_t length);
void ark_buffer_append_cstr(ArkBuffer *, const char *);
void ark_buffer_append_unsigned(ArkBuffer *, unsigned long);

#endif
