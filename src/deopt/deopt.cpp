#include "deopt.h"
#include "dispatch/dispatch.h"
#include "codegen/codegen.h"
#include "analysis/analysis.h"
#include <atomic>

namespace tbjit::deopt {

namespace {

// Tracks the current global epoch and per-thread observed epochs.
// A page is safe to reclaim once all threads have passed the epoch
// at which it was invalidated.
std::atomic<uint64_t> g_epoch{0};
thread_local uint64_t tl_epoch = 0;

struct PendingReclaim {
    void*    page;
    uint64_t epoch;
};

constexpr size_t MAX_PENDING = 256;
PendingReclaim   g_pending[MAX_PENDING];
size_t           g_pending_count = 0;

void try_reclaim() {
    // TODO: scan g_pending, free pages whose epoch is globally safe
}

} // namespace

void init() {}

void handle(CallSiteID id, void* code_page) {
    dispatch::revert(id);
    uint64_t e = g_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (g_pending_count < MAX_PENDING)
        g_pending[g_pending_count++] = {code_page, e};
    // Restart analysis from scratch for this call site (Task 6: wire up process_event)
}

void mark_safe_point() {
    tl_epoch = g_epoch.load(std::memory_order_acquire);
    try_reclaim();
}

} // namespace tbjit::deopt
