// Trace replay tool. Reads a binary trace file produced by setting
// TBJIT_TRACE=<path> at runtime and feeds the events back through
// analysis::process_event offline. Useful for reproducing analyzer
// decisions on a recorded workload without re-running the original
// program — debugging strategy selection, KS test thresholds, blacklist
// behavior, etc.
//
// Usage: tbjit_replay <trace-file>
//
// The output looks identical to TBJIT_DUMP=1 output: a per-call-site
// summary table plus the global jit_allocs / generic_allocs counters
// (always zero here since there's no real allocator behind the replay).

#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include "common.h"
#include "trace/writer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace tbjit;

static int usage(const char* argv0) {
    fprintf(stderr, "usage: %s <trace-file>\n", argv0);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2) return usage(argv[0]);

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "replay: open %s failed\n", argv[1]);
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

    AllocEvent ev{};
    size_t replayed = 0;
    while (fread(&ev, sizeof(ev), 1, f) == 1) {
        analysis::process_event(ev);
        ++replayed;
    }
    fclose(f);

    fprintf(stderr, "replay: processed %zu events from %s\n", replayed, argv[1]);
    analysis::dump_stats();
    return 0;
}
