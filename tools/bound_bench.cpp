// Trace-driven bound-replay benchmark (Comparison A of the 2026-06-02
// bound-replay plan). Replays a recorded allocation op stream against ONE
// backend with zero LD_PRELOAD interposition, isolating the specialized-
// allocator benefit from the trampoline dispatch tax.
//
//   glibc  — alloc -> malloc(size); free -> free(live).  Pure libc baseline.
//   bound  — offline learning pass populates dispatch via process_event, then
//            timing pass: alloc -> dispatch::lookup(id); fn(size) if compiled
//            else g_real_malloc; free -> codegen::free_managed if seg-managed
//            else free. No trampoline preamble. This is the headline.
//   sim    — bound + the trampoline malloc preamble (reentrancy guard, sampled
//            safe-point, dispatch generation + single-entry inline cache). It
//            cannot replay __builtin_return_address + hash_return_addr (the
//            harness already holds the site id), so it is a LOWER BOUND on the
//            real per-call tax and is labeled as such.
//
// trampoline.cpp is intentionally NOT linked into this tool: we need pure libc
// malloc/free for the glibc baseline and zero self-interception during timing.
// We supply the few globals the rest of tbjit expects the trampoline to define.
//
// Usage:
//   tbjit_bound_bench <trace> [--backend glibc|bound|sim] [--passes N]
//                     [--label NAME] [--tsv-header]
//
// Emits one TSV row to stdout:
//   backend  workload  p50_ns  ci95_ns  peak_rss_kb  matched_free_frac
// where p50_ns / ci95_ns are per-ALLOCATION wall-clock latency. Diagnostics go
// to stderr. RSS is the getrusage(ru_maxrss) high-watermark delta around the
// run; run one backend per process so the watermark is not contaminated by a
// prior backend.

#include "analysis/analysis.h"
#include "dispatch/dispatch.h"
#include "deopt/deopt.h"
#include "codegen/slow_init.h"
#include "seg/segment.h"
#include "alloc/alloc.h"
#include "trace/writer.h"  // TraceHeader, TRACE_MAGIC, TRACE_VERSION
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>
#include <execinfo.h>
#include <sys/resource.h>
#include <unistd.h>

// --- trampoline-shim globals (trampoline.cpp excluded from this tool) -------
// codegen embeds g_real_malloc as the JIT routines' fallback and the bound/sim
// backends call it for non-compiled sites; dump references the alloc counters.
// Initialized to libc malloc here (no trampoline links into this tool, so
// malloc IS libc) so it is valid before the learning pass compiles anything.
void* (*g_real_malloc)(size_t) = std::malloc;
std::atomic<uint64_t> g_jit_allocs{0};
std::atomic<uint64_t> g_generic_allocs{0};

using namespace tbjit;

namespace {

enum class Backend { Glibc, Bound, Sim };

enum class OpKind { Alloc, Free };
struct Op {
    OpKind                kind;
    CallSiteID            id;    // alloc only
    uint32_t              size;  // alloc only
    uint32_t              slot;  // live[] index: alloc fills it, free reads it
    dispatch::RoutineFn   fn;    // alloc only, bound backend: pre-resolved
                                 // routine (nullptr => not compiled => libc)
};

bool read_trace(const char* path, std::vector<AllocEvent>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "bound_bench: open %s failed\n", path); return false; }
    trace::TraceHeader hdr{};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "bound_bench: short read on header\n"); fclose(f); return false;
    }
    if (hdr.magic != trace::TRACE_MAGIC) {
        fprintf(stderr, "bound_bench: bad magic 0x%08x\n", hdr.magic); fclose(f); return false;
    }
    if (hdr.version != trace::TRACE_VERSION) {
        fprintf(stderr, "bound_bench: version mismatch (file=%u tool=%u)\n",
                hdr.version, trace::TRACE_VERSION);
        fclose(f); return false;
    }
    AllocEvent ev{};
    while (fread(&ev, sizeof(ev), 1, f) == 1) out.push_back(ev);
    fclose(f);
    return true;
}

// Resolve the recorded op stream into index-addressed ops (alloc -> live slot,
// free -> the slot of its matching alloc). Pointer remapping happens here, once,
// outside the timed loop, so the timing pass is pure array indexing. The
// orig->slot map mirrors process_event's convention: size==0 is a free (so are
// malloc(0) records, which the analyzer also treats as frees).
struct Resolved {
    std::vector<Op>       ops;
    std::vector<uint32_t> leaked;  // slots allocated but never freed in-trace
    uint32_t              n_allocs       = 0;
    uint64_t              total_frees    = 0;
    uint64_t              matched_frees  = 0;
};

