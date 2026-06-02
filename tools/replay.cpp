// Trace replay tool. Reads a binary trace file produced by setting
// TBJIT_TRACE=<path> at runtime and feeds the events back through
// analysis::process_event offline. Useful for reproducing analyzer
// decisions on a recorded workload without re-running the original
// program — debugging strategy selection, KS test thresholds, blacklist
// behavior, etc.
//
// Usage: tbjit_replay <trace-file> [--oracle]
//
// The output looks identical to TBJIT_DUMP=1 output: a per-call-site
// summary table plus the global jit_allocs / generic_allocs counters
// (always zero here since there's no real allocator behind the replay).
//
// --oracle replays with the rigorous never-deopt/never-blacklist picker: a
// site that ever specializes stays Compiled, so the reported capturable
// fraction is a true upper bound on what a perfect offline picker could serve
// via JIT. This is the number the paper cites for "headroom" — tighter and
// more defensible than the summary-derived oracle_picker.py bound, which can
// miss sites whose terminal phase drifted off Compiled online.

#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include "common.h"
#include "trace/writer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace tbjit;

static int usage(const char* argv0) {
    fprintf(stderr, "usage: %s <trace-file> [--oracle]\n", argv0);
    return 1;
}

int main(int argc, char** argv) {
    const char* trace_path = nullptr;
    bool oracle = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--oracle") == 0) oracle = true;
        else if (!trace_path)                 trace_path = argv[i];
        else                                  return usage(argv[0]);
    }
    if (!trace_path) return usage(argv[0]);

    FILE* f = fopen(trace_path, "rb");
    if (!f) {
        fprintf(stderr, "replay: open %s failed\n", trace_path);
        return 1;
    }

    trace::TraceHeader hdr{};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "replay: short read on header\n");
        fclose(f);
        return 1;
    }
    if (hdr.magic != trace::TRACE_MAGIC) {
        fprintf(stderr, "replay: bad magic 0x%08x (want 0x%08x)\n",
                hdr.magic, trace::TRACE_MAGIC);
        fclose(f);
        return 1;
    }
    if (hdr.version != trace::TRACE_VERSION) {
        fprintf(stderr, "replay: version mismatch (file=%u, replay=%u)\n",
                hdr.version, trace::TRACE_VERSION);
        fclose(f);
        return 1;
    }

    alloc::init();
    analysis::init_state();
    analysis::set_oracle_mode(oracle);

    AllocEvent ev{};
    size_t replayed = 0;
    while (fread(&ev, sizeof(ev), 1, f) == 1) {
        analysis::process_event(ev);
        ++replayed;
    }
    fclose(f);

    fprintf(stderr, "replay: processed %zu events from %s%s\n",
            replayed, trace_path, oracle ? " (oracle mode)" : "");
    analysis::dump_stats();

    if (oracle) {
        analysis::OracleResult r = analysis::capturable();
        double pct = r.total_events
                     ? 100.0 * static_cast<double>(r.captured_events) /
                       static_cast<double>(r.total_events)
                     : 0.0;
        fprintf(stderr,
                "oracle: capturable %llu / %llu events = %.1f%% (upper bound)\n",
                static_cast<unsigned long long>(r.captured_events),
                static_cast<unsigned long long>(r.total_events), pct);
    }
    return 0;
}
