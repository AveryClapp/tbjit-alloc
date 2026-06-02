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

bool paired_lifo_rewind(seg::SegmentHeader* s, void* ptr) {
    BumpSlot& slot = tl_bumps[s->slot_index];
    uint8_t* rewound = slot.ptr - s->chunk_size;
    if (rewound != static_cast<uint8_t*>(ptr)) return false;
    slot.ptr    = rewound;
    s->bump_ptr = rewound;
    return true;
}

void free_managed(seg::SegmentHeader* s, void* ptr) {
    switch (s->strategy) {
        case Strategy::BumpAlloc:
        case Strategy::EpochArena:
            break;  // chunks live until segment reclaim
        case Strategy::ProducerConsumer:
            // Active segment: foreign frees push to MPSC for the refill path
            // to drain on retire. Retired segment: decrement live_chunks;
            // reaper reclaims at zero.
            if (s->retired) {
                s->live_chunks.fetch_sub(1, std::memory_order_release);
            } else {
                seg::mpsc_push(s, ptr);
            }
            break;
        case Strategy::PairedStack: {
            // LIFO rewind via the TLS slot the JIT fast path actually reads —
            // seg->bump_ptr alone is a stale mirror and rewinding it wouldn't
            // recycle the chunk. Same-thread only: cross-thread frees can't
            // touch the owner's TLS. PairedStack's detection rule requires
            // concentrated alloc/free on one site pair, so cross-thread frees
            // are rare; drop those chunks.
            if (s->owner_tid == seg::current_tid())
                paired_lifo_rewind(s, ptr);
            break;
        }
        case Strategy::ThreadLocalFreeList: {
            if (s->retired) {
                s->live_chunks.fetch_sub(1, std::memory_order_release);
                break;
            }
            uint32_t my_tid = seg::current_tid();
            if (s->owner_tid == my_tid) {
                *static_cast<void**>(ptr) = tl_freelists[s->slot_index].head;
                tl_freelists[s->slot_index].head = ptr;
            } else {
                seg::mpsc_push(s, ptr);
            }
            break;
        }
        case Strategy::MultiSizeFreeList: {
            if (s->retired) {
                s->live_chunks.fetch_sub(1, std::memory_order_release);
                break;
            }
            uint32_t my_tid = seg::current_tid();
            if (s->owner_tid == my_tid) {
                auto& m = tl_multi_freelists[s->slot_index];
                *static_cast<void**>(ptr) = m.heads[s->class_idx];
                m.heads[s->class_idx] = ptr;
            } else {
                seg::mpsc_push(s, ptr);
            }
            break;
        }
        default: break;
    }
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
