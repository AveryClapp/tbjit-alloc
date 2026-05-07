#include "analysis/analysis.h"
#include "trace/trace.h"
#include "alloc/alloc.h"
#include <cassert>
#include <unistd.h>

static void test_background_drains_events() {
    tbjit::alloc::init();
    tbjit::analysis::init_state();
    tbjit::trace::init();
    tbjit::analysis::start_background_thread();

    for (int i = 0; i < 100; ++i)
        tbjit::trace::record_alloc(99, 48, reinterpret_cast<void*>(0x1000 + i));

    usleep(100'000); // 100ms — give background thread time to drain

    assert(tbjit::analysis::events_processed() >= 100);
    tbjit::analysis::stop_background_thread();
}

int main() {
    test_background_drains_events();
    return 0;
}
