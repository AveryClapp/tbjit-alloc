#include "slow_init.h"
#include "tls.h"
#include <sys/mman.h>
#include <cassert>
#include <atomic>
#include <pthread.h>

namespace tbjit::codegen {

namespace {

constexpr size_t MAX_BUMP_REGIONS = 4096;

struct BumpRegion { uint8_t* base; uint8_t* end; };

BumpRegion           g_regions[MAX_BUMP_REGIONS];
std::atomic<size_t>  g_region_count{0};
pthread_mutex_t      g_region_mutex = PTHREAD_MUTEX_INITIALIZER;

// Append-only registry: slots below g_region_count are immutable once
// published. Writer takes the mutex; readers do a release-acquire load
// of the count and then scan without locking.
void register_region(uint8_t* base, uint8_t* end) {
    pthread_mutex_lock(&g_region_mutex);
    size_t cur = g_region_count.load(std::memory_order_relaxed);
    if (cur < MAX_BUMP_REGIONS) {
        g_regions[cur] = {base, end};
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
    register_region(base, base + BUMP_REGION_SIZE);
    return base;
}

bool is_in_bump_region(const void* ptr) {
    auto* p = static_cast<const uint8_t*>(ptr);
    size_t n = g_region_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        if (p >= g_regions[i].base && p < g_regions[i].end)
            return true;
    }
    return false;
}

} // namespace tbjit::codegen
