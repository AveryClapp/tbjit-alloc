#pragma once
#include "common.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tbjit::seg {

// Segment granularity. Compile-time so it can be swept (the memory frontier
// study); overridable with -DTBJIT_SEG_SHIFT=N. Default 21 => 2 MiB, which
// matches the x86-64 hugepage size (see the MADV_HUGEPAGE hint in segment.cpp).
#ifndef TBJIT_SEG_SHIFT
#define TBJIT_SEG_SHIFT 21
#endif
constexpr unsigned  SEGMENT_SHIFT = TBJIT_SEG_SHIFT;
constexpr size_t    SEGMENT_SIZE = size_t(1) << SEGMENT_SHIFT;
constexpr uintptr_t SEGMENT_MASK = ~(static_cast<uintptr_t>(SEGMENT_SIZE) - 1);

struct alignas(64) SegmentHeader {
    Strategy              strategy;
    bool                  retired;        // true after a fresh active replaces it
    uint8_t               class_idx;      // MultiSizeFreeList: which class (0..3)
    bool                  decommitted;    // madvise reap mode: payload returned to OS
    uint32_t              slot_index;
    CallSiteID            alloc_site;
    uint32_t              owner_tid;
    std::atomic<uint32_t> live_chunks;    // outstanding chunks (set at retire)
    uint32_t              chunk_size;
    uint32_t              retire_epoch;
    uint32_t              _pad1;
    std::atomic<void*>    remote_head;
    uint8_t*              bump_ptr;
    uint8_t*              bump_limit;
    SegmentHeader*        next_in_site;
};
static_assert(sizeof(SegmentHeader) == 64, "header must be one cache line");

// Number of chunks of `chunk_size` that fit in the payload of seg `s`.
inline uint32_t chunks_in_segment(const SegmentHeader* s) {
    if (s->chunk_size == 0) return 0;
    size_t payload = SEGMENT_SIZE - sizeof(SegmentHeader);
    return static_cast<uint32_t>(payload / s->chunk_size);
}

inline uint8_t* base_of(SegmentHeader* s) {
    return reinterpret_cast<uint8_t*>(s);
}
inline uint8_t* payload_start(SegmentHeader* s) {
    return base_of(s) + sizeof(SegmentHeader);
}
inline uint8_t* segment_end(SegmentHeader* s) {
    return base_of(s) + SEGMENT_SIZE;
}

inline SegmentHeader* of(const void* p) {
    return reinterpret_cast<SegmentHeader*>(
        reinterpret_cast<uintptr_t>(p) & SEGMENT_MASK);
}

SegmentHeader* alloc_segment(Strategy s, uint32_t slot,
                             CallSiteID site, uint32_t chunk_size);
void           free_segment(SegmentHeader* seg);

// Segment-return policy (TBJIT_REAP_MODE, read once lazily):
//   Conservative — current default: reap only retired+empty segments whose
//                  alloc_site is Reap-tagged (LifetimePredicate gate).
//   Eager        — reap any retired+empty segment, dropping the tag gate;
//                  relies solely on the live_chunks==0 safety invariant.
//   Madvise      — like Eager but MADV_DONTNEED the payload instead of munmap,
//                  returning physical RSS while keeping the VA mapping (cheaper
//                  to re-acquire on churny workloads).
enum class ReapMode : uint8_t { Conservative, Eager, Madvise };
ReapMode reap_mode();

// Per-site THP policy (TBJIT_THP=auto): a registered predicate decides whether a
// new segment for `site` gets MADV_HUGEPAGE. Lets Hold-tagged sites keep the TLB
// benefit (their segments are full, so no RSS slack) while churn sites drop THP,
// which otherwise faults whole 2 MiB hugepages for partially-filled segments
// (the dominant RSS slack). Modes always (default) / never ignore the predicate.
using ThpPredicate = bool (*)(CallSiteID site);
void set_thp_predicate(ThpPredicate pred);

bool is_managed(const SegmentHeader* seg);
size_t segment_count();  // registered segments (is_managed scans this many)
void register_segment(SegmentHeader* seg);
void unregister_segment(SegmentHeader* seg);

uint32_t current_tid();

// MPSC remote-free queue rooted at seg->remote_head. Producers (non-owner
// threads) push freed chunks via mpsc_push (each chunk's first 8 bytes hold
// the next pointer while on the list). The owner thread harvests via
// mpsc_harvest, which returns the entire chain or nullptr and atomically
// resets remote_head.
void  mpsc_push(SegmentHeader* seg, void* chunk);
void* mpsc_harvest(SegmentHeader* seg);

// Sweep all registered segments and munmap any that are eligible: retired,
// live_chunks==0, and the alloc_site's lifetime tag (looked up via callback)
// is Reap. Returns the number of segments reclaimed.
using LifetimePredicate = bool (*)(CallSiteID alloc_site);
size_t reaper_sweep(LifetimePredicate pred);

} // namespace tbjit::seg
