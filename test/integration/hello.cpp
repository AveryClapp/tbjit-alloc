#include <cstdlib>
#include <cstring>
#include <time.h>

static volatile unsigned long g_sink;

int main() {
    // Hot call site: monomorphic 48-byte allocs.
    // 20,000 iterations with periodic yields: a tight single-threaded loop
    // outruns the SPSC ring (1024 slots), causing the background analysis
    // thread to miss events and never accumulate enough stable windows.
    // The yields cost ~5ms total but let the analyzer keep up.
    for (int i = 0; i < 20'000; ++i) {
        unsigned char* p = static_cast<unsigned char*>(malloc(48));
        if (!p) return 1;
        memset(p, 0xAB, 48);
        g_sink += p[0];  // force observable read — prevents dead-store+malloc/free elimination under -O2 -DNDEBUG
        free(p);
        if ((i & 1023) == 1023) {
            struct timespec ts{0, 250'000L};  // 250us
            nanosleep(&ts, nullptr);
        }
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
