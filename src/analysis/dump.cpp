#include "analysis/analysis.h"
#include "analysis/histogram.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// Counters defined at global scope in trampoline.cpp.
extern std::atomic<uint64_t> g_jit_allocs;
extern std::atomic<uint64_t> g_generic_allocs;

// Access internal analysis state. These are defined in analysis.cpp.
// We declare them extern rather than exposing them in the header.
namespace tbjit::analysis {

// Forward-declared internal types for dump access.
// Keep in sync with analysis.cpp anonymous namespace.
extern CallSiteSummary* g_summaries;
extern size_t           g_summary_count;

namespace {

const char* strategy_name(Strategy s) {
    switch (s) {
        case Strategy::BumpAlloc:           return "BumpAlloc";
        case Strategy::ThreadLocalFreeList: return "TLFreeList";
        case Strategy::EpochArena:          return "EpochArena";
        case Strategy::PairedStack:         return "PairedStack";
        case Strategy::ProducerConsumer:    return "ProducerConsumer";
        case Strategy::MultiSizeFreeList:   return "MultiSizeFreeList";
        default:                            return "-";
    }
}

const char* lifetime_name(LifetimeTag t) {
    switch (t) {
        case LifetimeTag::Reap:    return "Reap";
        case LifetimeTag::Hold:    return "Hold";
        default:                   return "Unknown";
    }
}

const char* phase_name(Phase p, bool blacklisted) {
    if (blacklisted)            return "Blacklisted";
    if (p == Phase::Compiled)   return "Compiled";
    if (p == Phase::Deopt)      return "Deopt";
    return "PreSpec";
}

const char* deopt_reason_name(DeoptReason r) {
    switch (r) {
        case DeoptReason::SizeDrift:     return "SizeDrift";
        case DeoptReason::RegionExhaust: return "RegionExhaust";
        case DeoptReason::ThreadShift:   return "ThreadShift";
        case DeoptReason::LifoViolation: return "LifoViolation";
        case DeoptReason::Other:         return "Other";
        case DeoptReason::None:          return "None";
    }
    return "None";
}

void print_per_site_table(FILE* out) {
    fprintf(out, "\n%-18s %10s %6s %6s %6s  %10s  %-12s  %s\n",
            "CALL SITE", "EVENTS", "P50", "P95", "P99",
            "JIT@EVENTS", "PHASE", "STRATEGY");
    fprintf(out, "%-18s %10s %6s %6s %6s  %10s  %-12s  %s\n",
            "----------", "------", "---", "---", "---",
            "----------", "-----", "--------");

    for (size_t i = 0; i < g_summary_count; ++i) {
        const CallSiteSummary& s = g_summaries[i];
        const ExactHistogram& h  = (s.phase == Phase::Compiled)
                                   ? s.baseline
                                   : s.windows[s.active].hist;
        char first_compile[16];
        if (s.first_compile_events == 0)
            std::snprintf(first_compile, sizeof first_compile, "-");
        else
            std::snprintf(first_compile, sizeof first_compile, "%lu",
                static_cast<unsigned long>(s.first_compile_events));
        fprintf(out, "0x%-16lx %10lu %6u %6u %6u  %10s  %-12s  %s\n",
                static_cast<unsigned long>(s.id),
                static_cast<unsigned long>(s.event_count),
                h.quantile(0.50),
                h.quantile(0.95),
                h.quantile(0.99),
                first_compile,
                phase_name(s.phase, s.blacklisted),
                strategy_name(s.candidate));
    }
}

struct Summary {
    size_t total      = 0;
    size_t compiled   = 0;
    size_t blacklist  = 0;
    size_t prespec    = 0;
    size_t deopt      = 0;
    size_t by_strategy[7] = {};   // index by Strategy enum value
    size_t by_lifetime[3] = {};   // index by LifetimeTag enum value
    uint64_t total_events = 0;
    uint64_t total_frees  = 0;
};

Summary build_summary() {
    Summary out;
    for (size_t i = 0; i < g_summary_count; ++i) {
        const CallSiteSummary& s = g_summaries[i];
        ++out.total;
        out.total_events += s.event_count;
        out.total_frees  += s.free_count;
        if (s.blacklisted)                 ++out.blacklist;
        else if (s.phase == Phase::Compiled) ++out.compiled;
        else if (s.phase == Phase::Deopt)    ++out.deopt;
        else                                  ++out.prespec;
        if (s.phase == Phase::Compiled && !s.blacklisted) {
            unsigned si = static_cast<unsigned>(s.candidate);
            if (si < 7) ++out.by_strategy[si];
            unsigned li = static_cast<unsigned>(s.lifetime);
            if (li < 3) ++out.by_lifetime[li];
        }
    }
    return out;
}

void print_summary(FILE* out, const Summary& sum) {
    auto pct = [&](size_t n) {
        return sum.total == 0 ? 0.0 : 100.0 * static_cast<double>(n) /
                                     static_cast<double>(sum.total);
    };
    auto pct_of_compiled = [&](size_t n) {
        return sum.compiled == 0 ? 0.0 : 100.0 * static_cast<double>(n) /
                                        static_cast<double>(sum.compiled);
    };
    uint64_t jit_n = g_jit_allocs.load(std::memory_order_relaxed);
    uint64_t gen_n = g_generic_allocs.load(std::memory_order_relaxed);
    uint64_t tot   = jit_n + gen_n;
    double jit_pct = tot == 0 ? 0.0
                              : 100.0 * static_cast<double>(jit_n) /
                                        static_cast<double>(tot);

    // Legacy lines first — kept as-is for the integration test regexes
    // and any external tools that grep for these tokens.
    fprintf(out, "\njit_allocs:     %llu\n",
            static_cast<unsigned long long>(jit_n));
    fprintf(out, "generic_allocs: %llu\n",
            static_cast<unsigned long long>(gen_n));

    fprintf(out, "\n=== Summary ===\n");
    fprintf(out, "Sites observed:      %zu\n", sum.total);
    fprintf(out, "  Compiled:          %zu (%.1f%%)\n", sum.compiled, pct(sum.compiled));
    fprintf(out, "  Blacklisted:       %zu (%.1f%%)\n", sum.blacklist, pct(sum.blacklist));
    fprintf(out, "  Deopt:             %zu (%.1f%%)\n", sum.deopt,     pct(sum.deopt));
    fprintf(out, "  PreSpec:           %zu (%.1f%%)\n", sum.prespec,   pct(sum.prespec));
    fprintf(out, "Total events:        %llu (%.1f%% via JIT)\n",
            static_cast<unsigned long long>(tot), jit_pct);

    // Time-to-specialize distribution across compiled sites.
    if (sum.compiled > 0) {
        // 4096 × 8 bytes = 32 KiB on the stack — safely within frame
        // limits; mirrors MAX_CALL_SITES in analysis.cpp.
        uint64_t fces[4096];
        size_t n = 0;
        for (size_t i = 0; i < g_summary_count; ++i) {
            const CallSiteSummary& s = g_summaries[i];
            if (s.first_compile_events != 0) fces[n++] = s.first_compile_events;
        }
        // Simple insertion sort — n is bounded by MAX_CALL_SITES (4096) and
        // typically far smaller; not worth pulling in <algorithm> here.
        for (size_t i = 1; i < n; ++i) {
            uint64_t v = fces[i]; size_t j = i;
            while (j > 0 && fces[j - 1] > v) { fces[j] = fces[j - 1]; --j; }
            fces[j] = v;
        }
        uint64_t fce_p50 = n ? fces[n / 2] : 0;
        uint64_t fce_p95 = n ? fces[(n * 95) / 100] : 0;
        uint64_t fce_min = n ? fces[0] : 0;
        uint64_t fce_max = n ? fces[n - 1] : 0;
        fprintf(out,
                "Events-to-specialize: min=%llu  p50=%llu  p95=%llu  max=%llu  (n=%zu)\n",
                static_cast<unsigned long long>(fce_min),
                static_cast<unsigned long long>(fce_p50),
                static_cast<unsigned long long>(fce_p95),
                static_cast<unsigned long long>(fce_max),
                n);
    }

    fprintf(out, "\n=== Strategy distribution (compiled sites) ===\n");
    for (unsigned si = 0; si < 7; ++si) {
        if (sum.by_strategy[si] == 0) continue;
        fprintf(out, "  %-20s %zu (%.1f%%)\n",
                strategy_name(static_cast<Strategy>(si)),
                sum.by_strategy[si],
                pct_of_compiled(sum.by_strategy[si]));
    }
    fprintf(out, "\n=== Lifetime distribution (compiled sites) ===\n");
    for (unsigned li = 0; li < 3; ++li) {
        if (sum.by_lifetime[li] == 0) continue;
        fprintf(out, "  %-20s %zu (%.1f%%)\n",
                lifetime_name(static_cast<LifetimeTag>(li)),
                sum.by_lifetime[li],
                pct_of_compiled(sum.by_lifetime[li]));
    }
}

// Optional structured-output sink for downstream paper-analysis scripts.
// Activated via env TBJIT_DUMP_JSON=/path/to/file.json. Emits a single
// JSON object: { meta:{...}, summary:{...}, sites:[...] }.
//
// `%p` in the path is replaced with the current PID — necessary when
// the workload forks subprocesses that inherit LD_PRELOAD (e.g. gcc
// driver → cc1plus → as). Without it, the last process to exit
// overwrites everyone else's dump and the JSON reflects whichever
// child happens to finish last — usually the smallest, since the
// driver typically does the least allocation. With `%p`, each
// process writes its own file and the runner can pick the dominant
// one post-hoc.
void write_json_dump(const char* path, const Summary& sum) {
    char resolved[1024];
    const char* pct = std::strstr(path, "%p");
    if (pct) {
        size_t prefix_len = static_cast<size_t>(pct - path);
        if (prefix_len >= sizeof(resolved)) return;
        std::memcpy(resolved, path, prefix_len);
        int n = std::snprintf(resolved + prefix_len,
                              sizeof(resolved) - prefix_len,
                              "%d%s",
                              static_cast<int>(getpid()),
                              pct + 2);
        if (n < 0 || prefix_len + static_cast<size_t>(n) >= sizeof(resolved))
            return;
        path = resolved;
    }
    FILE* j = std::fopen(path, "w");
    if (!j) return;
    uint64_t jit_n = g_jit_allocs.load(std::memory_order_relaxed);
    uint64_t gen_n = g_generic_allocs.load(std::memory_order_relaxed);
    fprintf(j, "{\n");
    fprintf(j, "  \"summary\": {\n");
    fprintf(j, "    \"sites_observed\": %zu,\n", sum.total);
    fprintf(j, "    \"compiled\": %zu,\n", sum.compiled);
    fprintf(j, "    \"blacklisted\": %zu,\n", sum.blacklist);
    fprintf(j, "    \"deopt\": %zu,\n", sum.deopt);
    fprintf(j, "    \"prespec\": %zu,\n", sum.prespec);
    fprintf(j, "    \"jit_allocs\": %llu,\n",
            static_cast<unsigned long long>(jit_n));
    fprintf(j, "    \"generic_allocs\": %llu,\n",
            static_cast<unsigned long long>(gen_n));
    fprintf(j, "    \"strategy_counts\": {");
    bool first = true;
    for (unsigned si = 0; si < 7; ++si) {
        if (sum.by_strategy[si] == 0) continue;
        fprintf(j, "%s\"%s\": %zu",
                first ? "" : ", ",
                strategy_name(static_cast<Strategy>(si)),
                sum.by_strategy[si]);
        first = false;
    }
    fprintf(j, "},\n");
    fprintf(j, "    \"lifetime_counts\": {");
    first = true;
    for (unsigned li = 0; li < 3; ++li) {
        if (sum.by_lifetime[li] == 0) continue;
        fprintf(j, "%s\"%s\": %zu",
                first ? "" : ", ",
                lifetime_name(static_cast<LifetimeTag>(li)),
                sum.by_lifetime[li]);
        first = false;
    }
    fprintf(j, "}\n");
    fprintf(j, "  },\n");
    fprintf(j, "  \"sites\": [\n");
    for (size_t i = 0; i < g_summary_count; ++i) {
        const CallSiteSummary& s = g_summaries[i];
        const ExactHistogram& h  = (s.phase == Phase::Compiled)
                                   ? s.baseline
                                   : s.windows[s.active].hist;
        fprintf(j,
                "    {\"id\": %lu, \"events\": %lu, \"frees\": %lu, "
                "\"p50\": %u, \"p95\": %u, \"p99\": %u, "
                "\"phase\": \"%s\", \"strategy\": \"%s\", "
                "\"lifetime\": \"%s\", \"deopts\": %u, "
                "\"deopt_reason\": \"%s\", "
                "\"blacklisted\": %s, \"first_compile_events\": %lu}%s\n",
                static_cast<unsigned long>(s.id),
                static_cast<unsigned long>(s.event_count),
                static_cast<unsigned long>(s.free_count),
                h.quantile(0.50), h.quantile(0.95), h.quantile(0.99),
                phase_name(s.phase, s.blacklisted),
                strategy_name(s.candidate),
                lifetime_name(s.lifetime),
                s.deopt_count,
                deopt_reason_name(s.deopt_reason),
                s.blacklisted ? "true" : "false",
                static_cast<unsigned long>(s.first_compile_events),
                (i + 1 == g_summary_count) ? "" : ",");
    }
    fprintf(j, "  ]\n");
    fprintf(j, "}\n");
    std::fclose(j);
}

} // namespace

void dump_stats() {
    print_per_site_table(stderr);
    Summary sum = build_summary();
    print_summary(stderr, sum);

    if (const char* path = std::getenv("TBJIT_DUMP_JSON"))
        write_json_dump(path, sum);
}

} // namespace tbjit::analysis
