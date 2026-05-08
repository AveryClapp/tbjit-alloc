#include "deopt.h"
#include "dispatch/dispatch.h"
#include "codegen/codegen.h"
#include "analysis/analysis.h"
#include <atomic>

// Prototype limitation: g_pending/g_pending_count and the CallSiteSummary
// fields accessed via reset_call_site() are not protected by a mutex.
// handle() may race with the analysis background thread and with concurrent
// deopt from other allocator threads. Production would require per-summary
// locks or a lock-free epoch-based design for the summary updates.

namespace tbjit::deopt {

namespace {

std::atomic<uint64_t> g_epoch{0};

struct PendingReclaim {
    void*    page;
    uint64_t epoch;
};

constexpr size_t MAX_PENDING = 256;
PendingReclaim   g_pending[MAX_PENDING];
size_t           g_pending_count = 0;

struct ThreadEpoch {
    std::atomic<uint64_t>     epoch{0};
    std::atomic<ThreadEpoch*> next{nullptr};
};

std::atomic<ThreadEpoch*> g_thread_list{nullptr};
thread_local ThreadEpoch  tl_thread_epoch;
thread_local bool         tl_registered{false};

void register_thread() {
    ThreadEpoch* head = g_thread_list.load(std::memory_order_acquire);
    do {
        tl_thread_epoch.next.store(head, std::memory_order_relaxed);
    } while (!g_thread_list.compare_exchange_weak(
                 head, &tl_thread_epoch,
                 std::memory_order_release,
                 std::memory_order_acquire));
    tl_registered = true;
}

void try_reclaim() {
    if (g_pending_count == 0) return;

    uint64_t min_epoch = g_epoch.load(std::memory_order_acquire);
    for (ThreadEpoch* t = g_thread_list.load(std::memory_order_acquire);
         t; t = t->next.load(std::memory_order_acquire)) {
        uint64_t te = t->epoch.load(std::memory_order_acquire);
        if (te < min_epoch) min_epoch = te;
    }

    size_t out = 0;
    for (size_t i = 0; i < g_pending_count; ++i) {
        if (g_pending[i].epoch < min_epoch) {
            codegen::reclaim(g_pending[i].page);
        } else {
            g_pending[out++] = g_pending[i];
        }
    }
    g_pending_count = out;
}

} // namespace

void init() {}

void handle(CallSiteID id, void* code_page) {
    dispatch::revert(id);
    uint64_t e = g_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (g_pending_count < MAX_PENDING)
        g_pending[g_pending_count++] = {code_page, e};
    analysis::reset_call_site(id);
}

void mark_safe_point() {
    if (!tl_registered) register_thread();
    tl_thread_epoch.epoch.store(
        g_epoch.load(std::memory_order_acquire),
        std::memory_order_release);
    try_reclaim();
}

} // namespace tbjit::deopt
