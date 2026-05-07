#include "analysis/ks_test.h"
#include "analysis/histogram.h"
#include <cassert>

static void test_identical_stable() {
    tbjit::analysis::ExactHistogram a, b;
    for (int i = 0; i < 1000; ++i) { a.record(48); b.record(48); }
    assert(tbjit::analysis::ks_stable(a, b, 0.05));
}

static void test_different_unstable() {
    tbjit::analysis::ExactHistogram a, b;
    for (int i = 0; i < 1000; ++i) a.record(48);
    for (int i = 0; i < 1000; ++i) b.record(128);
    assert(!tbjit::analysis::ks_stable(a, b, 0.05));
}

static void test_empty_unstable() {
    tbjit::analysis::ExactHistogram a, b;
    for (int i = 0; i < 1000; ++i) a.record(48);
    assert(!tbjit::analysis::ks_stable(a, b, 0.05));
}

static void test_similar_stable() {
    // Two histograms with mostly the same distribution — should be stable
    tbjit::analysis::ExactHistogram a, b;
    for (int i = 0; i < 950; ++i) { a.record(48); b.record(48); }
    for (int i = 0; i < 50; ++i)  { a.record(64); b.record(64); }
    assert(tbjit::analysis::ks_stable(a, b, 0.05));
}

int main() {
    test_identical_stable();
    test_different_unstable();
    test_empty_unstable();
    test_similar_stable();
    return 0;
}
