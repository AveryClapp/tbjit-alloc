#pragma once
#include "histogram.h"
#include <cmath>

namespace tbjit::analysis {

// Two-sample Kolmogorov-Smirnov test on ExactHistograms.
// Returns true if the two distributions are statistically indistinguishable
// at significance level alpha (typical: 0.05).
//
// Uses D = max|F1(x) - F2(x)| and the asymptotic critical value
// c(alpha) * sqrt((n1 + n2) / (n1 * n2)).
inline bool ks_stable(const ExactHistogram& a, const ExactHistogram& b, double alpha) {
    if (a.count() == 0 || b.count() == 0) return false;
    // Overflowed windows are polymorphic by definition: their CDFs are
    // incomplete (untracked mass), so they cannot be certified stable.
    if (a.overflow || b.overflow) return false;

    using Mode = ExactHistogram::Mode;
    Mode sa[ExactHistogram::CAP], sb[ExactHistogram::CAP];
    size_t na = a.sorted_modes(sa, ExactHistogram::CAP);
    size_t nb = b.sorted_modes(sb, ExactHistogram::CAP);

    double max_d = 0.0;
    double cum_a = 0.0, cum_b = 0.0;
    double n_a = static_cast<double>(a.count());
    double n_b = static_cast<double>(b.count());

    // Merge-walk both sorted size lists; the KS statistic D = max|F_a - F_b|
    // is attained at a step point, so evaluate the gap after advancing past
    // each distinct size in ascending order.
    size_t i = 0, j = 0;
    while (i < na || j < nb) {
        uint32_t size;
        if      (i >= na) size = sb[j].size;
        else if (j >= nb) size = sa[i].size;
        else              size = sa[i].size < sb[j].size ? sa[i].size : sb[j].size;

        while (i < na && sa[i].size == size) { cum_a += sa[i].count / n_a; ++i; }
        while (j < nb && sb[j].size == size) { cum_b += sb[j].count / n_b; ++j; }

        double d = cum_a > cum_b ? cum_a - cum_b : cum_b - cum_a;
        if (d > max_d) max_d = d;
    }

    // Asymptotic critical values: c(0.10)=1.2239, c(0.05)=1.3581, c(0.01)=1.6276
    double c_alpha = (alpha <= 0.01) ? 1.6276 : (alpha <= 0.05 ? 1.3581 : 1.2239);
    double critical = c_alpha * std::sqrt((n_a + n_b) / (n_a * n_b));
    return max_d < critical;
}

} // namespace tbjit::analysis
