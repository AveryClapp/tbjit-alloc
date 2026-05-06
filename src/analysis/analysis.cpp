#include "analysis.h"
#include "alloc/alloc.h"
#include <atomic>
#include <cstdint>

// Per-call-site summary. Tracks size distribution and event count.
// Specialization triggers after SPECIALIZE_THRESHOLD stable events.
namespace tbjit::analysis {

namespace {

constexpr uint32_t SPECIALIZE_THRESHOLD = 10'000;
constexpr uint32_t STABILITY_WINDOW     = 1'000;
constexpr size_t   MAX_CALL_SITES       = 4096;

struct CallSiteSummary {
    CallSiteID  id;
    uint64_t    event_count;
    uint64_t    stable_count;
    Strategy    current_strategy;
    // TODO: DDSketch for size distribution, KS test state
};

CallSiteSummary* g_summaries = nullptr;
size_t           g_summary_count = 0;

CallSiteSummary* find_or_create(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i) {
        if (g_summaries[i].id == id) return &g_summaries[i];
    }
    if (g_summary_count >= MAX_CALL_SITES) return nullptr;
    auto& s = g_summaries[g_summary_count++];
    s = {id, 0, 0, Strategy::Generic};
    return &s;
}

} // namespace

void init() {
    g_summaries = static_cast<CallSiteSummary*>(
        alloc::alloc(sizeof(CallSiteSummary) * MAX_CALL_SITES,
                     alignof(CallSiteSummary)));
}

void submit(const AllocEvent& ev) {
    CallSiteSummary* s = find_or_create(ev.call_site);
    if (!s) return;
    ++s->event_count;
    // TODO: update size/lifetime histograms, run KS test every STABILITY_WINDOW
}

void run() {
    // TODO: background thread loop — drain ring buffers, call submit, trigger codegen
}

} // namespace tbjit::analysis
