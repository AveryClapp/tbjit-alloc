#pragma once
#include "../common.h"
#include "histogram.h"

namespace tbjit::analysis {

enum class Phase : uint8_t { PreSpec, Compiled, Deopt };

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
    uint64_t    event_count{0};
    uint32_t    stable_windows{0};

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
void     reset_call_site(CallSiteID id);
void     run();  // background thread entry point (Task 6)
void     start_background_thread();
void     stop_background_thread();
uint64_t events_processed();
void     dump_stats();

} // namespace tbjit::analysis
