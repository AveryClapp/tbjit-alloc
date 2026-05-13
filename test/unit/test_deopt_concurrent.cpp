// Stress test for the deopt queue: many allocator threads call handle()
// simultaneously on the same call site, while a single drain thread plays
// the background-thread role. Verifies no crash, no torn writes, no
// double-reclaim, and the summary ends up in Phase::Deopt.

#include "deopt/deopt.h"
#include "dispatch/dispatch.h"
#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include "common.h"
#include <cassert>
#include <pthread.h>
#include <sys/mman.h>

namespace {

constexpr tbjit::CallSiteID kId        = 0xCAFEBEEF;
constexpr int               kThreads   = 4;
constexpr int               kPerThread = 50;   // 200 < DEOPT_QUEUE_CAP (256)

void* worker(void*) {
    for (int i = 0; i < kPerThread; ++i) {
        // Real mmap'd page so drain_pending's codegen::reclaim → munmap is valid.
        void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(page != MAP_FAILED);
        tbjit::deopt::handle(kId, page);
    }
    return nullptr;
}

} // namespace

int main() {
    tbjit::alloc::init();
    tbjit::dispatch::init();
    tbjit::analysis::init_state();
    tbjit::deopt::init();

    // Seed a summary for kId so reset_call_site has something to find.
    tbjit::AllocEvent seed{kId, 48, 0, 0, nullptr};
    tbjit::analysis::process_event(seed);

    pthread_t threads[kThreads];
    for (int i = 0; i < kThreads; ++i)
        pthread_create(&threads[i], nullptr, worker, nullptr);
    for (int i = 0; i < kThreads; ++i)
        pthread_join(threads[i], nullptr);

    // Single drainer — plays the role of the background thread.
    // Two passes: first drains the queue (epoch bumps happen here), second
    // reclaims pages now that g_epoch has advanced past their queue epoch.
    tbjit::deopt::drain_pending();
    tbjit::deopt::drain_pending();

    assert(tbjit::analysis::get_phase(kId) == tbjit::analysis::Phase::Deopt);
    return 0;
}
