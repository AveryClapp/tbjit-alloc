#include "tls.h"
#include <atomic>

namespace tbjit::codegen {

thread_local BumpSlot          tl_bumps[MAX_COMPILED_SITES];
thread_local FreeListSlot      tl_freelists[MAX_COMPILED_SITES];
thread_local ArenaSlot         tl_arenas[MAX_COMPILED_SITES];
thread_local MultiFreeListSlot tl_multi_freelists[MAX_COMPILED_SITES];

CallSiteID g_slot_to_site[MAX_COMPILED_SITES] = {};

static std::atomic<uint32_t> g_next_index{0};

uint32_t alloc_slot_index() {
    uint32_t idx = g_next_index.fetch_add(1, std::memory_order_relaxed);
    return (idx < MAX_COMPILED_SITES) ? idx : MAX_COMPILED_SITES;
}

} // namespace tbjit::codegen
