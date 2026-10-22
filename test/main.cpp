#include <ark/memory.h>

int main() {
    void* memory = ark_memory_allocate(1024);

    if (!memory) {
        return 1;
    }

    ark_memory_free(memory);

    return 0;
}