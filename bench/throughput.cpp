#include <cstdlib>
#include <cstdio>
#include <ctime>

// Baseline allocation throughput benchmark.
// Run under LD_PRELOAD to measure tbjit vs. system allocator.
int main() {
    constexpr int ITERS = 1'000'000;
    constexpr size_t SIZE = 48;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < ITERS; ++i) {
        void* p = malloc(SIZE);
        free(p);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    printf("%.1f ns/op  (%d iters, %zu bytes)\n", ns / ITERS, ITERS, SIZE);
    return 0;
}
