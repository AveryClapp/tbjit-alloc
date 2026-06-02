#include "trace/writer.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <unistd.h>

namespace tbjit::trace {

namespace {
FILE* g_file = nullptr;
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
}

void writer_open(const char* path) {
    // `%p` in the path is replaced with the current PID (same convention as
    // TBJIT_DUMP_JSON). Workloads that fork subprocesses inheriting LD_PRELOAD
    // (gcc driver → cc1plus → as) would otherwise clobber one trace file; with
    // `%p` each process writes its own and the runner picks the dominant one.
    char resolved[1024];
    const char* pct = std::strstr(path, "%p");
    if (pct) {
        size_t prefix = static_cast<size_t>(pct - path);
        if (prefix >= sizeof(resolved)) return;
        std::memcpy(resolved, path, prefix);
        int n = std::snprintf(resolved + prefix, sizeof(resolved) - prefix,
                              "%d%s", static_cast<int>(getpid()), pct + 2);
        if (n < 0 || prefix + static_cast<size_t>(n) >= sizeof(resolved)) return;
        path = resolved;
    }
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
    pthread_mutex_lock(&g_mutex);
    if (g_file) fwrite(&ev, sizeof(ev), 1, g_file);
    pthread_mutex_unlock(&g_mutex);
}

void writer_close() {
    pthread_mutex_lock(&g_mutex);
    if (g_file) {
        fflush(g_file);
        fclose(g_file);
        g_file = nullptr;
    }
    pthread_mutex_unlock(&g_mutex);
}

bool writer_active() { return g_file != nullptr; }

} // namespace tbjit::trace