Resolved resolve(const std::vector<AllocEvent>& events) {
    Resolved r;
    std::vector<char> freed;  // freed[slot] == 1 once a Free op references it
    std::unordered_map<const void*, uint32_t> live_index;
    live_index.reserve(events.size());
    for (const AllocEvent& ev : events) {
        if (ev.size != 0) {  // alloc
            uint32_t slot = r.n_allocs++;
            r.ops.push_back({OpKind::Alloc, ev.call_site, ev.size, slot, nullptr});
            freed.push_back(0);
            if (ev.ptr) live_index[ev.ptr] = slot;  // overwrite stale (reuse)
        } else {             // free
            ++r.total_frees;
            auto it = live_index.find(ev.ptr);
            if (it != live_index.end()) {
                r.ops.push_back({OpKind::Free, 0, 0, it->second, nullptr});
                freed[it->second] = 1;
                live_index.erase(it);
                ++r.matched_frees;
            }
            // unmatched free (pre-capture alloc, or dropped on ring overflow):
            // skip — never fabricate. Counted via total_frees - matched_frees.
        }
    }
    for (uint32_t s = 0; s < r.n_allocs; ++s)
        if (!freed[s]) r.leaked.push_back(s);
    return r;
}

// --- backend alloc/free -----------------------------------------------------

inline void* glibc_alloc(CallSiteID, size_t size) { return malloc(size); }

// Trampoline malloc preamble, minus the return-address hash (the harness holds
// the id). Lower-bound per-call tax. Mirrors trampoline.cpp's malloc body.
thread_local bool                sim_guard    = false;
thread_local CallSiteID          sim_ic_id    = 0;
thread_local dispatch::RoutineFn sim_ic_fn    = nullptr;
thread_local uint64_t            sim_ic_gen   = 0;
thread_local uint32_t            sim_safe_ctr = 0;

inline void* sim_alloc(CallSiteID id, size_t size) {
    if (sim_guard) return g_real_malloc(size);
    sim_guard = true;
    if ((++sim_safe_ctr & 31) == 0) deopt::mark_safe_point();
    uint64_t cur_gen = dispatch::generation();
    dispatch::RoutineFn fn;
    if (id == sim_ic_id && cur_gen == sim_ic_gen) {
        fn = sim_ic_fn;
    } else {
        fn = dispatch::lookup(id);
        sim_ic_id = id; sim_ic_fn = fn; sim_ic_gen = cur_gen;
    }
    void* ptr = fn ? fn(size) : g_real_malloc(size);
    sim_guard = false;
    return ptr;
}

// Free a live pointer: seg-managed chunks go back through the shared managed-
// free helper (same path the trampoline uses); libc pointers go to libc.
inline void managed_aware_free(void* p) {
    if (!p) return;
    seg::SegmentHeader* s = seg::of(p);
    if (seg::is_managed(s)) codegen::free_managed(s, p);
    else                    free(p);
}

inline void run_pass(Backend backend, const std::vector<Op>& ops,
                     std::vector<void*>& live) {
    switch (backend) {
        case Backend::Glibc:
            for (const Op& op : ops) {
                if (op.kind == OpKind::Alloc) live[op.slot] = glibc_alloc(op.id, op.size);
                else                          free(live[op.slot]);
            }
            break;
        case Backend::Bound:
            // Pre-resolved direct call to the specialized routine — no per-call
            // dispatch::lookup, so this isolates the pure allocator-level cost
            // (the ceiling a static-linked deployment could reach). The dispatch
            // tax is measured separately by the sim backend.
            for (const Op& op : ops) {
                if (op.kind == OpKind::Alloc)
                    live[op.slot] = op.fn ? op.fn(op.size) : g_real_malloc(op.size);
                else
                    managed_aware_free(live[op.slot]);
            }
            break;
        case Backend::Sim:
            for (const Op& op : ops) {
                if (op.kind == OpKind::Alloc) live[op.slot] = sim_alloc(op.id, op.size);
                else                          managed_aware_free(live[op.slot]);
            }
            break;
    }
}

// Free the allocations that the trace never frees (leaked slots). Run untimed
// between passes so each timed pass measures a recycled steady state instead of
// accumulating ~one-pass-of-leak per iteration (perl leaks millions/pass).
inline void reclaim_leaked(Backend backend, const std::vector<uint32_t>& leaked,
                           std::vector<void*>& live) {
    for (uint32_t s : leaked) {
        if (!live[s]) continue;
        if (backend == Backend::Glibc) free(live[s]);
        else                           managed_aware_free(live[s]);
        live[s] = nullptr;
    }
}

