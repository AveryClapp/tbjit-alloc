#include "analysis/analysis.h"
#include "dispatch/dispatch.h"
#undef NDEBUG  // build is RelWithDebInfo (-DNDEBUG); force assert() active so
#include <cassert>  // these tests actually check rather than no-op.
#include <cstdlib>

using namespace tbjit;

// Drive a hot, perfectly size-stable stream for one site — enough events to
// clear PreSpec convergence and trigger the picker's compile decision.
static void drive_hot_stream(CallSiteID id) {
    for (int i = 0; i < 11'000; ++i) {
        AllocEvent ev{id, 48, 0, 0, nullptr};
        analysis::process_event(ev);
    }
}

// With TBJIT_TRACE_ONLY set, a hot size-stable stream must never specialize:
// the site stays in PreSpec and nothing is installed in the dispatch table, so
// the trampoline's generic (recorded) path stays live for the full stream.
static void test_trace_only_blocks_install() {
    setenv("TBJIT_TRACE_ONLY", "1", 1);
    analysis::init();           // reads the flag, resets state
    assert(analysis::trace_only());

    drive_hot_stream(1);

    assert(analysis::get_phase(1) == analysis::Phase::PreSpec);
    assert(dispatch::lookup(1) == nullptr);

    unsetenv("TBJIT_TRACE_ONLY");
}

// Control: with the flag clear, the identical stream compiles and installs a
// routine — proving the gate above is what suppresses specialization, not the
// stream itself.
static void test_default_installs() {
    unsetenv("TBJIT_TRACE_ONLY");
    analysis::init();
    assert(!analysis::trace_only());

    drive_hot_stream(2);

    assert(analysis::get_phase(2) == analysis::Phase::Compiled);
    assert(dispatch::lookup(2) != nullptr);
}

int main() {
    test_trace_only_blocks_install();
    test_default_installs();
    return 0;
}
