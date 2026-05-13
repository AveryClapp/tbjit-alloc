#include "seg/segment.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <thread>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;
using namespace tbjit::seg;

static void test_mpsc_push_and_harvest_single_thread() {
    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 0, 0, 64);
    CHECK(s);
    uint8_t* base = payload_start(s);

    // Push 5 chunks; harvest; verify chain length 5 and same pointers.
    void* a = base + 0;
    void* b = base + 64;
    void* c = base + 128;
    void* d = base + 192;
    void* e = base + 256;
    mpsc_push(s, a);
    mpsc_push(s, b);
    mpsc_push(s, c);
    mpsc_push(s, d);
    mpsc_push(s, e);

    void* h = mpsc_harvest(s);
    CHECK(h);
    size_t n = 0;
    while (h) { ++n; h = *static_cast<void**>(h); }
    CHECK(n == 5);
    CHECK(mpsc_harvest(s) == nullptr);  // empty after harvest

    free_segment(s);
}

static void test_mpsc_concurrent_producers() {
    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 0, 0, 64);
    CHECK(s);
    uint8_t* base = payload_start(s);

    constexpr int N_THREADS = 4;
    constexpr int PER_THREAD = 1000;

    std::vector<std::thread> producers;
    producers.reserve(N_THREADS);
    std::atomic<int> ready{0};
    for (int t = 0; t < N_THREADS; ++t) {
        producers.emplace_back([&, t]() {
            ready.fetch_add(1);
            while (ready.load() < N_THREADS) {}
            for (int i = 0; i < PER_THREAD; ++i) {
                void* chunk = base + (t * PER_THREAD + i) * 64;
                mpsc_push(s, chunk);
            }
        });
    }
    for (auto& th : producers) th.join();

    void* h = mpsc_harvest(s);
    size_t n = 0;
    while (h) { ++n; h = *static_cast<void**>(h); }
    CHECK(n == N_THREADS * PER_THREAD);

    free_segment(s);
}

int main() {
    test_mpsc_push_and_harvest_single_thread();
    test_mpsc_concurrent_producers();
    std::puts("test_mpsc_free OK");
    return 0;
}
