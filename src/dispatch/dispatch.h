#pragma once
#include "../common.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tbjit::dispatch {

using RoutineFn = void* (*)(size_t size);

void      init();
void      install(CallSiteID id, RoutineFn fn);
void      revert(CallSiteID id);         // back to generic path
RoutineFn lookup(CallSiteID id);

// Monotonic counter bumped on every install() / revert(). Hot-path
// inline caches read this to detect when their (id, fn) entry is
// stale — a relaxed load on x86 is essentially free and lets callers
// skip the table walk + atomic acquire load when the dispatch state
// hasn't changed since their last lookup.
extern std::atomic<uint64_t> g_generation;

inline uint64_t generation() {
    return g_generation.load(std::memory_order_relaxed);
}

} // namespace tbjit::dispatch
