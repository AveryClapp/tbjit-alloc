#include "analysis/analysis.h"
#include "dispatch/dispatch.h"
#include "alloc/alloc.h"
#include "deopt/deopt.h"
#include <cassert>

static void test_jit_dispatch_installed() {
    tbjit::alloc::init();
    tbjit::analysis::init_state();
    tbjit::dispatch::init();

    const tbjit::CallSiteID SITE = 42;

    for (int i = 0; i < 11'000; ++i) {
        tbjit::AllocEvent ev{SITE, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }

    assert(tbjit::analysis::get_phase(SITE) == tbjit::analysis::Phase::Compiled);

    tbjit::dispatch::RoutineFn fn = tbjit::dispatch::lookup(SITE);
    (void)fn;
    assert(fn != nullptr);

    tbjit::deopt::mark_safe_point();
}

static void test_deopt_reverts_dispatch() {
    tbjit::analysis::reset_state();

    const tbjit::CallSiteID SITE = 43;

    for (int i = 0; i < 11'000; ++i) {
        tbjit::AllocEvent ev{SITE, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(SITE) == tbjit::analysis::Phase::Compiled);

    tbjit::dispatch::RoutineFn fn = tbjit::dispatch::lookup(SITE);
    assert(fn != nullptr);

    // Simulate deopt: handle() reverts dispatch + resets analysis.
    // The RoutineFn pointer IS the start of the code page.
    tbjit::dispatch::RoutineFn fn_before = fn;
    tbjit::deopt::handle(SITE, reinterpret_cast<void*>(fn_before));
    tbjit::deopt::mark_safe_point();

    tbjit::dispatch::RoutineFn fn_after = tbjit::dispatch::lookup(SITE);
    (void)fn_after;
    assert(fn_after == nullptr);

    // reset_call_site sets phase to Deopt; without a subsequent process_event
    // it stays Deopt.
    assert(tbjit::analysis::get_phase(SITE) == tbjit::analysis::Phase::PreSpec ||
           tbjit::analysis::get_phase(SITE) == tbjit::analysis::Phase::Deopt);
}

int main() {
    test_jit_dispatch_installed();
    test_deopt_reverts_dispatch();
    return 0;
}
