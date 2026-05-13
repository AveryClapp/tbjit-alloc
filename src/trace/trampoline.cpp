#include "trace.h"
#include "trace/writer.h"
#include "dispatch/dispatch.h"
#include "deopt/deopt.h"
#include "shadow/shadow.h"
#include "analysis/analysis.h"
#include "codegen/tls.h"
#include "seg/segment.h"
#include "common.h"
#include <atomic>
#include <cstdlib>
#include <dlfcn.h>
#include <pthread.h>

// LD_PRELOAD entry points. These intercept every malloc/free call
// in the target process and route them through the dispatch table.
// reentrancy_guard prevents recursive interposition when tbjit
// internals legitimately call the real allocator (they shouldn't,
// but this is a safety net during development).

// g_real_malloc is extern so codegen.cpp can embed its address in JIT stubs.
void* (*g_real_malloc)(size_t) = nullptr;

std::atomic<uint64_t> g_jit_allocs{0};
std::atomic<uint64_t> g_generic_allocs{0};

namespace {

thread_local bool reentrancy_guard = false;

using free_fn = void (*)(void*);

free_fn real_free = nullptr;

__attribute__((constructor))
void tbjit_init() {
    g_real_malloc = reinterpret_cast<void* (*)(size_t)>(dlsym(RTLD_NEXT, "malloc"));
    real_free      = reinterpret_cast<free_fn>(dlsym(RTLD_NEXT, "free"));
    tbjit::trace::init();
    tbjit::dispatch::init();
    tbjit::deopt::init();
    tbjit::analysis::init();
    tbjit::analysis::start_background_thread();
    const char* trace_path = getenv("TBJIT_TRACE");
    if (trace_path) tbjit::trace::writer_open(trace_path);
}

__attribute__((destructor))
void tbjit_fini() {
    tbjit::analysis::stop_background_thread();
    tbjit::trace::writer_close();
    if (getenv("TBJIT_DUMP"))
        tbjit::analysis::dump_stats();
}

} // namespace

extern "C" {

void* malloc(size_t size) {
    if (!g_real_malloc) return nullptr;  // pre-init: dlsym not yet complete
    if (reentrancy_guard) return g_real_malloc(size);
    reentrancy_guard = true;
    tbjit::deopt::mark_safe_point();

    void* ra = __builtin_return_address(0);
    tbjit::CallSiteID id = tbjit::hash_return_addr(ra);

    tbjit::dispatch::RoutineFn fn = tbjit::dispatch::lookup(id);
    void* ptr;
    if (__builtin_expect(fn != nullptr, 1)) {
        g_jit_allocs.fetch_add(1, std::memory_order_relaxed);
        ptr = fn(size);
    } else {
        g_generic_allocs.fetch_add(1, std::memory_order_relaxed);
        ptr = g_real_malloc(size);
        tbjit::trace::record_alloc(id, size, ptr);
    }

    tbjit::shadow::validate_alloc(id, size, ptr);
    reentrancy_guard = false;
    return ptr;
}

void free(void* ptr) {
    if (reentrancy_guard || !real_free) { real_free(ptr); return; }
    reentrancy_guard = true;

    void* ra = __builtin_return_address(0);
    tbjit::CallSiteID id = tbjit::hash_return_addr(ra);
    tbjit::trace::record_free(id, ptr);
    tbjit::shadow::validate_free(id, ptr);

    if (ptr) {
        tbjit::seg::SegmentHeader* s = tbjit::seg::of(ptr);
        if (tbjit::seg::is_managed(s)) {
            switch (s->strategy) {
                case tbjit::Strategy::BumpAlloc:
                case tbjit::Strategy::EpochArena:
                    break;  // chunks live until segment reclaim
                case tbjit::Strategy::ThreadLocalFreeList: {
                    // Route by chunk's owning thread. Foreign frees go onto
                    // the segment's MPSC remote queue; the owner thread
                    // harvests on refill.
                    uint32_t my_tid = tbjit::seg::current_tid();
                    if (s->owner_tid == my_tid) {
                        *static_cast<void**>(ptr) =
                            tbjit::codegen::tl_freelists[s->slot_index].head;
                        tbjit::codegen::tl_freelists[s->slot_index].head = ptr;
                    } else {
                        tbjit::seg::mpsc_push(s, ptr);
                    }
                    break;
                }
                default: break;
            }
        } else {
            real_free(ptr);
        }
    }

    reentrancy_guard = false;
}

} // extern "C"
