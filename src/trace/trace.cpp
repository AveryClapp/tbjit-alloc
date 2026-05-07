#include "trace/trace.h"
#include "alloc/alloc.h"
#include <atomic>
#include <cstdint>
#include <new>

namespace tbjit::trace {

static std::atomic<RingBuffer*> g_ring_head{nullptr};

bool RingBuffer::push(const AllocEvent& ev) {
    uint64_t h = head.load(std::memory_order_relaxed);
    uint64_t t = tail.load(std::memory_order_acquire);
    bool was_empty = (h == t);
    if (h - t >= CAPACITY) return false; // drop on overflow
    slots[h & (CAPACITY - 1)] = ev;
    head.store(h + 1, std::memory_order_release);
    return was_empty;
}

bool RingBuffer::pop(AllocEvent& out) {
    uint64_t t = tail.load(std::memory_order_relaxed);
    uint64_t h = head.load(std::memory_order_acquire);
    if (t == h) return false;
    out = slots[t & (CAPACITY - 1)];
    tail.store(t + 1, std::memory_order_release);
    return true;
}

static RingBuffer* get_ring() {
    thread_local RingBuffer* tl_ring = nullptr;
    if (__builtin_expect(tl_ring == nullptr, 0)) {
        tl_ring = new (alloc::alloc(sizeof(RingBuffer), alignof(RingBuffer))) RingBuffer{};
        RingBuffer* old_head;
        do {
            old_head = g_ring_head.load(std::memory_order_relaxed);
            tl_ring->next.store(old_head, std::memory_order_relaxed);
        } while (!g_ring_head.compare_exchange_weak(
            old_head, tl_ring, std::memory_order_release, std::memory_order_relaxed));
    }
    return tl_ring;
}

static uint64_t rdtsc() {
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    // Fallback for non-x86 hosts (e.g. macOS ARM dev builds)
    return 0;
#endif
}

static uint32_t next_thread_id() {
    static std::atomic<uint32_t> counter{0};
    thread_local uint32_t id = counter.fetch_add(1, std::memory_order_relaxed);
    return id;
}

void init() {
    alloc::init();
}

void record_alloc(CallSiteID id, size_t size, void* ptr) {
    AllocEvent ev{id, static_cast<uint32_t>(size), rdtsc(), next_thread_id(), ptr};
    get_ring()->push(ev);
}

void record_free(CallSiteID id, void* ptr) {
    AllocEvent ev{id, 0, rdtsc(), next_thread_id(), ptr};
    get_ring()->push(ev);
}

RingBuffer* ring_head() {
    return g_ring_head.load(std::memory_order_acquire);
}

} // namespace tbjit::trace
