#include "slow_init.h"
#include "tls.h"
#include "seg/segment.h"
#include <cassert>
#include <cstdio>

// TEMPORARY: diagnostic prints to localize the JIT-execution segfault on
// Linux CI. Compile-time gated so the local build is unaffected. To be
// removed once the root cause is identified.
#ifdef TBJIT_DEBUG_JIT
#define DBG(...) do { std::fprintf(stderr, "[tbjit] " __VA_ARGS__); std::fflush(stderr); } while (0)
#else
#define DBG(...) ((void)0)
#endif

namespace tbjit::codegen {

uint8_t* bump_slow_init(uint32_t index, uint32_t size) {
    DBG("bump_slow_init enter idx=%u size=%u\n", index, size);
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::BumpAlloc, index, /*site=*/g_slot_to_site[index], size);
    DBG("bump_slow_init seg=%p\n", static_cast<void*>(s));
    assert(s);
    uint8_t* base = seg::payload_start(s);
    tl_bumps[index].ptr = base + size;
    tl_bumps[index].end = seg::segment_end(s);
    s->bump_ptr  = base + size;
    DBG("bump_slow_init exit base=%p ptr=%p end=%p\n",
        base, tl_bumps[index].ptr, tl_bumps[index].end);
    return base;
}

uint8_t* pc_refill(uint32_t index, uint32_t size) {
    // Retire any current segment for this slot.
    if (tl_bumps[index].end) {
        seg::SegmentHeader* active = seg::of(tl_bumps[index].end - 1);
        if (seg::is_managed(active) &&
            active->strategy == Strategy::ProducerConsumer) {
            uint8_t* payload = seg::payload_start(active);
            uint32_t served = static_cast<uint32_t>(
                (active->bump_ptr - payload) / active->chunk_size);

            // Drain MPSC: any chunk on the queue is already-freed by the
            // consumer. Subtract them from the served count.
            uint32_t drained = 0;
            void* chain = seg::mpsc_harvest(active);
            while (chain) {
                ++drained;
                chain = *static_cast<void**>(chain);
            }
            uint32_t outstanding =
                (served > drained) ? (served - drained) : 0;
            active->live_chunks.store(
                outstanding, std::memory_order_release);
            active->retired = true;
        }
    }

    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::ProducerConsumer, index, g_slot_to_site[index], size);
    assert(s);
    uint8_t* base = seg::payload_start(s);
    tl_bumps[index].ptr = base + size;
    tl_bumps[index].end = seg::segment_end(s);
    s->bump_ptr  = base + size;
    return base;
}

uint8_t* paired_slow_init(uint32_t index, uint32_t size) {
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::PairedStack, index, g_slot_to_site[index], size);
    assert(s);
    uint8_t* base = seg::payload_start(s);
    tl_bumps[index].ptr = base + size;
    tl_bumps[index].end = seg::segment_end(s);
    s->bump_ptr  = base + size;
    return base;
}

void* freelist_refill(uint32_t index, uint32_t obj_size) {
    assert(obj_size >= sizeof(void*) && "free-list chunk must hold a pointer");

    // Harvest the active segment's MPSC remote queue first (foreign frees).
    // Retired segments' queues are not harvested; chunks freed into them
    // are accounted via live_chunks (decremented in the free trampoline).
    seg::SegmentHeader* active = tl_freelists[index].segs;
    if (active) {
        void* harvested = seg::mpsc_harvest(active);
        if (harvested) {
            void* chunk = harvested;
            tl_freelists[index].head = *static_cast<void**>(harvested);
            return chunk;
        }
        // Active is drained: every chunk it ever produced is now in user
        // code (head was null, MPSC empty). Retire it so the reaper can
        // reclaim once all chunks are freed back.
        active->live_chunks.store(
            seg::chunks_in_segment(active), std::memory_order_release);
        active->retired = true;
    }

    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::ThreadLocalFreeList, index,
        g_slot_to_site[index], obj_size);
    assert(s);
    s->next_in_site = tl_freelists[index].segs;
    tl_freelists[index].segs = s;

    uint8_t* base = seg::payload_start(s);
    uint8_t* end  = seg::segment_end(s);

    void* head = nullptr;
    for (uint8_t* p = base; p + obj_size <= end; p += obj_size) {
        *reinterpret_cast<void**>(p) = head;
        head = p;
    }

    void* chunk = head;
    tl_freelists[index].head = *reinterpret_cast<void**>(head);
    return chunk;
}

void* multi_refill(uint32_t slot, uint32_t obj_size, uint32_t class_idx) {
    assert(obj_size >= sizeof(void*));
    assert(class_idx < MULTI_MAX_CLASSES);

    MultiFreeListSlot& m = tl_multi_freelists[slot];

    // Harvest active segment's MPSC queue for this class, retire on miss.
    seg::SegmentHeader* active = m.segs[class_idx];
    if (active) {
        void* harvested = seg::mpsc_harvest(active);
        if (harvested) {
            void* chunk = harvested;
            m.heads[class_idx] = *static_cast<void**>(harvested);
            return chunk;
        }
        active->live_chunks.store(
            seg::chunks_in_segment(active), std::memory_order_release);
        active->retired = true;
    }

    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::MultiSizeFreeList, slot,
        g_slot_to_site[slot], obj_size);
    assert(s);
    s->class_idx = static_cast<uint8_t>(class_idx);
    s->next_in_site = m.segs[class_idx];
    m.segs[class_idx] = s;

    uint8_t* base = seg::payload_start(s);
    uint8_t* end  = seg::segment_end(s);
    void* head = nullptr;
    for (uint8_t* p = base; p + obj_size <= end; p += obj_size) {
        *reinterpret_cast<void**>(p) = head;
        head = p;
    }
    void* chunk = head;
    m.heads[class_idx] = *reinterpret_cast<void**>(head);
    return chunk;
}

uint8_t* arena_slow_init(uint32_t index, uint32_t size) {
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::EpochArena, index, /*site=*/g_slot_to_site[index], size);
    assert(s);
    uint8_t* base = seg::payload_start(s);
    tl_arenas[index].base = base;
    tl_arenas[index].ptr  = base + size;
    tl_arenas[index].end  = seg::segment_end(s);
    return base;
}

uint8_t* arena_reset_alloc(uint32_t index, uint32_t size) {
    uint8_t* base = tl_arenas[index].base;
    if (!base) return arena_slow_init(index, size);  // first call: same as init
    tl_arenas[index].ptr = base + size;
    return base;
}

} // namespace tbjit::codegen
