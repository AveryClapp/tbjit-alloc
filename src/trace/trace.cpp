#include "trace.h"
#include "alloc/alloc.h"
#include <atomic>
#include <cstdint>

namespace tbjit::trace {

// Lock-free single-producer ring buffer per thread.
// The background analyzer is the sole consumer.
namespace {

constexpr size_t RING_CAPACITY = 1024; // must be power of 2

struct RingBuffer {
    AllocEvent  slots[RING_CAPACITY];
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};

    bool push(const AllocEvent& ev) {
        uint64_t h = head.load(std::memory_order_relaxed);
        uint64_t t = tail.load(std::memory_order_acquire);
        if (h - t >= RING_CAPACITY) return false; // full, drop
        slots[h & (RING_CAPACITY - 1)] = ev;
        head.store(h + 1, std::memory_order_release);
        return true;
    }
};

thread_local RingBuffer* tl_ring = nullptr;

RingBuffer* get_ring() {
    if (__builtin_expect(tl_ring == nullptr, 0)) {
        tl_ring = new (alloc::alloc(sizeof(RingBuffer), alignof(RingBuffer))) RingBuffer{};
    }
    return tl_ring;
}

inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline uint32_t thread_id() {
    static std::atomic<uint32_t> counter{0};
    thread_local uint32_t id = counter.fetch_add(1, std::memory_order_relaxed);
    return id;
}

} // namespace

void init() {
    alloc::init();
}

void record_alloc(CallSiteID id, size_t size, void* ptr) {
    AllocEvent ev{id, static_cast<uint32_t>(size), rdtsc(), thread_id(), ptr};
    get_ring()->push(ev);
}

void record_free(CallSiteID id, void* ptr) {
    AllocEvent ev{id, 0, rdtsc(), thread_id(), ptr};
    get_ring()->push(ev);
}

} // namespace tbjit::trace
