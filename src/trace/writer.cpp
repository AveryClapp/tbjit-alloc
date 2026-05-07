#include "trace/writer.h"
#include <cstdio>
#include <ctime>

namespace tbjit::trace {

namespace {
FILE* g_file = nullptr;
}

void writer_open(const char* path) {
    g_file = fopen(path, "wb");
    if (!g_file) return;
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    TraceHeader hdr{
        TRACE_MAGIC,
        TRACE_VERSION,
        static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
            static_cast<uint64_t>(ts.tv_nsec)
    };
    fwrite(&hdr, sizeof(hdr), 1, g_file);
}

void writer_write(const AllocEvent& ev) {
    if (g_file) fwrite(&ev, sizeof(ev), 1, g_file);
}

void writer_close() {
    if (g_file) {
        fflush(g_file);
        fclose(g_file);
        g_file = nullptr;
    }
}

bool writer_active() { return g_file != nullptr; }

} // namespace tbjit::trace
