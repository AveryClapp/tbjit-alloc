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

    double max_d = 0.0;
    double cum_a = 0.0, cum_b = 0.0;
    double n_a = static_cast<double>(a.count());
    double n_b = static_cast<double>(b.count());

    for (uint32_t i = 0; i < ExactHistogram::MAX_SIZE; ++i) {
        cum_a += a.counts[i] / n_a;
        cum_b += b.counts[i] / n_b;
        double d = cum_a > cum_b ? cum_a - cum_b : cum_b - cum_a;
        if (d > max_d) max_d = d;
    }

    // Asymptotic critical values: c(0.10)=1.2239, c(0.05)=1.3581, c(0.01)=1.6276
    double c_alpha = (alpha <= 0.01) ? 1.6276 : (alpha <= 0.05 ? 1.3581 : 1.2239);
    double critical = c_alpha * std::sqrt((n_a + n_b) / (n_a * n_b));
    return max_d < critical;
}

} // namespace tbjit::analysis
