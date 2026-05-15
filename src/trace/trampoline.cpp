#include "trace.h"
#include "trace/writer.h"
#include "dispatch/dispatch.h"
#include "deopt/deopt.h"
#include "shadow/shadow.h"
#include "analysis/analysis.h"
#include "codegen/tls.h"
#include "codegen/slow_init.h"
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

thread_local bool     reentrancy_guard = false;
// Sampled safe-point counter: marks a safe point every 32nd malloc. The
// epoch reclaimer just needs threads to check in "often enough" for
// pages-pending-reclamation to be freed eventually. Skipping the atomic
// load+store on 31/32 calls drops a measurable slice of trampoline
// overhead on tight alloc/free loops. Free does not mark a safe point
// (it never did) so the every-32-mallocs cadence is the only check-in.
thread_local uint32_t tl_safe_point_counter = 0;

// Single-entry inline cache for dispatch::lookup. The vast majority of
// hot programs hammer one (or a small set of) call site(s) in tight
// loops; this skips the dispatch hash-table walk + atomic-acquire load
// on every malloc when the previous lookup is still valid.
// Validity check: the global dispatch generation hasn't changed since
// we cached. dispatch bumps it on every install/revert (rare), so the
// check is a single relaxed atomic load on the hot path.
thread_local tbjit::CallSiteID        tl_ic_id  = 0;
thread_local tbjit::dispatch::RoutineFn tl_ic_fn = nullptr;
thread_local uint64_t                 tl_ic_gen = 0;
// (dispatch::g_generation is initialized to 1, so the first read here
// always mismatches tl_ic_gen and forces a real lookup. After that
// every install/revert bumps the global generation and invalidates all
// per-thread caches uniformly — no per-thread invalidation broadcast.)

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
    if ((++tl_safe_point_counter & 31) == 0)
        tbjit::deopt::mark_safe_point();

    void* ra = __builtin_return_address(0);
    tbjit::CallSiteID id = tbjit::hash_return_addr(ra);

    uint64_t cur_gen = tbjit::dispatch::generation();
    tbjit::dispatch::RoutineFn fn;
    if (__builtin_expect(id == tl_ic_id && cur_gen == tl_ic_gen, 1)) {
        fn = tl_ic_fn;
    } else {
        fn = tbjit::dispatch::lookup(id);
        tl_ic_id  = id;
        tl_ic_fn  = fn;
        tl_ic_gen = cur_gen;
    }
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
                case tbjit::Strategy::ProducerConsumer:
                    // Active segment: foreign frees push to MPSC for the
                    // refill path to drain on retire. Retired segment:
                    // decrement live_chunks; reaper reclaims at zero.
                    if (s->retired) {
                        s->live_chunks.fetch_sub(
                            1, std::memory_order_release);
                    } else {
                        tbjit::seg::mpsc_push(s, ptr);
                    }
                    break;
                case tbjit::Strategy::PairedStack: {
                    // LIFO rewind via the TLS slot the JIT fast path
                    // actually reads — seg->bump_ptr alone is a stale
                    // mirror and rewinding it wouldn't recycle the chunk.
                    // Same-thread only: cross-thread frees can't touch
                    // the owner's TLS. PairedStack's detection rule
                    // requires concentrated alloc/free on one site pair,
                    // so cross-thread frees are rare; drop those chunks.
                    if (s->owner_tid == tbjit::seg::current_tid())
                        tbjit::codegen::paired_lifo_rewind(s, ptr);
                    break;
                }
                case tbjit::Strategy::ThreadLocalFreeList: {
                    if (s->retired) {
                        s->live_chunks.fetch_sub(
                            1, std::memory_order_release);
                        break;
                    }
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
                case tbjit::Strategy::MultiSizeFreeList: {
                    if (s->retired) {
                        s->live_chunks.fetch_sub(
                            1, std::memory_order_release);
                        break;
                    }
                    uint32_t my_tid = tbjit::seg::current_tid();
                    if (s->owner_tid == my_tid) {
                        auto& m = tbjit::codegen::tl_multi_freelists[s->slot_index];
                        *static_cast<void**>(ptr) = m.heads[s->class_idx];
                        m.heads[s->class_idx] = ptr;
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
