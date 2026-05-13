#include "analysis/histogram.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit::analysis;

static void test_two_dominant_sizes_returns_both() {
    ExactHistogram h;
    for (int i = 0; i < 600; ++i) h.record(32);
    for (int i = 0; i < 400; ++i) h.record(96);

    ExactHistogram::Mode modes[4];
    size_t n = h.top_k_modes(modes, 4, 0.90);
    CHECK(n == 2);
    CHECK(modes[0].size == 32);
    CHECK(modes[0].count == 600);
    CHECK(modes[1].size == 96);
    CHECK(modes[1].count == 400);
}

static void test_single_dominant_satisfies_coverage_early() {
    ExactHistogram h;
    for (int i = 0; i < 990; ++i) h.record(48);
    for (int i = 0; i < 10; ++i)  h.record(128);

    ExactHistogram::Mode modes[4];
    size_t n = h.top_k_modes(modes, 4, 0.90);
    CHECK(n == 1);
    CHECK(modes[0].size == 48);
}

static void test_k_limits_results() {
    ExactHistogram h;
    for (int i = 0; i < 100; ++i) h.record(8);
    for (int i = 0; i < 100; ++i) h.record(16);
    for (int i = 0; i < 100; ++i) h.record(24);
    for (int i = 0; i < 100; ++i) h.record(32);
    for (int i = 0; i < 100; ++i) h.record(40);

    ExactHistogram::Mode modes[3];
    size_t n = h.top_k_modes(modes, 3, 0.99);  // can't reach 99% with 3
    CHECK(n == 3);
}

static void test_empty_returns_zero() {
    ExactHistogram h;
    ExactHistogram::Mode modes[4];
    CHECK(h.top_k_modes(modes, 4, 0.5) == 0);
}

int main() {
    test_two_dominant_sizes_returns_both();
    test_single_dominant_satisfies_coverage_early();
    test_k_limits_results();
    test_empty_returns_zero();
    std::puts("test_top_k_modes OK");
    return 0;
}
