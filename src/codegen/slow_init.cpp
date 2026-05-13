#include "slow_init.h"
#include "tls.h"
#include "seg/segment.h"
#include <sys/mman.h>
#include <cassert>
#include <atomic>
#include <pthread.h>

namespace tbjit::codegen {

namespace {

constexpr size_t MAX_REGIONS = 4096;

struct Region {
    uint8_t*   base;
    uint8_t*   end;
    RegionKind kind;
    uint32_t   slot_index;   // valid for FreeList
};

Region              g_regions[MAX_REGIONS];
std::atomic<size_t> g_region_count{0};
pthread_mutex_t     g_region_mutex = PTHREAD_MUTEX_INITIALIZER;

// Append-only registry. Slots below g_region_count are immutable once
// published. Writers serialize on the mutex; readers do an acquire-load
// of the count and then scan without locking.
void register_region(uint8_t* base, uint8_t* end,
                     RegionKind kind, uint32_t slot_index) {
    pthread_mutex_lock(&g_region_mutex);
    size_t cur = g_region_count.load(std::memory_order_relaxed);
    if (cur < MAX_REGIONS) {
        g_regions[cur] = {base, end, kind, slot_index};
        g_region_count.store(cur + 1, std::memory_order_release);
    }
    pthread_mutex_unlock(&g_region_mutex);
}

} // namespace

uint8_t* bump_slow_init(uint32_t index, uint32_t size) {
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::BumpAlloc, index, /*site=*/0, size);
    assert(s);
    uint8_t* base = seg::payload_start(s);
    tl_bumps[index].ptr = base + size;
    tl_bumps[index].end = seg::segment_end(s);
    s->bump_ptr  = base + size;
    return base;
}

void* freelist_refill(uint32_t index, uint32_t obj_size) {
    assert(obj_size >= sizeof(void*) && "free-list chunk must hold a pointer");

    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::ThreadLocalFreeList, index, /*site=*/0, obj_size);
    assert(s);
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

bool find_region(const void* ptr, RegionInfo* info_out) {
    auto* p = static_cast<const uint8_t*>(ptr);
    size_t n = g_region_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        if (p >= g_regions[i].base && p < g_regions[i].end) {
            if (info_out) {
                info_out->kind       = g_regions[i].kind;
                info_out->slot_index = g_regions[i].slot_index;
            }
            return true;
        }
    }
    return false;
}

bool is_in_bump_region(const void* ptr) {
    RegionInfo info{};
    return find_region(ptr, &info) && info.kind == RegionKind::Bump;
}

uint8_t* arena_slow_init(uint32_t index, uint32_t size) {
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::EpochArena, index, /*site=*/0, size);
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
