#include <cstdlib>
#include <cstring>
#include <time.h>

// Workload designed to exercise ThreadLocalFreeList end-to-end:
// monomorphic 48-byte malloc/free in a long loop. With TBJIT_FORCE_STRATEGY=freelist
// the JIT compiles a free-list routine that recycles chunks indefinitely;
// BumpAlloc would deopt after exhausting its 256 KiB region (~5460 allocs).

static volatile unsigned long g_sink;

int main() {
    constexpr int ITERS = 60'000;
    constexpr size_t SIZE = 48;

    for (int i = 0; i < ITERS; ++i) {
        unsigned char* p = static_cast<unsigned char*>(malloc(SIZE));
        if (!p) return 1;
        memset(p, 0xAB, SIZE);
        g_sink += p[0];
        free(p);
        if ((i & 1023) == 1023) {
            struct timespec ts{0, 250'000L};
            nanosleep(&ts, nullptr);
        }
    }
    return 0;
}
