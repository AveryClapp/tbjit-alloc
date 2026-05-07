#include "analysis/analysis.h"
#include "analysis/histogram.h"
#include <cstdio>

// Access internal analysis state. These are defined in analysis.cpp.
// We declare them extern rather than exposing them in the header.
namespace tbjit::analysis {

// Forward-declared internal types for dump access.
// Keep in sync with analysis.cpp anonymous namespace.
extern CallSiteSummary* g_summaries;
extern size_t           g_summary_count;

void dump_stats() {
    fprintf(stderr, "\n%-18s %10s %6s %6s %6s  %-10s  %s\n",
            "CALL SITE", "EVENTS", "P50", "P95", "P99", "STABLE", "STRATEGY");
    fprintf(stderr, "%-18s %10s %6s %6s %6s  %-10s  %s\n",
            "----------", "------", "---", "---", "---", "------", "--------");

    for (size_t i = 0; i < g_summary_count; ++i) {
        const CallSiteSummary& s = g_summaries[i];
        const ExactHistogram& h  = (s.phase == Phase::Compiled)
                                   ? s.baseline
                                   : s.windows[s.active].hist;

        const char* stable = (s.phase == Phase::Compiled) ? "YES" : "NO";
        const char* strategy;
        switch (s.candidate) {
            case Strategy::BumpAlloc:           strategy = "BumpAlloc";   break;
            case Strategy::ThreadLocalFreeList: strategy = "TLFreeList";  break;
            case Strategy::EpochArena:          strategy = "EpochArena";  break;
            case Strategy::PairedStack:         strategy = "PairedStack"; break;
            default:                            strategy = "-";            break;
        }

        fprintf(stderr, "0x%-16lx %10lu %6u %6u %6u  %-10s  %s\n",
                static_cast<unsigned long>(s.id),
                static_cast<unsigned long>(s.event_count),
                h.quantile(0.50),
                h.quantile(0.95),
                h.quantile(0.99),
                stable, strategy);
    }
}

} // namespace tbjit::analysis
