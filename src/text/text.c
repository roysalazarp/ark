#include <ark/text.h>

static unsigned char ascii_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

size_t ark_cstr_length(const char *s) {
    size_t length = 0;

    while (s[length] != '\0') {
        ++length;
    }

    return length;
}

int ark_cstr_equal(const char *a, const char *b) {
    size_t i = 0;

    while (a[i] != '\0' && a[i] == b[i]) {
        ++i;
    }

    return a[i] == b[i];
}

int ark_bytes_equal_cstr(const unsigned char *bytes, size_t length, const char *s) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (s[i] == '\0' || bytes[i] != (unsigned char)s[i]) {
            return 0;
        }
    }

    return s[length] == '\0';
}

int ark_bytes_equal_cstr_ci(const unsigned char *bytes, size_t length, const char *s) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (s[i] == '\0' || ascii_lower(bytes[i]) != (unsigned char)s[i]) {
            return 0;
        }
    }

    return s[length] == '\0';
}

int ark_is_whitespace(unsigned char c) {
    return c == ' ' || c == '\t';
}

void ark_buffer_init(ArkBuffer *buffer, void *data, size_t capacity) {
    buffer->data = (unsigned char *)data;
    buffer->length = 0;
    buffer->capacity = data != NULL ? capacity : 0;
    buffer->overflowed = 0;
}

int ark_buffer_push(ArkBuffer *buffer, Memory *memory, size_t capacity) {
    void *data = ark_memory_push(memory, capacity);

    ark_buffer_init(buffer, data, capacity);

    return data != NULL;
}

void ark_buffer_append(ArkBuffer *buffer, const void *data, size_t length) {
    if (length > buffer->capacity - buffer->length) {
        buffer->overflowed = 1;
        return;
    }

    ark_memory_copy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}

void ark_buffer_append_cstr(ArkBuffer *buffer, const char *s) {
    ark_buffer_append(buffer, s, ark_cstr_length(s));
}

void ark_buffer_append_unsigned(ArkBuffer *buffer, unsigned long value) {
    char digits[20];
    size_t count = 0;
    char reversed[20];
    size_t i;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    for (i = 0; i < count; ++i) {
        reversed[i] = digits[count - 1 - i];
    }

    ark_buffer_append(buffer, reversed, count);
}
