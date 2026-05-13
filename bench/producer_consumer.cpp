// Producer-consumer pattern: one thread allocates at a hot call site,
// another thread frees the allocations. Drives the analyzer's alloc_dist /
// free_dist tracking; expected pick is ProducerConsumer.
//
// Communication via a bounded SPSC ring of pointers. The producer never
// frees; the consumer never allocates. Each calls malloc/free from its own
// noinline helper so the return-address hashes are distinct.

#include "bench_common.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

constexpr int    RING_LEN = 1 << 14;  // 16k slots
constexpr size_t SIZE     = 48;

static void* g_ring[RING_LEN];
static std::atomic<uint64_t> g_head{0};   // producer writes
static std::atomic<uint64_t> g_tail{0};   // consumer reads

__attribute__((noinline))
static unsigned char* prod_alloc() {
    auto* p = static_cast<unsigned char*>(malloc(SIZE));
    if (p) memset(p, g_pattern, SIZE);
    escape(p);
    return p;
}

__attribute__((noinline))
static void cons_free(unsigned char* p) {
    free(p);
}

int main() {
    constexpr int N = 1'000'000;
    std::atomic<bool> done{false};

    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            unsigned char* p = prod_alloc();
            if (!p) std::exit(1);
            uint64_t h = g_head.load(std::memory_order_relaxed);
            // back-pressure: wait if ring full
            while (h - g_tail.load(std::memory_order_acquire) >= RING_LEN) {}
            g_ring[h & (RING_LEN - 1)] = p;
            g_head.store(h + 1, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    });

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    std::thread consumer([&]() {
        uint64_t t = 0;
        while (true) {
            uint64_t h = g_head.load(std::memory_order_acquire);
            if (t == h) {
                if (done.load(std::memory_order_acquire) &&
                    t == g_head.load(std::memory_order_acquire)) break;
                continue;
            }
            auto* p = static_cast<unsigned char*>(g_ring[t & (RING_LEN - 1)]);
            g_sink += p[0];
            cons_free(p);
            g_tail.store(++t, std::memory_order_release);
        }
    });

    producer.join();
    consumer.join();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    report("steady:", elapsed_ns(t0, t1), N * 2,
           ", 48B producer/consumer, 2 threads");
    return 0;
}
