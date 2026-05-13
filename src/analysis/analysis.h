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

struct SizeWindow {
    ExactHistogram hist;
    uint32_t       count{0};

    static constexpr uint32_t WINDOW_SIZE = 1000;

    bool full() const { return count >= WINDOW_SIZE; }
    void record(uint32_t size) { hist.record(size); ++count; }
    void reset() { hist.reset(); count = 0; }
};

struct CallSiteSummary {
    CallSiteID  id{0};
    Phase       phase{Phase::PreSpec};
    uint64_t    event_count{0};      // alloc events
    uint64_t    free_count{0};       // free events whose alloc_site = this site
    uint32_t    stable_windows{0};
    uint32_t    deopt_count{0};      // total deopts seen; blacklist threshold
    bool        blacklisted{false};  // true → never recompile this site
    LifetimeTag lifetime{LifetimeTag::Unknown};

    SizeWindow  windows[2];
    uint8_t     active{0};

    ExactHistogram baseline;  // frozen at specialization time
    SizeWindow     post_window;

    Strategy    candidate{Strategy::Generic};
    void*       code_page{nullptr};
};

void     init();
void     init_state();
void     reset_state();
void     process_event(const AllocEvent& ev);
Phase    get_phase(CallSiteID id);
Strategy get_candidate_strategy(CallSiteID id);
LifetimeTag get_lifetime_tag(CallSiteID id);
void     reset_call_site(CallSiteID id);
void     run();  // background thread entry point (Task 6)
void     start_background_thread();
void     stop_background_thread();
uint64_t events_processed();
void     dump_stats();

} // namespace tbjit::analysis
