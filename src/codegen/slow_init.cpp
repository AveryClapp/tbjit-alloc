#include "slow_init.h"
#include "tls.h"
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
    void* mem = mmap(nullptr, BUMP_REGION_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);

    uint8_t* base = static_cast<uint8_t*>(mem);
    tl_bumps[index].ptr = base + size;
    tl_bumps[index].end = base + BUMP_REGION_SIZE;
    register_region(base, base + BUMP_REGION_SIZE, RegionKind::Bump, 0);
    return base;
}

void* freelist_refill(uint32_t index, uint32_t obj_size) {
    // Each free-list chunk's first 8 bytes hold the `next` pointer while
    // the chunk is on the list; user data overwrites that on allocation
    // and the caller restores it on free.
    assert(obj_size >= sizeof(void*) && "free-list chunk must hold a pointer");

    void* mem = mmap(nullptr, FREELIST_REGION_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);

    uint8_t* base = static_cast<uint8_t*>(mem);
    uint8_t* end  = base + FREELIST_REGION_SIZE;

    void* head = nullptr;
    for (uint8_t* p = base; p + obj_size <= end; p += obj_size) {
        *reinterpret_cast<void**>(p) = head;
        head = p;
    }
    register_region(base, end, RegionKind::FreeList, index);

    // Pop one chunk to return; install the rest as the live free list.
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
    void* mem = mmap(nullptr, ARENA_REGION_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);

    uint8_t* base = static_cast<uint8_t*>(mem);
    tl_arenas[index].base = base;
    tl_arenas[index].ptr  = base + size;
    tl_arenas[index].end  = base + ARENA_REGION_SIZE;
    // Register as Bump-kind so the free interceptor drops these pointers
    // on the floor — same lifetime semantics as bump (epoch-scoped, no free).
    register_region(base, base + ARENA_REGION_SIZE, RegionKind::Bump, 0);
    return base;
}

uint8_t* arena_reset_alloc(uint32_t index, uint32_t size) {
    uint8_t* base = tl_arenas[index].base;
    if (!base) return arena_slow_init(index, size);  // first call: same as init
    tl_arenas[index].ptr = base + size;
    return base;
}

} // namespace tbjit::codegen
