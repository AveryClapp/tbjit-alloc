#include <cstdlib>
#include <cstring>
#include <cassert>

// Minimal target program for LD_PRELOAD integration testing.
// Performs a mix of alloc/free patterns that exercise the trampoline.
int main() {
    for (int i = 0; i < 1000; ++i) {
        void* p = malloc(48);
        assert(p != nullptr);
        memset(p, 0, 48);
        free(p);
    }
    return 0;
}
