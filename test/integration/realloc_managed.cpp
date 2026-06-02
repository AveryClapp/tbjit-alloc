// Force a site to compile (so its chunks are tbjit-managed), then realloc a
// managed pointer and assert the contents survive and the program does not
// abort. Pre-fix this aborts in libc realloc (it chokes on the unfamiliar
// tbjit segment pointer). Run with TBJIT_STABLE_WINDOWS=3 so the site
// compiles quickly. Linux only: relies on tbjit interposing malloc/realloc/
// free for this process's own calls (a no-op on macOS).
#include <cstdlib>
#include <cstring>
#include <cstdio>

int main() {
    void* keep[64];
    // Warm the site past the convergence threshold so it compiles.
    for (int i = 0; i < 20000; ++i) {
        void* p = malloc(64);
        if (i < 64) keep[i] = p; else free(p);
    }
    char* p = static_cast<char*>(malloc(64));
    memset(p, 0xAB, 64);
    char* q = static_cast<char*>(realloc(p, 128));
    // First 64 bytes must be preserved across the grow.
    for (int i = 0; i < 64; ++i) {
        if (static_cast<unsigned char>(q[i]) != 0xAB) {
            fprintf(stderr, "realloc lost data at %d\n", i);
            return 1;
        }
    }
    free(q);
    for (int i = 0; i < 64; ++i) free(keep[i]);
    printf("OK\n");
    return 0;
}
