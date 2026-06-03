#include "analysis/analysis.h"
#include "analysis/ks_test.h"
#include "analysis/futex.h"
#include "alloc/alloc.h"
#include "trace/trace.h"
#include "trace/writer.h"
#include "codegen/codegen.h"
#include "dispatch/dispatch.h"
#include "deopt/deopt.h"
#include "seg/segment.h"
#include <new>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace tbjit::analysis {

CallSiteSummary* g_summaries     = nullptr;
size_t           g_summary_count = 0;

namespace {

constexpr size_t   MAX_CALL_SITES        = 4096;
constexpr uint32_t STABLE_WINDOWS_DEFAULT = 10;
uint32_t           g_stable_windows       = STABLE_WINDOWS_DEFAULT;
constexpr uint32_t WINDOW_SIZE_DEFAULT    = 1000;
uint32_t           g_window_size          = WINDOW_SIZE_DEFAULT;
constexpr double   KS_ALPHA_DEFAULT       = 0.05;
double             g_ks_alpha             = KS_ALPHA_DEFAULT;
constexpr uint32_t DEOPT_BLACKLIST_LIMIT_DEFAULT = 3;  // after this many deopts, stop recompiling
uint32_t           g_deopt_blacklist_limit = DEOPT_BLACKLIST_LIMIT_DEFAULT;
constexpr double   LIFETIME_REAP_RATIO   = 0.50;  // free_count/event_count >= → Reap
constexpr double   LIFETIME_HOLD_RATIO   = 0.10;  // free_count/event_count <  → Hold
constexpr double   PC_TID_CONCENTRATION  = 0.95;  // top_count/total >= → concentrated
constexpr double   MULTI_CLASS_COVERAGE  = 0.90;  // top-K modes must cover this fraction
constexpr size_t   MULTI_CLASS_MAX       = 4;     // max classes to learn per site
constexpr double   PAIRED_PAIR_RATIO     = 0.95;  // pair_count/event_count >= → pair-dominant
constexpr double   PAIRED_LIFO_RATIO     = 0.95;  // lifo_count/pair_count >= → strict LIFO

std::atomic<bool>     g_running{false};
std::atomic<uint64_t> g_events_processed{0};
pthread_t             g_thread;

// Oracle replay mode: when set, check_postspec never deopts a Compiled site
// (models a perfect, never-blacklisting picker). Replay-tool only; off in the
// live allocator. Not atomic: replay is single-threaded.
bool                  g_oracle_mode = false;

// Trace-only capture mode (TBJIT_TRACE_ONLY): when set, no site ever
// specializes — advance_prespec keeps cycling PreSpec windows but never
// compiles/installs, so every alloc stays on the generic (recorded) path and
// the captured trace is a complete, unspecialized event stream. All
// specialization decisions then happen offline in the bound-replay harness.
// Read once in init(); off in the offline tools (which call init_state()
// directly and *want* to compile).
bool                  g_trace_only = false;

// Strategy override: TBJIT_FORCE_STRATEGY={bump,freelist} overrides the
// is_monomorphic-based candidate pick. Read once on first call to avoid
// re-scanning the env on every stable transition.
Strategy strategy_override() {
    static Strategy cached = []() {
        const char* v = std::getenv("TBJIT_FORCE_STRATEGY");
        if (!v) return Strategy::Generic;
        if (std::strcmp(v, "bump") == 0)     return Strategy::BumpAlloc;
        if (std::strcmp(v, "freelist") == 0) return Strategy::ThreadLocalFreeList;
        if (std::strcmp(v, "arena") == 0)    return Strategy::EpochArena;
        return Strategy::Generic;
    }();
    return cached;
}

CallSiteSummary* find_or_create(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return &g_summaries[i];
    if (g_summary_count >= MAX_CALL_SITES) return nullptr;
    CallSiteSummary* s = &g_summaries[g_summary_count++];
    new (s) CallSiteSummary{};
    s->id = id;
    return s;
}

void advance_prespec(CallSiteSummary* s) {
    uint8_t prev = 1 - s->active;
    bool stable = s->windows[prev].count > 0 &&
                  ks_stable(s->windows[s->active].hist,
                             s->windows[prev].hist, g_ks_alpha);
    if (stable) {
        ++s->stable_windows;
        if (s->stable_windows >= g_stable_windows) {
            // Blacklisted sites stay in PreSpec forever — analysis still
            // runs but compile() is skipped. Avoids pathological recompile
            // loops on call sites that keep deopting (truly polymorphic,
            // size-changing over time, etc). Trace-only capture mode rides
            // the same path: never compile, so the generic record path stays
            // live for the entire stream.
            if (s->blacklisted || g_trace_only) {
                s->stable_windows = 0;
                s->active = 1 - s->active;
                s->windows[s->active].reset();
                return;
            }
            s->baseline  = s->windows[s->active].hist;
            // Derive lifetime tag from observed free/alloc ratio. Used by
            // the future reaper to decide which retired segments are safe
            // to munmap eagerly.
            if (s->event_count > 0) {
                double r = static_cast<double>(s->free_count) /
                           static_cast<double>(s->event_count);
                if      (r >= LIFETIME_REAP_RATIO) s->lifetime = LifetimeTag::Reap;
                else if (r <  LIFETIME_HOLD_RATIO) s->lifetime = LifetimeTag::Hold;
                else                                s->lifetime = LifetimeTag::Unknown;
            }
            // Producer-consumer detection: alloc dist concentrated on one
            // thread, free dist concentrated on a *different* thread.
            auto concentrated = [](const ThreadDist& d) {
                return d.total > 0 &&
                       static_cast<double>(d.top_count) /
                       static_cast<double>(d.total) >= PC_TID_CONCENTRATION;
            };
            bool pc_pattern =
                concentrated(s->alloc_dist) &&
                concentrated(s->free_dist) &&
                s->alloc_dist.top_tid != s->free_dist.top_tid;

            // Learn top-K size classes; used by MultiSizeFreeList and
            // recorded for diagnostics/paper figures regardless of strategy.
            ExactHistogram::Mode modes[MULTI_CLASS_MAX];
            size_t nclass = s->windows[s->active].hist.top_k_modes(
                modes, MULTI_CLASS_MAX, MULTI_CLASS_COVERAGE);
            s->class_count = static_cast<uint8_t>(nclass);
            for (size_t i = 0; i < nclass; ++i)
                s->classes[i] = {modes[i].size, modes[i].count};

            // PairedStack: dominant freeing site + frees match LIFO order.
            bool paired_pattern = false;
            if (s->event_count > 0 && s->top_pair.pair_count > 0) {
                double pr = static_cast<double>(s->top_pair.pair_count) /
                            static_cast<double>(s->event_count);
                double lr = static_cast<double>(s->top_pair.lifo_count) /
                            static_cast<double>(s->top_pair.pair_count);
                paired_pattern = pr >= PAIRED_PAIR_RATIO &&
                                 lr >= PAIRED_LIFO_RATIO;
            }

            Strategy forced = strategy_override();
            if (forced != Strategy::Generic) {
                s->candidate = forced;
            } else if (paired_pattern) {
                s->candidate = Strategy::PairedStack;
            } else if (pc_pattern) {
                s->candidate = Strategy::ProducerConsumer;
            } else if (s->windows[s->active].hist.is_monomorphic(0.95)) {
                // BumpAlloc carves one segment per slot and deopts on
                // exhaust — fatal for churn workloads (every alloc paired
                // with a free): after SEGMENT_SIZE/dom_size allocs the
                // site deopts, and after DEOPT_BLACKLIST_LIMIT cycles
                // it's blacklisted forever, falling back to glibc. Only
                // pick BumpAlloc when frees are rare (Hold); otherwise
                // pick TLFreeList so frees recycle. TLFreeList needs at
                // least a pointer-width chunk to thread the free list
                // through; fall back to BumpAlloc below that.
                uint32_t dom = s->windows[s->active].hist.dominant_size();
                if (s->lifetime != LifetimeTag::Hold &&
                    dom >= sizeof(void*)) {
                    s->candidate = Strategy::ThreadLocalFreeList;
                } else {
                    s->candidate = Strategy::BumpAlloc;
                }
            } else if (nclass >= 2) {
                s->candidate = Strategy::MultiSizeFreeList;
            } else {
                s->candidate = Strategy::ThreadLocalFreeList;
            }
            s->phase = Phase::Compiled;
            if (s->first_compile_events == 0)
                s->first_compile_events = s->event_count;
            {
                codegen::RoutineSpec spec{};
                spec.id       = s->id;
                spec.strategy = s->candidate;
                spec.size     = s->windows[s->active].hist.dominant_size();
                if (s->candidate == Strategy::MultiSizeFreeList) {
                    spec.class_count = s->class_count;
                    for (uint8_t i = 0; i < s->class_count; ++i)
                        spec.class_sizes[i] = s->classes[i].size;
                }
                void* routine = codegen::compile(spec);
                if (routine) {
                    dispatch::install(s->id,
                        reinterpret_cast<dispatch::RoutineFn>(routine));
                    s->code_page = routine;
                }
            }
            return;
        }
    } else {
        s->stable_windows = 0;
    }
    s->active = 1 - s->active;
    s->windows[s->active].reset();
}

void check_postspec(CallSiteSummary* s) {
    // Oracle replay: a perfect picker never deopts — keep the site Compiled so
    // its post-warmup events stay captured. Online mode deopts on drift.
    if (g_oracle_mode) { s->post_window.reset(); return; }
    if (!ks_stable(s->post_window.hist, s->baseline, g_ks_alpha)) {
        s->phase = Phase::Deopt;
        // Ground truth: the KS test rejected the post-spec window against the
        // baseline, i.e. the size distribution drifted off the locked-in modes.
        s->deopt_reason = DeoptReason::SizeDrift;
        s->stable_windows = 0;
        s->windows[0].reset();
        s->windows[1].reset();
        s->active = 0;
    }
    s->post_window.reset();
}

bool drain_all() {
    bool found = false;
    tbjit::trace::RingBuffer* rb = tbjit::trace::ring_head();
    while (rb) {
        AllocEvent ev;
        while (rb->pop(ev)) {
            process_event(ev);
            if (tbjit::trace::writer_active())
                tbjit::trace::writer_write(ev);
            g_events_processed.fetch_add(1, std::memory_order_relaxed);
            found = true;
        }
        rb = rb->next.load(std::memory_order_acquire);
    }
    return found;
}

// Used by the reaper to decide which retired segments are safe to munmap.
// Only segments whose alloc_site is tagged Reap get reclaimed.
bool reap_predicate(CallSiteID alloc_site) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == alloc_site)
            return g_summaries[i].lifetime == LifetimeTag::Reap;
    return false;
}

