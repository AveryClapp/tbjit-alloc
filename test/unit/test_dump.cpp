#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include <cassert>
#include <cstdio>

static void test_dump_does_not_crash() {
    tbjit::alloc::init();
    tbjit::analysis::init_state();

    // Feed enough events to trigger Compiled state
    for (int i = 0; i < 10'000; ++i) {
        tbjit::AllocEvent ev{7, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }

    // Redirect stderr to /dev/null, call dump, assert no crash
    FILE* saved = stderr;
    stderr = fopen("/dev/null", "w");
    tbjit::analysis::dump_stats();
    fclose(stderr);
    stderr = saved;
}

static void test_dump_empty_state() {
    tbjit::analysis::reset_state();
    FILE* saved = stderr;
    stderr = fopen("/dev/null", "w");
    tbjit::analysis::dump_stats();  // should not crash on empty table
    fclose(stderr);
    stderr = saved;
}

int main() {
    test_dump_does_not_crash();
    test_dump_empty_state();
    return 0;
}
