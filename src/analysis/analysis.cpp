#include "analysis/analysis.h"
#include "analysis/ks_test.h"
#include "analysis/futex.h"
#include "alloc/alloc.h"
#include "trace/trace.h"
#include "trace/writer.h"
#include "codegen/codegen.h"
#include "dispatch/dispatch.h"
#include <new>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <atomic>

namespace tbjit::analysis {

CallSiteSummary* g_summaries     = nullptr;
size_t           g_summary_count = 0;

namespace {

constexpr size_t   MAX_CALL_SITES        = 4096;
constexpr uint32_t STABLE_WINDOWS_NEEDED = 10;
constexpr double   KS_ALPHA              = 0.05;

std::atomic<bool>     g_running{false};
std::atomic<uint64_t> g_events_processed{0};
pthread_t             g_thread;

CallSiteSummary* find_or_create(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return &g_summaries[i];
    if (g_summary_count >= MAX_CALL_SITES) return nullptr;
    CallSiteSummary* s = &g_summaries[g_summary_count++];
    new (s) CallSiteSummary{};
    s->id = id;
    return s;
}

void advance_prespec(CallSiteSummary* s) {
    uint8_t prev = 1 - s->active;
    bool stable = s->windows[prev].count > 0 &&
                  ks_stable(s->windows[s->active].hist,
                             s->windows[prev].hist, KS_ALPHA);
    if (stable) {
        ++s->stable_windows;
        if (s->stable_windows >= STABLE_WINDOWS_NEEDED) {
            s->baseline  = s->windows[s->active].hist;
            s->candidate = s->windows[s->active].hist.is_monomorphic(0.95)
                               ? Strategy::BumpAlloc
                               : Strategy::ThreadLocalFreeList;
            s->phase = Phase::Compiled;
            {
                codegen::RoutineSpec spec{s->id, s->candidate,
                    s->windows[s->active].hist.dominant_size()};
                void* routine = codegen::compile(spec);
                if (routine) {
                    dispatch::install(s->id,
                        reinterpret_cast<dispatch::RoutineFn>(routine));
                    s->code_page = routine;
                }
            }
            return;
        }
    } else {
        s->stable_windows = 0;
    }
    s->active = 1 - s->active;
    s->windows[s->active].reset();
}

void check_postspec(CallSiteSummary* s) {
    if (!ks_stable(s->post_window.hist, s->baseline, KS_ALPHA)) {
        s->phase = Phase::Deopt;
        s->stable_windows = 0;
        s->windows[0].reset();
        s->windows[1].reset();
        s->active = 0;
    }
    s->post_window.reset();
}

bool drain_all() {
    bool found = false;
    tbjit::trace::RingBuffer* rb = tbjit::trace::ring_head();
    while (rb) {
        AllocEvent ev;
        while (rb->pop(ev)) {
            process_event(ev);
            if (tbjit::trace::writer_active())
                tbjit::trace::writer_write(ev);
            g_events_processed.fetch_add(1, std::memory_order_relaxed);
            found = true;
        }
        rb = rb->next.load(std::memory_order_acquire);
    }
    return found;
}

void* background_loop(void*) {
    uint32_t backoff_us = 1;
    while (g_running.load(std::memory_order_acquire)) {
        if (drain_all()) {
            backoff_us = 1;
            continue;
        }
        if (backoff_us < 500) {
            struct timespec ts{0, static_cast<long>(backoff_us) * 1000L};
            nanosleep(&ts, nullptr);
            backoff_us = (backoff_us * 2 < 500) ? backoff_us * 2 : 500;
        } else {
            futex_wait();
        }
    }
    drain_all(); // final drain on shutdown
    return nullptr;
}

} // namespace

void init() {
    alloc::init();
    init_state();
}

void init_state() {
    if (!g_summaries) {
        // The summary array is ~256 MiB for MAX_CALL_SITES=4096 — too large
        // for the 4 MiB internal bump arena. Use mmap directly.
        const size_t sz = sizeof(CallSiteSummary) * MAX_CALL_SITES;
        void* mem = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        g_summaries = static_cast<CallSiteSummary*>(mem);
    }
    reset_state();
}

void reset_state() {
    if (!g_summaries) return;  // init_state not yet called
    // Only clear the used slots (mmap pages already zero-initialized at first use)
    for (size_t i = 0; i < g_summary_count; ++i)
        new (&g_summaries[i]) CallSiteSummary{};
    g_summary_count = 0;
}

void process_event(const AllocEvent& ev) {
    if (ev.size == 0 || ev.size >= ExactHistogram::MAX_SIZE) return;
    CallSiteSummary* s = find_or_create(ev.call_site);
    if (!s) return;
    ++s->event_count;

    if (s->phase == Phase::Deopt) s->phase = Phase::PreSpec;

    if (s->phase == Phase::PreSpec) {
        s->windows[s->active].record(ev.size);
        if (s->windows[s->active].full()) advance_prespec(s);
    } else {
        s->post_window.record(ev.size);
        if (s->post_window.full()) check_postspec(s);
    }
}

Phase get_phase(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return g_summaries[i].phase;
    return Phase::PreSpec;
}

Strategy get_candidate_strategy(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return g_summaries[i].candidate;
    return Strategy::Generic;
}

void run() {
    start_background_thread();
}

void start_background_thread() {
    g_running.store(true, std::memory_order_release);
    pthread_create(&g_thread, nullptr, background_loop, nullptr);
}

void stop_background_thread() {
    g_running.store(false, std::memory_order_release);
    futex_wake();
    pthread_join(g_thread, nullptr);
}

uint64_t events_processed() {
    return g_events_processed.load(std::memory_order_relaxed);
}

} // namespace tbjit::analysis