void* background_loop(void*) {
    uint32_t backoff_us = 1;
    while (g_running.load(std::memory_order_acquire)) {
        tbjit::deopt::drain_pending();
        seg::reaper_sweep(reap_predicate);
        if (drain_all()) {
            backoff_us = 1;
            continue;
        }
        if (backoff_us < 500) {
            struct timespec ts{0, static_cast<long>(backoff_us) * 1000L};
            nanosleep(&ts, nullptr);
            backoff_us = (backoff_us * 2 < 500) ? backoff_us * 2 : 500;
        } else {
            // Re-check g_running before parking: stop_background_thread() may
            // have set it false and called futex_wake() before we reached here,
            // which would cause us to sleep with no further wakeup pending.
            if (!g_running.load(std::memory_order_acquire)) break;
            futex_wait();
        }
    }
    tbjit::deopt::drain_pending();
    drain_all(); // final drain on shutdown
    return nullptr;
}

} // namespace

uint32_t stable_windows_threshold() { return g_stable_windows; }
uint32_t window_size()              { return g_window_size; }
double   ks_alpha()                 { return g_ks_alpha; }
uint32_t deopt_blacklist_limit()    { return g_deopt_blacklist_limit; }

void init() {
    alloc::init();
    // Picker knobs are env-overridable for the sensitivity sweep; reset each to
    // its default first so an override does not persist across re-inits.
    g_stable_windows = STABLE_WINDOWS_DEFAULT;
    if (const char* v = std::getenv("TBJIT_STABLE_WINDOWS")) {
        uint32_t n = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if (n > 0) g_stable_windows = n;
    }
    g_window_size = WINDOW_SIZE_DEFAULT;
    if (const char* v = std::getenv("TBJIT_WINDOW_SIZE")) {
        uint32_t n = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if (n > 0) g_window_size = n;
    }
    g_ks_alpha = KS_ALPHA_DEFAULT;
    if (const char* v = std::getenv("TBJIT_KS_ALPHA")) {
        double a = std::strtod(v, nullptr);
        if (a > 0.0 && a < 1.0) g_ks_alpha = a;
    }
    g_deopt_blacklist_limit = DEOPT_BLACKLIST_LIMIT_DEFAULT;
    if (const char* v = std::getenv("TBJIT_DEOPT_LIMIT")) {
        uint32_t n = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if (n > 0) g_deopt_blacklist_limit = n;
    }
    g_trace_only = (std::getenv("TBJIT_TRACE_ONLY") != nullptr);
    init_state();
}

