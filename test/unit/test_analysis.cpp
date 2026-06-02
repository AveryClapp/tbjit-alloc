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

static void test_window_size_reads_env() {
    setenv("TBJIT_WINDOW_SIZE", "500", 1);
    tbjit::analysis::init();
    assert(tbjit::analysis::window_size() == 500u);
    unsetenv("TBJIT_WINDOW_SIZE");
    tbjit::analysis::init();
    assert(tbjit::analysis::window_size() == 1000u);
}

static void test_ks_alpha_reads_env() {
    setenv("TBJIT_KS_ALPHA", "0.10", 1);
    tbjit::analysis::init();
    assert(tbjit::analysis::ks_alpha() > 0.099 && tbjit::analysis::ks_alpha() < 0.101);
    unsetenv("TBJIT_KS_ALPHA");
    tbjit::analysis::init();
    assert(tbjit::analysis::ks_alpha() > 0.049 && tbjit::analysis::ks_alpha() < 0.051);
}

static void test_deopt_blacklist_limit_reads_env() {
    setenv("TBJIT_DEOPT_LIMIT", "5", 1);
    tbjit::analysis::init();
    assert(tbjit::analysis::deopt_blacklist_limit() == 5u);
    unsetenv("TBJIT_DEOPT_LIMIT");
    tbjit::analysis::init();
    assert(tbjit::analysis::deopt_blacklist_limit() == 3u);
}

static void test_deopt_reason_size_drift() {
    tbjit::analysis::reset_state();

    // Compile a site at a stable size.
    for (int i = 0; i < 11'000; ++i) {
        tbjit::AllocEvent ev{9, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(9) == tbjit::analysis::Phase::Compiled);

    // Drift the size distribution: check_postspec's KS test detects the drift
    // and must record the ground-truth reason (SizeDrift), not None.
    for (int i = 0; i < 1'000; ++i) {
        tbjit::AllocEvent ev{9, 512, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_deopt_reason(9) ==
           tbjit::analysis::DeoptReason::SizeDrift);
}

static void test_deopt_reason_recorded_by_reset() {
    tbjit::analysis::reset_state();

    for (int i = 0; i < 11'000; ++i) {
        tbjit::AllocEvent ev{10, 64, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(10) == tbjit::analysis::Phase::Compiled);

    // The runtime JIT deopt path passes a reason into reset_call_site; it must
    // be retained on the summary so the dump records ground truth.
    tbjit::analysis::reset_call_site(10, tbjit::analysis::DeoptReason::RegionExhaust);
    assert(tbjit::analysis::get_deopt_reason(10) ==
           tbjit::analysis::DeoptReason::RegionExhaust);
}

int main() {
    test_prespec_triggers_compiled();
    test_prespec_unstable_stays_prespec();
    test_compiled_drift_causes_deopt();
    test_candidate_strategy_monomorphic();
    test_stable_windows_threshold_reads_env();
    test_stable_windows_threshold_default();
    test_window_size_reads_env();
    test_ks_alpha_reads_env();
    test_deopt_blacklist_limit_reads_env();
    test_deopt_reason_size_drift();
    test_deopt_reason_recorded_by_reset();
    return 0;
}
