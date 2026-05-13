#include <cstdlib>
#include <cstring>
#include <time.h>

// EpochArena workload: monomorphic alloc + free. The arena strategy
// recycles its region every ~5460 allocations of 48 bytes (256 KiB /
// 48 ≈ 5460); BumpAlloc would deopt at that point. We run far more
// iterations than fit in one region so the difference matters.

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