void init_state() {
    if (!g_summaries) {
        // The summary array is ~256 MiB for MAX_CALL_SITES=4096 — too large
        // for the 4 MiB internal bump arena. Use mmap directly.
        const size_t sz = sizeof(CallSiteSummary) * MAX_CALL_SITES;
        void* mem = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        g_summaries = static_cast<CallSiteSummary*>(mem);
    }
    reset_state();
}

void reset_state() {
    if (!g_summaries) return;  // init_state not yet called
    // Only clear the used slots (mmap pages already zero-initialized at first use)
    for (size_t i = 0; i < g_summary_count; ++i)
        new (&g_summaries[i]) CallSiteSummary{};
    g_summary_count = 0;
}

void process_event(const AllocEvent& ev) {
    // Free events (size == 0) carry the freeing call site in ev.call_site
    // and the freed pointer in ev.ptr. Attribute the free back to the
    // *allocating* site via the segment header.
    if (ev.size == 0) {
        if (!ev.ptr) return;
        // Find the allocating-site summary. For seg-managed allocs (post-JIT)
        // the segment header tells us directly; for pre-JIT (glibc) allocs
        // we scan summaries' lifo_stack tops looking for a match.
        CallSiteSummary* alloc_s = nullptr;
        seg::SegmentHeader* h = seg::of(ev.ptr);
        const bool seg_managed = seg::is_managed(h);
        if (seg_managed) {
            alloc_s = find_or_create(h->alloc_site);
        } else {
            for (size_t i = 0; i < g_summary_count; ++i) {
                CallSiteSummary& cs = g_summaries[i];
                if (cs.lifo_head > 0 &&
                    cs.lifo_stack[cs.lifo_head - 1] == ev.ptr) {
                    alloc_s = &cs;
                    break;
                }
            }
        }
        if (!alloc_s) return;

        ++alloc_s->free_count;
        alloc_s->free_dist.record(ev.thread_id);

        // JIT'd allocs never appear in the trace (trampoline only records
        // on the generic branch), so their ptrs were never pushed onto
        // lifo_stack — the LIFO match below can't fire for them. Updating
        // pair_count from these frees without a matching lifo_count
        // opportunity would skew the paired_pattern ratio on the next
        // compile cycle (the bug that pushed monomorphic workloads from
        // PairedStack into BumpAlloc → exhaust → blacklist). Skip top_pair
        // updates for seg-managed frees; the next PreSpec cycle's
        // alloc-side observations will drive the picker.
        if (seg_managed) return;

        // Track dominant freeing site via Boyer-Moore majority.
        if (alloc_s->top_pair.pair_count == 0 ||
            alloc_s->top_pair.free_site == ev.call_site) {
            alloc_s->top_pair.free_site = ev.call_site;
            ++alloc_s->top_pair.pair_count;
        } else if (--alloc_s->top_pair.pair_count == 0) {
            alloc_s->top_pair.free_site = ev.call_site;
            alloc_s->top_pair.pair_count = 1;
            alloc_s->top_pair.lifo_count = 0;
        }

        // LIFO discipline: did the freed ptr match the top of the stack?
        if (alloc_s->lifo_head > 0 &&
            alloc_s->lifo_stack[alloc_s->lifo_head - 1] == ev.ptr) {
            --alloc_s->lifo_head;
            if (alloc_s->top_pair.free_site == ev.call_site)
                ++alloc_s->top_pair.lifo_count;
        }
        return;
    }

    if (ev.size >= ExactHistogram::MAX_SIZE) return;
    CallSiteSummary* s = find_or_create(ev.call_site);
    if (!s) return;
    ++s->event_count;
    s->alloc_dist.record(ev.thread_id);

    // Track this allocation on the per-site LIFO ring (oldest entry is
    // overwritten on overflow). Used by PairedStack detection.
    if (ev.ptr) {
        if (s->lifo_head < 16) {
            s->lifo_stack[s->lifo_head++] = ev.ptr;
        } else {
            // ring: shift down by 1
            for (int i = 0; i < 15; ++i)
                s->lifo_stack[i] = s->lifo_stack[i + 1];
            s->lifo_stack[15] = ev.ptr;
        }
    }

    if (s->phase == Phase::Deopt) s->phase = Phase::PreSpec;

    if (s->phase == Phase::PreSpec) {
        s->windows[s->active].record(ev.size);
        if (s->windows[s->active].full()) advance_prespec(s);
    } else {
        s->post_window.record(ev.size);
        if (s->post_window.full()) check_postspec(s);
    }
}

