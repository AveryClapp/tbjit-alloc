#include "analysis/analysis.h"
#include "alloc/alloc.h"
#undef NDEBUG  // build is RelWithDebInfo (-DNDEBUG); force assert() active so
#include <cassert>  // these tests actually check rather than no-op.
#include <cstdlib>

static void test_prespec_triggers_compiled() {
    tbjit::alloc::init();
    tbjit::analysis::init_state();

    for (int i = 0; i < 11'000; ++i) {
        tbjit::AllocEvent ev{1, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(1) == tbjit::analysis::Phase::Compiled);
}

static void test_prespec_unstable_stays_prespec() {
    tbjit::analysis::reset_state();

    // Each window of 1000 events alternates between all-48 and all-128.
    // Consecutive windows always have different distributions → KS always unstable.
    for (int w = 0; w < 22; ++w) {
        uint32_t size = (w % 2 == 0) ? 48 : 128;
        for (int i = 0; i < 1000; ++i) {
            tbjit::AllocEvent ev{2, size, 0, 0, nullptr};
            tbjit::analysis::process_event(ev);
        }
    }
    assert(tbjit::analysis::get_phase(2) == tbjit::analysis::Phase::PreSpec);
}

static void test_compiled_drift_causes_deopt() {
    tbjit::analysis::reset_state();

    // Establish compiled state with size=48
    for (int i = 0; i < 11'000; ++i) {
        tbjit::AllocEvent ev{3, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(3) == tbjit::analysis::Phase::Compiled);

    // Feed completely different sizes — should deopt
    for (int i = 0; i < 1'000; ++i) {
        tbjit::AllocEvent ev{3, 512, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(3) == tbjit::analysis::Phase::Deopt ||
           tbjit::analysis::get_phase(3) == tbjit::analysis::Phase::PreSpec);
}

static void test_candidate_strategy_monomorphic() {
    tbjit::analysis::reset_state();

    for (int i = 0; i < 11'000; ++i) {
        tbjit::AllocEvent ev{4, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(4) == tbjit::analysis::Phase::Compiled);
    assert(tbjit::analysis::get_candidate_strategy(4) == tbjit::Strategy::BumpAlloc);
}

static void test_stable_windows_threshold_reads_env() {
    setenv("TBJIT_STABLE_WINDOWS", "3", 1);
    tbjit::analysis::init();
    assert(tbjit::analysis::stable_windows_threshold() == 3u);
    unsetenv("TBJIT_STABLE_WINDOWS");
}

static void test_stable_windows_threshold_default() {
    unsetenv("TBJIT_STABLE_WINDOWS");
    tbjit::analysis::init();
    assert(tbjit::analysis::stable_windows_threshold() == 10u);
}

int main() {
    test_prespec_triggers_compiled();
    test_prespec_unstable_stays_prespec();
    test_compiled_drift_causes_deopt();
    test_candidate_strategy_monomorphic();
    test_stable_windows_threshold_reads_env();
    test_stable_windows_threshold_default();
    return 0;
}
