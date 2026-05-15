#include "segment.h"
#include <atomic>
#include <cassert>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace tbjit::seg {

namespace {

constexpr size_t MAX_SEGMENTS = 4096;

std::atomic<SegmentHeader*> g_index[MAX_SEGMENTS];
std::atomic<size_t>         g_index_count{0};
pthread_mutex_t             g_index_mutex = PTHREAD_MUTEX_INITIALIZER;

void* aligned_mmap_2mib() {
    // Over-allocate by SEGMENT_SIZE, then trim leading and trailing slack so
    // the surviving range is 2 MiB-aligned. munmap on partial ranges is safe
    // and the kernel coalesces neighboring unmapped regions.
    void* raw = mmap(nullptr, SEGMENT_SIZE * 2,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return nullptr;

    uintptr_t lo  = reinterpret_cast<uintptr_t>(raw);
    uintptr_t hi  = lo + SEGMENT_SIZE * 2;
    uintptr_t aligned_lo = (lo + SEGMENT_SIZE - 1) & SEGMENT_MASK;
    uintptr_t aligned_hi = aligned_lo + SEGMENT_SIZE;

    if (aligned_lo != lo)
        munmap(raw, aligned_lo - lo);
    if (hi != aligned_hi)
        munmap(reinterpret_cast<void*>(aligned_hi), hi - aligned_hi);

#if defined(__linux__) && defined(MADV_HUGEPAGE)
    // Hint the kernel to back this region with a transparent hugepage.
    // The surviving range is naturally 2-MiB-aligned (== x86-64 large page
    // size) so a single THP can cover the entire segment, dropping TLB
    // pressure on alloc-heavy workloads. mimalloc does this; we didn't,
    // and the `hold` bench gap (mimalloc 6.9 ns vs ours ~87 ns/op tracked
    // closely with TLB-miss cost) is what this targets. madvise is
    // best-effort: if THP is disabled at the kernel level, the segment
    // stays backed by 4 KiB pages, no harm done.
    (void) madvise(reinterpret_cast<void*>(aligned_lo), SEGMENT_SIZE,
                   MADV_HUGEPAGE);
#endif

    return reinterpret_cast<void*>(aligned_lo);
}

} // namespace

uint32_t current_tid() {
#if defined(__linux__)
    static thread_local uint32_t cached = 0;
    if (cached == 0)
        cached = static_cast<uint32_t>(syscall(SYS_gettid));
    return cached;
#else
    return static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(pthread_self()) & 0xffffffffu);
#endif
}

void mpsc_push(SegmentHeader* seg, void* chunk) {
    void* cur = seg->remote_head.load(std::memory_order_relaxed);
    do {
        *static_cast<void**>(chunk) = cur;
    } while (!seg->remote_head.compare_exchange_weak(
        cur, chunk,
        std::memory_order_release,
        std::memory_order_relaxed));
}

void* mpsc_harvest(SegmentHeader* seg) {
    return seg->remote_head.exchange(nullptr, std::memory_order_acquire);
}

SegmentHeader* alloc_segment(Strategy s, uint32_t slot,
                             CallSiteID site, uint32_t chunk_size) {
    void* mem = aligned_mmap_2mib();
    if (!mem) return nullptr;
    auto* h = static_cast<SegmentHeader*>(mem);
    h->strategy     = s;
    h->retired      = false;
    h->class_idx    = 0;
    h->slot_index   = slot;
    h->alloc_site   = site;
    h->owner_tid    = current_tid();
    h->live_chunks.store(0, std::memory_order_relaxed);
    h->chunk_size   = chunk_size;
    h->retire_epoch = 0;
    h->remote_head.store(nullptr, std::memory_order_relaxed);
    h->bump_ptr     = payload_start(h);
    h->bump_limit   = segment_end(h);
    h->next_in_site = nullptr;
    register_segment(h);
    return h;
}

size_t reaper_sweep(LifetimePredicate pred) {
    // Snapshot eligible segments under the index lock, then munmap outside it
    // so munmap doesn't block other threads' registration.
    SegmentHeader* eligible[MAX_SEGMENTS];
    size_t n_eligible = 0;

    pthread_mutex_lock(&g_index_mutex);
    size_t n = g_index_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        SegmentHeader* h = g_index[i].load(std::memory_order_relaxed);
        if (!h || !h->retired) continue;
        if (h->live_chunks.load(std::memory_order_acquire) != 0) continue;
        if (pred && !pred(h->alloc_site)) continue;
        eligible[n_eligible++] = h;
    }
    pthread_mutex_unlock(&g_index_mutex);

    for (size_t i = 0; i < n_eligible; ++i)
        free_segment(eligible[i]);
    return n_eligible;
}

void free_segment(SegmentHeader* seg) {
    if (!seg) return;
    unregister_segment(seg);
    munmap(seg, SEGMENT_SIZE);
}

void register_segment(SegmentHeader* seg) {
    pthread_mutex_lock(&g_index_mutex);
    size_t n = g_index_count.load(std::memory_order_relaxed);
    if (n < MAX_SEGMENTS) {
        g_index[n].store(seg, std::memory_order_relaxed);
        g_index_count.store(n + 1, std::memory_order_release);
    }
    pthread_mutex_unlock(&g_index_mutex);
}

void unregister_segment(SegmentHeader* seg) {
    pthread_mutex_lock(&g_index_mutex);
    size_t n = g_index_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        if (g_index[i].load(std::memory_order_relaxed) == seg) {
            g_index[i].store(g_index[n - 1].load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
            g_index[n - 1].store(nullptr, std::memory_order_relaxed);
            g_index_count.store(n - 1, std::memory_order_release);
            break;
        }
    }
    pthread_mutex_unlock(&g_index_mutex);
}

bool is_managed(const SegmentHeader* seg) {
    if (!seg) return false;
    size_t n = g_index_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        if (g_index[i].load(std::memory_order_relaxed) == seg)
            return true;
    }
    return false;
}

} // namespace tbjit::seg