Phase get_phase(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return g_summaries[i].phase;
    return Phase::PreSpec;
}

Strategy get_candidate_strategy(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return g_summaries[i].candidate;
    return Strategy::Generic;
}

LifetimeTag get_lifetime_tag(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return g_summaries[i].lifetime;
    return LifetimeTag::Unknown;
}

DeoptReason get_deopt_reason(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return g_summaries[i].deopt_reason;
    return DeoptReason::None;
}

void set_oracle_mode(bool on) { g_oracle_mode = on; }
bool oracle_mode()            { return g_oracle_mode; }
bool trace_only()             { return g_trace_only; }

OracleResult capturable() {
    OracleResult r{0, 0};
    for (size_t i = 0; i < g_summary_count; ++i) {
        r.total_events += g_summaries[i].event_count;
        // A perfect (never-blacklisting) picker captures every event of any
        // site that reached a strategy decision. In oracle replay drift never
        // deopts, so such sites stay Compiled through to exit.
        if (g_summaries[i].phase == Phase::Compiled)
            r.captured_events += g_summaries[i].event_count;
    }
    return r;
}

void reset_call_site(CallSiteID id, DeoptReason reason) {
    for (size_t i = 0; i < g_summary_count; ++i) {
        if (g_summaries[i].id == id) {
            ++g_summaries[i].deopt_count;
            if (g_summaries[i].deopt_count >= g_deopt_blacklist_limit)
                g_summaries[i].blacklisted = true;
            // Record the ground-truth cause the JIT deopt path passed in. Keep
            // any specific reason already set (e.g. SizeDrift from
            // check_postspec) rather than overwriting it with a generic Other.
            if (reason != DeoptReason::Other ||
                g_summaries[i].deopt_reason == DeoptReason::None)
                g_summaries[i].deopt_reason = reason;
            g_summaries[i].phase = Phase::Deopt;
            g_summaries[i].code_page = nullptr;
            g_summaries[i].stable_windows = 0;
            g_summaries[i].windows[0].reset();
            g_summaries[i].windows[1].reset();
            g_summaries[i].active = 0;
            return;
        }
    }
}

void run() {
    start_background_thread();
}

void start_background_thread() {
    g_running.store(true, std::memory_order_release);
    pthread_create(&g_thread, nullptr, background_loop, nullptr);
}

void stop_background_thread() {
    g_running.store(false, std::memory_order_release);
    futex_wake();
    pthread_join(g_thread, nullptr);
}

uint64_t events_processed() {
    return g_events_processed.load(std::memory_order_relaxed);
}

size_t summary_count() { return g_summary_count; }

} // namespace tbjit::analysis
