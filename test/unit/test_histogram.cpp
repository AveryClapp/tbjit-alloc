#include "analysis/histogram.h"
#include <cassert>

static void test_exact_quantiles() {
    tbjit::analysis::ExactHistogram h;
    for (int i = 0; i < 1000; ++i) h.record(48);
    for (int i = 0; i < 1000; ++i) h.record(64);

    assert(h.count() == 2000);
    assert(h.quantile(0.50) == 48);
    assert(h.quantile(0.75) == 64);
    assert(h.quantile(0.99) == 64);
    assert(h.dominant_size() == 48);
}

static void test_boundary() {
    tbjit::analysis::ExactHistogram h;
    h.record(0);
    h.record(4095);
    assert(h.quantile(0.0) == 0);
    assert(h.quantile(1.0) == 4095);
}

static void test_monomorphic() {
    tbjit::analysis::ExactHistogram h;
    for (int i = 0; i < 500; ++i) h.record(48);
    assert(h.is_monomorphic(0.95));
}

static void test_not_monomorphic() {
    tbjit::analysis::ExactHistogram h;
    for (int i = 0; i < 500; ++i) h.record(48);
    for (int i = 0; i < 500; ++i) h.record(64);
    assert(!h.is_monomorphic(0.95));
}

static void test_reset() {
    tbjit::analysis::ExactHistogram h;
    for (int i = 0; i < 100; ++i) h.record(48);
    h.reset();
    assert(h.count() == 0);
    assert(h.dominant_size() == 0);
}

int main() {
    test_exact_quantiles();
    test_boundary();
    test_monomorphic();
    test_not_monomorphic();
    test_reset();
    return 0;
}