#if defined(__linux__) && defined(__x86_64__)
// Serializing-ish timestamp for per-op profiling. The rdtscp overhead is a
// near-constant added to every bracket, so it cancels when comparing alloc vs
// free within a backend and across backends. Localization, not absolute timing.
inline uint64_t rdtscp_now() {
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// Phase-1 evidence: split one pass into alloc-cycles vs free-cycles so we can
// see WHERE the bound overhead lives (alloc routine vs free path / is_managed),
// plus the exact segment count (is_managed scan length) and the fraction of
// allocs actually served by a JIT routine vs falling back to libc.
void profile_pass(Backend backend, const std::vector<Op>& ops,
                  std::vector<void*>& live, const char* backend_name,
                  const char* label) {
    uint64_t a_cyc = 0, f_cyc = 0, a_n = 0, f_n = 0, jit_n = 0;
    for (const Op& op : ops) {
        if (op.kind == OpKind::Alloc) {
            uint64_t t0 = rdtscp_now();
            void* p;
            switch (backend) {
                case Backend::Glibc: p = glibc_alloc(op.id, op.size); break;
                case Backend::Bound: p = op.fn ? op.fn(op.size)
                                               : g_real_malloc(op.size); break;
                default:             p = sim_alloc(op.id, op.size); break;
            }
            uint64_t t1 = rdtscp_now();
            live[op.slot] = p;
            a_cyc += t1 - t0; ++a_n; if (op.fn) ++jit_n;
        } else {
            void* p = live[op.slot];
            uint64_t t0 = rdtscp_now();
            if (backend == Backend::Glibc) free(p);
            else                           managed_aware_free(p);
            uint64_t t1 = rdtscp_now();
            f_cyc += t1 - t0; ++f_n;
        }
    }
    fprintf(stderr,
        "PROFILE %s %s alloc=%.1f cyc/op free=%.1f cyc/op segments=%zu "
        "jit_served=%.3f\n",
        backend_name, label,
        a_n ? static_cast<double>(a_cyc) / static_cast<double>(a_n) : 0.0,
        f_n ? static_cast<double>(f_cyc) / static_cast<double>(f_n) : 0.0,
        tbjit::seg::segment_count(),
        a_n ? static_cast<double>(jit_n) / static_cast<double>(a_n) : 0.0);
}
#endif

long peak_rss_kb() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return ru.ru_maxrss / 1024;  // Darwin reports bytes
#else
    return ru.ru_maxrss;         // Linux reports kilobytes
#endif
}

// Current (not high-water) resident set. ru_maxrss is useless for the one-pass
// footprint because the multi-GB learning pass already set the process peak;
// current RSS drops after we free the event buffer, then rises with the
// allocator's own pages during the pass.
long current_rss_kb() {
#if defined(__linux__)
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long total_pages = 0, rss_pages = 0;
    int got = fscanf(f, "%ld %ld", &total_pages, &rss_pages);
    fclose(f);
    if (got != 2) return 0;
    return rss_pages * (sysconf(_SC_PAGESIZE) / 1024);
#else
    return peak_rss_kb();  // macOS dev fallback (not the data venue)
#endif
}

// Reaper predicate for the footprint measurement: mirrors the live system's
// reap_predicate (reclaim a site whose learned lifetime is Reap). Used only by
// the conservative reap mode; eager/madvise ignore it and reclaim any retired,
// empty segment.
inline bool bench_reap_pred(CallSiteID site) {
    return analysis::get_lifetime_tag(site) == analysis::LifetimeTag::Reap;
}

// Re-point each alloc op at its site's currently-installed routine. Run after
// the learning pass and again after each replay pass: sites that deopt during
// replay revert to null here, so the bound backend stops calling a stale routine
// that would deopt (mutex-locked) on every subsequent call. Keeps the timed
// passes a clean pure-fast-path-or-libc measurement.
inline void resolve_fns(std::vector<Op>& ops) {
    for (Op& op : ops)
        if (op.kind == OpKind::Alloc) op.fn = dispatch::lookup(op.id);
}

double now_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

