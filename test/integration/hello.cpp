#include <cstdlib>
#include <cstring>
#include <cassert>

int main() {
    // Hot call site: monomorphic 48-byte allocs.
    // 12,000 iterations = 12 stable windows of 1000 -> should reach Compiled+BumpAlloc.
    for (int i = 0; i < 12'000; ++i) {
        void* p = malloc(48);
        assert(p != nullptr);
        memset(p, 0xAB, 48);
        free(p);
    }

    // Cold call site: mixed sizes -- should stay in PreSpec.
    for (int i = 0; i < 300; ++i) {
        size_t size = (i % 3 == 0) ? 64 : (i % 3 == 1) ? 128 : 256;
        void* p = malloc(size);
        assert(p != nullptr);
        free(p);
    }

    return 0;
}
