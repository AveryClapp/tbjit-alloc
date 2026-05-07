#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include <cassert>

static void test_prespec_triggers_compiled() {
    tbjit::alloc::init();
    tbjit::analysis::init_state();

    for (int i = 0; i < 10'000; ++i) {
        tbjit::AllocEvent ev{1, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(1) == tbjit::analysis::Phase::Compiled);
}

static void test_prespec_unstable_stays_prespec() {
    tbjit::analysis::reset_state();

    for (int i = 0; i < 10'000; ++i) {
        uint32_t size = (i % 2 == 0) ? 48 : 128;
        tbjit::AllocEvent ev{2, size, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(2) == tbjit::analysis::Phase::PreSpec);
}

static void test_compiled_drift_causes_deopt() {
    tbjit::analysis::reset_state();

    // Establish compiled state with size=48
    for (int i = 0; i < 10'000; ++i) {
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

    for (int i = 0; i < 10'000; ++i) {
        tbjit::AllocEvent ev{4, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(4) == tbjit::analysis::Phase::Compiled);
    assert(tbjit::analysis::get_candidate_strategy(4) == tbjit::Strategy::BumpAlloc);
}

int main() {
    test_prespec_triggers_compiled();
    test_prespec_unstable_stays_prespec();
    test_compiled_drift_causes_deopt();
    test_candidate_strategy_monomorphic();
    return 0;
}
