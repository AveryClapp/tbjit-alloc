#pragma once
#include "../common.h"
#include "histogram.h"

namespace tbjit::analysis {

enum class Phase : uint8_t { PreSpec, Compiled, Deopt };

// Per-site prediction: how aggressively should the reaper munmap retired
// segments belonging to this site?
//   Reap  — frees dominate; chunks come back fast; safe to reclaim eagerly.
//   Hold  — alloc-heavy with few frees; reaping is futile and may crash if
//           user still holds pointers.
//   Unknown — too few events; default to Hold (conservative).
enum class LifetimeTag : uint8_t { Unknown, Reap, Hold };

// Ground-truth reason a compiled site deoptimized / was blacklisted, recorded
// where the deopt actually happens (vs. analyze_dumps.py's strategy-inferred
// guess). Emitted per-site in the JSON dump.
//   SizeDrift     — observed size distribution drifted off the locked-in modes
//   RegionExhaust — segment/arena filled before chunks were reclaimed
//   ThreadShift   — owning thread changed (cross-thread alloc/free)
//   LifoViolation — PairedStack discipline broken (frees not LIFO)
//   Other         — deopt happened but no specific reason was threaded in
enum class DeoptReason : uint8_t {
    None, SizeDrift, RegionExhaust, ThreadShift, LifoViolation, Other
};

// Runtime-configurable picker knobs (env-overridable; see analysis.cpp). Declared
// here so SizeWindow::full() can read window_size() from its inline body.
uint32_t window_size();            // TBJIT_WINDOW_SIZE, default 1000
double   ks_alpha();               // TBJIT_KS_ALPHA, default 0.05
uint32_t deopt_blacklist_limit();  // TBJIT_DEOPT_LIMIT, default 3

struct SizeWindow {
    ExactHistogram hist;
    uint32_t       count{0};

    // WINDOW_SIZE is runtime-configurable (TBJIT_WINDOW_SIZE); see analysis.cpp.
    bool full() const { return count >= window_size(); }
    void record(uint32_t size) { hist.record(size); ++count; }
    void reset() { hist.reset(); count = 0; }
};

// Single-mode top-1 thread-id distribution. Replaces top_tid whenever a
// non-matching event arrives more than half the time; tracks alloc/free
// concentration cheaply (no hash table). 'total' counts events seen.
struct ThreadDist {
    uint32_t top_tid{0};
    uint32_t top_count{0};
    uint32_t total{0};

    void record(uint32_t tid) {
        ++total;
        if (top_count == 0 || tid == top_tid) {
            top_tid = tid;
            ++top_count;
        } else if (--top_count == 0) {
            top_tid = tid;
            top_count = 1;
        }
    }
};

struct SizeClass {
    uint32_t size{0};
    uint32_t count{0};
};

// Tracks site-pair LIFO discipline for PairedStack detection. top_pair is
// the dominant freeing site for this *allocating* site; lifo_count counts
// frees that matched the top of lifo_stack at the time of the free.
struct PairCandidate {
    CallSiteID free_site{0};
    uint32_t   pair_count{0};
    uint32_t   lifo_count{0};
};

struct CallSiteSummary {
    CallSiteID  id{0};
    Phase       phase{Phase::PreSpec};
    uint64_t    event_count{0};      // alloc events
    uint64_t    free_count{0};       // free events whose alloc_site = this site
    uint32_t    stable_windows{0};
    uint32_t    deopt_count{0};      // total deopts seen; blacklist threshold
    bool        blacklisted{false};  // true → never recompile this site
    DeoptReason deopt_reason{DeoptReason::None};  // ground-truth deopt cause
    LifetimeTag lifetime{LifetimeTag::Unknown};
    uint64_t    first_compile_events{0};  // event_count when this site first
                                          // reached Compiled. 0 = never
                                          // compiled. Latency-to-specialize
                                          // metric for paper analysis.

    ThreadDist  alloc_dist;
    ThreadDist  free_dist;

    SizeWindow  windows[2];
    uint8_t     active{0};

    SizeClass   classes[4];       // learned classes at specialization time
    uint8_t     class_count{0};

    void*       lifo_stack[16];   // last 16 outstanding allocs from this site
    uint8_t     lifo_head{0};
    PairCandidate top_pair;       // dominant freeing site + LIFO matches

    ExactHistogram baseline;  // frozen at specialization time
    SizeWindow     post_window;

    Strategy    candidate{Strategy::Generic};
    void*       code_page{nullptr};
};

void     init();
uint32_t stable_windows_threshold();  // convergence bar, TBJIT_STABLE_WINDOWS
void     init_state();
void     reset_state();
void     process_event(const AllocEvent& ev);
Phase    get_phase(CallSiteID id);
Strategy get_candidate_strategy(CallSiteID id);
LifetimeTag get_lifetime_tag(CallSiteID id);
DeoptReason get_deopt_reason(CallSiteID id);
void     reset_call_site(CallSiteID id, DeoptReason reason = DeoptReason::Other);

// Rigorous trace-replay oracle (tools/replay.cpp --oracle). In oracle mode the
// post-spec drift check never deopts: once a site specializes it stays
// Compiled, modeling a perfect picker that never blacklists. capturable()
// reports the upper-bound capturable allocation fraction from the replayed
// summaries (events of all Compiled sites / all events).
void set_oracle_mode(bool on);
bool oracle_mode();

// Trace-only capture mode (TBJIT_TRACE_ONLY, read once in init()). When set, no
// site specializes — the trampoline's generic record path stays live for every
// alloc, yielding a complete unspecialized event stream for offline replay.
bool trace_only();
struct OracleResult { uint64_t total_events; uint64_t captured_events; };
OracleResult capturable();
void     run();  // background thread entry point (Task 6)
void     start_background_thread();
void     stop_background_thread();
uint64_t events_processed();
size_t   summary_count();  // learned call sites (resident g_summaries slots)
void     dump_stats();

} // namespace tbjit::analysis
