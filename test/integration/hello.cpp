#include <cstdlib>
#include <cstring>

static volatile unsigned long g_sink;

int main() {
    // Hot call site: monomorphic 48-byte allocs.
    // 12,000 iterations = 12 stable windows of 1000 -> should reach Compiled+BumpAlloc.
    for (int i = 0; i < 12'000; ++i) {
        unsigned char* p = static_cast<unsigned char*>(malloc(48));
        if (!p) return 1;
        memset(p, 0xAB, 48);
        g_sink += p[0];  // force observable read — prevents dead-store+malloc/free elimination under -O2 -DNDEBUG
        free(p);
    }

    // Cold call site: mixed sizes -- should stay in PreSpec.
    for (int i = 0; i < 300; ++i) {
        size_t size = (i % 3 == 0) ? 64 : (i % 3 == 1) ? 128 : 256;
        void* p = malloc(size);
        if (!p) return 1;
        free(p);
    }

    return 0;
}
