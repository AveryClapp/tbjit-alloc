#include "deopt.h"
#include "dispatch/dispatch.h"
#include "codegen/codegen.h"
#include "analysis/analysis.h"
#include <atomic>
#include <pthread.h>

// Ownership model:
//   - Allocator threads: only revert dispatch (atomic) and enqueue {id, page}
//     onto a mutex-protected MPSC queue. No summary writes, no epoch bumps.
//   - Background thread: sole writer of g_summaries (via analysis::reset_call_site)
//     and g_pending. Drains the deopt queue and reclaims pages.
//   - mark_safe_point(): updates only the calling thread's epoch — no reclaim,
//     no shared mutable state, no contention on the hot malloc path.

namespace tbjit::deopt {

namespace {

std::atomic<uint64_t> g_epoch{0};

struct PendingReclaim {
    void*    page;
    uint64_t epoch;
};

constexpr size_t MAX_PENDING = 256;
PendingReclaim   g_pending[MAX_PENDING];     // background-thread-only
size_t           g_pending_count = 0;        // background-thread-only

struct DequeuedDeopt {
    CallSiteID            id;
    void*                 page;
    analysis::DeoptReason reason;
};

constexpr size_t DEOPT_QUEUE_CAP = 256;
DequeuedDeopt    g_deopt_queue[DEOPT_QUEUE_CAP];
size_t           g_deopt_head = 0;
size_t           g_deopt_tail = 0;
pthread_mutex_t  g_deopt_mutex = PTHREAD_MUTEX_INITIALIZER;

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

} // namespace

void init() {}

void handle(CallSiteID id, void* code_page, analysis::DeoptReason reason) {
    dispatch::revert(id);
    pthread_mutex_lock(&g_deopt_mutex);
    if (g_deopt_tail - g_deopt_head < DEOPT_QUEUE_CAP) {
        g_deopt_queue[g_deopt_tail % DEOPT_QUEUE_CAP] = {id, code_page, reason};
        ++g_deopt_tail;
    }
    pthread_mutex_unlock(&g_deopt_mutex);
}

void mark_safe_point() {
    if (!tl_registered) register_thread();
    tl_thread_epoch.epoch.store(
        g_epoch.load(std::memory_order_acquire),
        std::memory_order_release);
}

void drain_pending() {
    while (true) {
        pthread_mutex_lock(&g_deopt_mutex);
        if (g_deopt_head == g_deopt_tail) {
            pthread_mutex_unlock(&g_deopt_mutex);
            break;
        }
        DequeuedDeopt d = g_deopt_queue[g_deopt_head % DEOPT_QUEUE_CAP];
        ++g_deopt_head;
        pthread_mutex_unlock(&g_deopt_mutex);

        analysis::reset_call_site(d.id, d.reason);
        uint64_t e = g_epoch.fetch_add(1, std::memory_order_acq_rel);
        if (g_pending_count < MAX_PENDING)
            g_pending[g_pending_count++] = {d.page, e};
    }

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

} // namespace tbjit::deopt
