#include "segment.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
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

// Pagemap: one bit per 2 MiB-aligned region of the address space, marking tbjit
// segments. Makes is_managed() an O(1) atomic bit test on the free hot path
// instead of an O(#segments) linear scan over g_index, and it never
// dereferences the candidate pointer (so it is safe on libc pointers whose
// 2 MiB-aligned base may be unmapped). mmap'd (16 MiB virtual) and lazily paged:
// only the words covering real segment addresses become resident. Lock-free:
// readers do an atomic load; register/unregister do atomic fetch_or/fetch_and.
// Standard x86-64 user VA is <= 2^47 < 2^48, so addr>>SEG_SHIFT always fits.
constexpr unsigned SEG_SHIFT = SEGMENT_SHIFT;                 // segment granularity
constexpr size_t   PM_SLOTS  = size_t(1) << (48 - SEG_SHIFT); // 2^(48-shift) regions
constexpr size_t   PM_WORDS  = PM_SLOTS / 64;                 // pagemap word count

std::atomic<uint64_t>* pagemap() {
    static std::atomic<uint64_t>* pm = []() -> std::atomic<uint64_t>* {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
        flags |= MAP_NORESERVE;
#endif
        void* m = mmap(nullptr, PM_WORDS * sizeof(uint64_t),
                       PROT_READ | PROT_WRITE, flags, -1, 0);
        return (m == MAP_FAILED) ? nullptr
                                 : static_cast<std::atomic<uint64_t>*>(m);
    }();
    return pm;
}

void pagemap_mark(const void* seg, bool set) {
    std::atomic<uint64_t>* pm = pagemap();
    if (!pm) return;
    size_t bit = reinterpret_cast<uintptr_t>(seg) >> SEG_SHIFT;
    if (bit >= PM_SLOTS) return;
    uint64_t mask = uint64_t(1) << (bit & 63);
    if (set) pm[bit >> 6].fetch_or(mask, std::memory_order_release);
    else     pm[bit >> 6].fetch_and(~mask, std::memory_order_release);
}

// MADV_DONTNEED the payload of an empty retired segment, keeping the header
// page (first page) resident and the mapping registered. Returns the bulk of
// the segment's physical RSS without unmapping the VA.
void decommit_segment(SegmentHeader* seg) {
#ifdef MADV_DONTNEED
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0 || static_cast<size_t>(ps) >= SEGMENT_SIZE) return;
    uintptr_t from = reinterpret_cast<uintptr_t>(seg) + static_cast<uintptr_t>(ps);
    (void) madvise(reinterpret_cast<void*>(from),
                   SEGMENT_SIZE - static_cast<size_t>(ps), MADV_DONTNEED);
#endif
    seg->decommitted = true;
}

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

ReapMode reap_mode() {
    static ReapMode m = []() {
        const char* e = std::getenv("TBJIT_REAP_MODE");
        if (e) {
            if (std::strcmp(e, "eager")   == 0) return ReapMode::Eager;
            if (std::strcmp(e, "madvise") == 0) return ReapMode::Madvise;
        }
        return ReapMode::Conservative;
    }();
    return m;
}

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
    h->decommitted  = false;
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
    // Snapshot eligible segments under the index lock, then reclaim outside it
    // so munmap/madvise doesn't block other threads' registration.
    ReapMode mode = reap_mode();
    SegmentHeader* eligible[MAX_SEGMENTS];
    size_t n_eligible = 0;

    pthread_mutex_lock(&g_index_mutex);
    size_t n = g_index_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        SegmentHeader* h = g_index[i].load(std::memory_order_relaxed);
        if (!h || !h->retired) continue;
        if (h->live_chunks.load(std::memory_order_acquire) != 0) continue;
        // Conservative keeps the Reap-tag gate; eager/madvise drop it and rely
        // solely on the live_chunks==0 safety invariant.
        if (mode == ReapMode::Conservative && pred && !pred(h->alloc_site))
            continue;
        // Madvise decommits a segment once, then leaves it registered/mapped.
        if (mode == ReapMode::Madvise && h->decommitted) continue;
        eligible[n_eligible++] = h;
    }
    pthread_mutex_unlock(&g_index_mutex);

    for (size_t i = 0; i < n_eligible; ++i) {
        if (mode == ReapMode::Madvise) decommit_segment(eligible[i]);
        else                           free_segment(eligible[i]);
    }
    return n_eligible;
}

void free_segment(SegmentHeader* seg) {
    if (!seg) return;
    unregister_segment(seg);
    munmap(seg, SEGMENT_SIZE);
}

void register_segment(SegmentHeader* seg) {
    // Mark the pagemap regardless of the g_index cap, so is_managed stays
    // correct even past MAX_SEGMENTS (those segments just aren't reaper-tracked).
    pagemap_mark(seg, true);
    pthread_mutex_lock(&g_index_mutex);
    size_t n = g_index_count.load(std::memory_order_relaxed);
    if (n < MAX_SEGMENTS) {
        g_index[n].store(seg, std::memory_order_relaxed);
        g_index_count.store(n + 1, std::memory_order_release);
    }
    pthread_mutex_unlock(&g_index_mutex);
}

void unregister_segment(SegmentHeader* seg) {
    pagemap_mark(seg, false);
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
    std::atomic<uint64_t>* pm = pagemap();
    if (!pm) return false;
    size_t bit = reinterpret_cast<uintptr_t>(seg) >> SEG_SHIFT;
    if (bit >= PM_SLOTS) return false;
    return (pm[bit >> 6].load(std::memory_order_acquire) >> (bit & 63)) & 1u;
}

size_t segment_count() { return g_index_count.load(std::memory_order_acquire); }

} // namespace tbjit::seg
