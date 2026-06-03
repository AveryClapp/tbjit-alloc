#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// Per-site size histogram capacity: the number of *distinct* allocation sizes a
// single window can track exactly. Compile-time so it can be swept (the memory
// frontier study); overridable with -DTBJIT_HIST_CAP=N. A window that sees more
// than CAP distinct sizes sets `overflow` and reads as polymorphic (won't
// specialize), which is the correct decision for such a site anyway.
#ifndef TBJIT_HIST_CAP
#define TBJIT_HIST_CAP 64
#endif

namespace tbjit::analysis {

// Sparse exact histogram. Replaces a dense counts[4096] (16 KiB) with a
// fixed-capacity (size,count) table (~0.5 KiB at CAP=64), cutting per-site
// profiling memory ~30x. Real allocation streams touch few distinct sizes per
// site, so the table stays tiny; genuinely polymorphic windows overflow.
struct ExactHistogram {
    static constexpr size_t MAX_SIZE = 4096;   // largest trackable size (exclusive)
    static constexpr size_t CAP      = TBJIT_HIST_CAP;

    struct Mode { uint32_t size; uint32_t count; };

    Mode     entries[CAP]{};
    uint16_t n{0};            // distinct sizes currently tracked
    bool     overflow{false}; // saw > CAP distinct sizes
    uint64_t total{0};        // all recorded events (incl. overflowed ones)

    void record(uint32_t size) {
        if (size >= MAX_SIZE) return;
        for (uint16_t i = 0; i < n; ++i) {
            if (entries[i].size == size) {
                ++entries[i].count;
                ++total;
                return;
            }
        }
        if (n < CAP) {
            entries[n++] = {size, 1};
            ++total;
        } else {
            overflow = true;   // size untracked, but still counted in total
            ++total;
        }
    }

    uint64_t count() const { return total; }

    // Index of the dominant entry (highest count; ties -> lowest size), or -1
    // if empty. Matches the old dense argmax tie-break (lowest size wins).
    int dominant_index() const {
        int best = -1;
        for (uint16_t i = 0; i < n; ++i) {
            if (best < 0 ||
                entries[i].count > entries[best].count ||
                (entries[i].count == entries[best].count &&
                 entries[i].size  < entries[best].size)) {
                best = i;
            }
        }
        return best;
    }

    uint32_t dominant_size() const {
        int b = dominant_index();
        return b < 0 ? 0 : entries[b].size;
    }

    bool is_monomorphic(double fraction) const {
        if (total == 0) return false;
        int b = dominant_index();
        if (b < 0) return false;
        return entries[b].count >=
               static_cast<uint64_t>(fraction * static_cast<double>(total));
    }

    // p in [0.0, 1.0]. Walks tracked sizes in ascending order.
    uint32_t quantile(double p) const {
        if (total == 0) return 0;
        Mode sorted[CAP];
        size_t m = sorted_modes(sorted, CAP);
        uint64_t target = static_cast<uint64_t>(p * static_cast<double>(total - 1));
        uint64_t cum = 0;
        for (size_t i = 0; i < m; ++i) {
            cum += sorted[i].count;
            if (cum > target) return sorted[i].size;
        }
        return m == 0 ? 0 : sorted[m - 1].size;
    }

    // Fills out[] with the highest-count sizes (descending count; ties ->
    // lowest size) until k modes are written or cumulative count exceeds
    // coverage_target * total. Returns the number written.
    size_t top_k_modes(Mode out[], size_t k, double coverage_target) const {
        if (total == 0 || k == 0) return 0;
        uint32_t local[CAP];
        for (uint16_t i = 0; i < n; ++i) local[i] = entries[i].count;
        size_t written = 0;
        uint64_t cum = 0;
        uint64_t target =
            static_cast<uint64_t>(coverage_target * static_cast<double>(total));
        while (written < k) {
            int best = -1;
            for (uint16_t i = 0; i < n; ++i) {
                if (local[i] == 0) continue;
                if (best < 0 ||
                    local[i] > local[best] ||
                    (local[i] == local[best] &&
                     entries[i].size < entries[best].size)) {
                    best = i;
                }
            }
            if (best < 0) break;
            out[written++] = {entries[best].size, local[best]};
            cum += local[best];
            local[best] = 0;
            if (cum >= target) break;
        }
        return written;
    }

    // Snapshot of tracked entries sorted ascending by size (for ks_stable's
    // CDF merge-walk). Returns the number written (== n). n <= CAP, so this is
    // a cheap insertion sort run off the hot path.
    size_t sorted_modes(Mode out[], size_t cap) const {
        size_t m = 0;
        for (uint16_t i = 0; i < n && m < cap; ++i) {
            size_t j = m;
            while (j > 0 && out[j - 1].size > entries[i].size) {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = entries[i];
            ++m;
        }
        return m;
    }

    void reset() {
        n = 0;
        overflow = false;
        total = 0;
    }
};

} // namespace tbjit::analysis
