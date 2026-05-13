#include "slow_init.h"
#include "tls.h"
#include "seg/segment.h"
#include <cassert>

namespace tbjit::codegen {

uint8_t* bump_slow_init(uint32_t index, uint32_t size) {
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::BumpAlloc, index, /*site=*/g_slot_to_site[index], size);
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