// Async-signal-safe backtrace on a fault in the JIT execution path. JIT pages
// have no symbols (raw addresses), but the C++ caller frames symbolize with
// -rdynamic, which localizes the crash (e.g. a slow-init refill vs free_managed
// vs the emitted routine itself).
extern "C" void crash_handler(int sig) {
    void* bt[64];
    int n = backtrace(bt, 64);
    const char msg[] = "\n*** bound_bench caught fatal signal; backtrace:\n";
    ssize_t w = write(2, msg, sizeof(msg) - 1);
    (void)w;
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

int usage(const char* a0) {
    fprintf(stderr,
        "usage: %s <trace> [--backend glibc|bound|sim] [--passes N] "
        "[--label NAME] [--tsv-header]\n", a0);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);

    const char* trace_path = nullptr;
    Backend backend = Backend::Bound;
    const char* backend_name = "bound";
    int passes = 12;
    std::string label;
    bool tsv_header = false;
    bool profile = false;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            backend_name = argv[++i];
            if      (!strcmp(backend_name, "glibc")) backend = Backend::Glibc;
            else if (!strcmp(backend_name, "bound")) backend = Backend::Bound;
            else if (!strcmp(backend_name, "sim"))   backend = Backend::Sim;
            else { fprintf(stderr, "bad backend: %s\n", backend_name); return usage(argv[0]); }
        } else if (!strcmp(argv[i], "--passes") && i + 1 < argc) {
            passes = atoi(argv[++i]);
            if (passes < 1) passes = 1;
        } else if (!strcmp(argv[i], "--label") && i + 1 < argc) {
            label = argv[++i];
        } else if (!strcmp(argv[i], "--tsv-header")) {
            tsv_header = true;
        } else if (!strcmp(argv[i], "--profile")) {
            profile = true;
        } else if (!trace_path) {
            trace_path = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    if (!trace_path) return usage(argv[0]);
    if (label.empty()) {
        const char* slash = strrchr(trace_path, '/');
        label = slash ? slash + 1 : trace_path;
        size_t dot = label.rfind('.');
        if (dot != std::string::npos) label.resize(dot);
    }

    std::vector<AllocEvent> events;
    if (!read_trace(trace_path, events)) return 1;

    Resolved r = resolve(events);
    double matched_frac = r.total_frees
        ? static_cast<double>(r.matched_frees) / static_cast<double>(r.total_frees)
        : 1.0;

    fprintf(stderr,
        "bound_bench: %zu events, %u allocs, %llu frees (%.4f matched), "
        "backend=%s label=%s\n",
        events.size(), r.n_allocs,
        static_cast<unsigned long long>(r.total_frees), matched_frac,
        backend_name, label.c_str());

    if (tsv_header)
        printf("backend\tworkload\tp50_ns\tci95_ns\tpeak_rss_kb\tmatched_free_frac\n");

    if (r.n_allocs == 0) {
        fprintf(stderr, "bound_bench: no alloc ops; nothing to time\n");
        printf("%s\t%s\t0\t0\t0\t%.4f\n", backend_name, label.c_str(), matched_frac);
        return 0;
    }

    tbjit::alloc::init();
    tbjit::dispatch::init();         // allocates the dispatch table; the live
                                     // system does this in the trampoline ctor,
                                     // which this tool deliberately excludes
    tbjit::analysis::init_state();   // NOT init(): keep TBJIT_TRACE_ONLY out of
                                     // the picture and leave compilation enabled
    tbjit::deopt::init();

    // Oracle ceiling for the specialized backends: never deopt, so routines
    // stay installed for the entire replay (the live system sustains its JIT
    // yield by re-specializing on the background thread; a synchronous replay
    // cannot, so without this the routines bleed away to libc and we end up
    // timing glibc, not the allocator). Models a perfect, never-deopting picker.
    if (backend != Backend::Glibc) {
        tbjit::analysis::set_oracle_mode(true);  // no analysis-side deopt
        tbjit::deopt::set_enabled(false);        // no runtime revert
    }

    // Learning pass: drive every event through the offline analyzer so the
    // picker compiles + installs routines exactly as it would online. glibc
    // needs no learning. Done on this (the timing) thread so the JIT routines'
    // fs-relative TLS offsets are valid at call time.
    if (backend != Backend::Glibc) {
        for (const AllocEvent& ev : events) tbjit::analysis::process_event(ev);
    }

    // bound: resolve each alloc's routine now that dispatch is populated, so the
    // timed loop calls it directly (no per-call lookup). Stable for the whole
    // run because deopt is disabled above (routines never revert). sim looks up
    // live (it models the dispatch path); glibc uses libc malloc.
    if (backend == Backend::Bound) resolve_fns(r.ops);

    // The raw event buffer (often >2 GB) is no longer needed; free it before
    // timing so it does not inflate the RSS baseline or crowd the allocator.
    events.clear();
    events.shrink_to_fit();

    std::vector<void*> live(r.n_allocs, nullptr);

#if defined(__linux__) && defined(__x86_64__)
    const bool can_time = true;
#else
    // The JIT routines are emitted x86-64 machine code; calling them on any
    // other host faults. The glibc backend is portable; bound/sim timing is
    // Linux-x86-64 only. Plumbing (resolve, matched_free_frac) still validates.
    const bool can_time = (backend == Backend::Glibc);
#endif

    if (!can_time) {
        fprintf(stderr, "bound_bench: backend=%s timing skipped on this host "
                        "(JIT is x86-64); emitting matched_free_frac only\n",
                backend_name);
        printf("%s\t%s\t0\t0\t%ld\t%.4f\n",
               backend_name, label.c_str(), peak_rss_kb(), matched_frac);
        return 0;
    }

    // Space: the allocator's footprint for this stream = segment growth across
    // ONE full replay + the resident profiling structures. One pass mirrors the
    // original process, which survived this footprint live (net-live set held).
    //
    // Two terms, because the memory knobs move different ones:
    //   seg_kb  — RSS delta across the pass (SEG_SHIFT-dependent). After the
    //             pass we run the reaper so TBJIT_REAP_MODE affects this term:
    //             the live system reclaims on a background thread; this
    //             synchronous proxy reaps once. The net-live set is still held,
    //             so only retired, empty segments are eligible (safety invariant
    //             preserved).
    //   prof_kb — g_summaries resident (HIST_CAP-dependent). It is touched in the
    //             learning pass, so it already sits in rss_before and is NOT in
    //             the segment delta; add it back explicitly. Exact: each used
    //             summary slot is fully written.
    long rss_before = current_rss_kb();
    run_pass(backend, r.ops, live);
    if (backend != Backend::Glibc) seg::reaper_sweep(bench_reap_pred);
    long seg_kb = current_rss_kb() - rss_before;
    if (seg_kb < 0) seg_kb = 0;
    long prof_kb = (backend == Backend::Glibc) ? 0
        : static_cast<long>(analysis::summary_count()
                            * sizeof(analysis::CallSiteSummary) / 1024);
    long rss_footprint = seg_kb + prof_kb;
    reclaim_leaked(backend, r.leaked, live);

    // Latency: warmup then timed passes, reclaiming leaked allocs between each so
    // memory stays bounded (otherwise non-recycling strategies accumulate across
    // passes and exhaust the segment table / RAM). bound's op.fn is stable
    // across passes (deopt disabled => routines never revert), so no refresh.
    const int warmup = 3;
    for (int p = 0; p < warmup; ++p) {
        run_pass(backend, r.ops, live);
        reclaim_leaked(backend, r.leaked, live);
    }

    std::vector<double> per_alloc_ns;
    per_alloc_ns.reserve(passes);
    for (int p = 0; p < passes; ++p) {
        double t0 = now_ns();
        run_pass(backend, r.ops, live);
        double t1 = now_ns();
        per_alloc_ns.push_back((t1 - t0) / static_cast<double>(r.n_allocs));
        reclaim_leaked(backend, r.leaked, live);  // untimed
    }

#if defined(__linux__) && defined(__x86_64__)
    if (profile) {
        profile_pass(backend, r.ops, live, backend_name, label.c_str());
        reclaim_leaked(backend, r.leaked, live);
    }
#else
    (void)profile;
#endif

    std::sort(per_alloc_ns.begin(), per_alloc_ns.end());
    size_t n = per_alloc_ns.size();
    double p50 = (n % 2) ? per_alloc_ns[n / 2]
                         : 0.5 * (per_alloc_ns[n / 2 - 1] + per_alloc_ns[n / 2]);
    double mean = 0.0;
    for (double x : per_alloc_ns) mean += x;
    mean /= static_cast<double>(n);
    double var = 0.0;
    for (double x : per_alloc_ns) var += (x - mean) * (x - mean);
    var = (n > 1) ? var / static_cast<double>(n - 1) : 0.0;
    double ci95 = 1.96 * std::sqrt(var) / std::sqrt(static_cast<double>(n));

    fprintf(stderr,
        "bound_bench: p50=%.3f ns/alloc  ci95=%.3f  rss_footprint=%ld kB "
        "(seg=%ld prof=%ld)  passes=%d\n",
        p50, ci95, rss_footprint, seg_kb, prof_kb, passes);

    printf("%s\t%s\t%.3f\t%.3f\t%ld\t%.4f\n",
           backend_name, label.c_str(), p50, ci95, rss_footprint, matched_frac);
    return 0;
}
