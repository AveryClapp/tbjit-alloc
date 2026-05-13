#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace tbjit::analysis {

struct ExactHistogram {
    static constexpr size_t MAX_SIZE = 4096;

    uint32_t counts[MAX_SIZE]{};
    uint64_t total{0};

    void record(uint32_t size) {
        if (size < MAX_SIZE) {
            ++counts[size];
            ++total;
        }
    }

    uint64_t count() const { return total; }

    // p in [0.0, 1.0]
    uint32_t quantile(double p) const {
        if (total == 0) return 0;
        uint64_t target = static_cast<uint64_t>(p * static_cast<double>(total - 1));
        uint64_t cum = 0;
        for (uint32_t i = 0; i < MAX_SIZE; ++i) {
            cum += counts[i];
            if (cum > target) return i;
        }
        return MAX_SIZE - 1;
    }

    uint32_t dominant_size() const {
        uint32_t best = 0;
        for (uint32_t i = 1; i < MAX_SIZE; ++i)
            if (counts[i] > counts[best]) best = i;
        return best;
    }

    bool is_monomorphic(double fraction) const {
        if (total == 0) return false;
        uint32_t dom = dominant_size();
        return counts[dom] >= static_cast<uint64_t>(fraction * static_cast<double>(total));
    }

    struct Mode { uint32_t size; uint32_t count; };

    // Fills out[] with the highest-count sizes (in descending order) until
    // either k modes are written or the cumulative count exceeds
    // coverage_target * total. Returns the number written.
    size_t top_k_modes(Mode out[], size_t k, double coverage_target) const {
        if (total == 0 || k == 0) return 0;
        size_t written = 0;
        uint64_t cum = 0;
        uint64_t target = static_cast<uint64_t>(coverage_target * static_cast<double>(total));
        // Build a working copy of counts to mask out picked modes. 4096
        // entries is small enough to scan k times.
        uint32_t local[MAX_SIZE];
        memcpy(local, counts, sizeof(local));
        while (written < k) {
            uint32_t best = 0;
            for (uint32_t i = 1; i < MAX_SIZE; ++i)
                if (local[i] > local[best]) best = i;
            if (local[best] == 0) break;
            out[written++] = {best, local[best]};
            cum += local[best];
            local[best] = 0;
            if (cum >= target) break;
        }
        return written;
    }

    void reset() {
        memset(counts, 0, sizeof(counts));
        total = 0;
    }
};

} // namespace tbjit::analysis
