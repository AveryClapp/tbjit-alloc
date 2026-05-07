# Tracing Pipeline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build the end-to-end tracing pipeline: trampoline → lock-free ring buffers → adaptive background drain → per-call-site analysis → binary trace file + human-readable TBJIT_DUMP output.

**Architecture:** LD_PRELOAD trampoline records alloc/free events into per-thread lock-free ring buffers registered in a global intrusive linked list. A background thread drains all buffers using adaptive polling with futex parking. The analyzer maintains per-call-site summaries with exact size histograms and a two-phase KS stability test. On exit, stats dump to stderr; events stream to a binary file when TBJIT_TRACE is set.

**Tech Stack:** C++17, Linux futex (`syscall(SYS_futex, ...)`), pthreads, `__builtin_return_address`, `__attribute__((constructor/destructor))`.

---

## Build commands (run from worktree root)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j$(nproc)
ctest --test-dir build -V
```

Symlink compile_commands for LSP:
```bash
ln -sf build/compile_commands.json compile_commands.json
```

---

## Task 1: Linked-list ring buffer registry

**Files:**
- Modify: `src/trace/trace.h`
- Modify: `src/trace/trace.cpp`
- Modify: `test/unit/test_ring.cpp`

**Step 1: Write the failing test**

Replace `test/unit/test_ring.cpp` with:

```cpp
#include "trace/trace.h"
#include "alloc/alloc.h"
#include <cassert>
#include <pthread.h>

// After init, the ring list must be non-null (main thread's buffer registered).
static void test_registry_non_null() {
    tbjit::alloc::init();
    tbjit::trace::init();
    // Trigger buffer creation for this thread
    void* fake = reinterpret_cast<void*>(0x1000);
    tbjit::trace::record_alloc(1, 48, fake);
    assert(tbjit::trace::ring_head() != nullptr);
}

// Two threads each push an event; both buffers appear in the list.
static void* thread_push(void*) {
    void* fake = reinterpret_cast<void*>(0x2000);
    tbjit::trace::record_alloc(2, 64, fake);
    return nullptr;
}

static void test_registry_multithread() {
    pthread_t t;
    pthread_create(&t, nullptr, thread_push, nullptr);
    pthread_join(t, nullptr);

    int count = 0;
    tbjit::trace::RingBuffer* n = tbjit::trace::ring_head();
    while (n) { ++count; n = n->next.load(std::memory_order_relaxed); }
    assert(count >= 2);
}

int main() {
    test_registry_non_null();
    test_registry_multithread();
    return 0;
}
```

**Step 2: Verify it fails**

```bash
cmake --build build -j$(nproc) 2>&1 | grep -E "error:|undefined"
```
Expected: compile errors — `ring_head()`, `RingBuffer::next` not defined.

**Step 3: Update `src/trace/trace.h`**

```cpp
#pragma once
#include "../common.h"
#include <atomic>
#include <cstddef>

namespace tbjit::trace {

struct RingBuffer {
    static constexpr size_t CAPACITY = 1024;

    AllocEvent slots[CAPACITY];
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    std::atomic<RingBuffer*> next{nullptr};

    // Returns true if buffer transitioned empty->non-empty (caller should wake background thread).
    bool push(const AllocEvent& ev);

    bool pop(AllocEvent& out);
};

void        init();
void        record_alloc(CallSiteID id, size_t size, void* ptr);
void        record_free(CallSiteID id, void* ptr);
RingBuffer* ring_head();   // head of the global linked list, for background thread

} // namespace tbjit::trace
```

**Step 4: Update `src/trace/trace.cpp`**

```cpp
#include "trace/trace.h"
#include "alloc/alloc.h"
#include <atomic>
#include <cstdint>

namespace tbjit::trace {

static std::atomic<RingBuffer*> g_ring_head{nullptr};

bool RingBuffer::push(const AllocEvent& ev) {
    uint64_t h   = head.load(std::memory_order_relaxed);
    uint64_t t   = tail.load(std::memory_order_acquire);
    bool was_empty = (h == t);
    if (h - t >= CAPACITY) return false; // drop on overflow
    slots[h & (CAPACITY - 1)] = ev;
    head.store(h + 1, std::memory_order_release);
    return was_empty;
}

bool RingBuffer::pop(AllocEvent& out) {
    uint64_t t = tail.load(std::memory_order_relaxed);
    uint64_t h = head.load(std::memory_order_acquire);
    if (t == h) return false;
    out = slots[t & (CAPACITY - 1)];
    tail.store(t + 1, std::memory_order_release);
    return true;
}

static RingBuffer* get_ring() {
    thread_local RingBuffer* tl_ring = nullptr;
    if (__builtin_expect(tl_ring == nullptr, 0)) {
        tl_ring = new (alloc::alloc(sizeof(RingBuffer), alignof(RingBuffer))) RingBuffer{};
        // CAS-prepend into global list
        RingBuffer* old_head;
        do {
            old_head = g_ring_head.load(std::memory_order_relaxed);
            tl_ring->next.store(old_head, std::memory_order_relaxed);
        } while (!g_ring_head.compare_exchange_weak(
            old_head, tl_ring, std::memory_order_release, std::memory_order_relaxed));
    }
    return tl_ring;
}

static uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

static uint32_t next_thread_id() {
    static std::atomic<uint32_t> counter{0};
    thread_local uint32_t id = counter.fetch_add(1, std::memory_order_relaxed);
    return id;
}

void init() {
    alloc::init();
}

void record_alloc(CallSiteID id, size_t size, void* ptr) {
    AllocEvent ev{id, static_cast<uint32_t>(size), rdtsc(), next_thread_id(), ptr};
    get_ring()->push(ev);
}

void record_free(CallSiteID id, void* ptr) {
    AllocEvent ev{id, 0, rdtsc(), next_thread_id(), ptr};
    get_ring()->push(ev);
}

RingBuffer* ring_head() {
    return g_ring_head.load(std::memory_order_acquire);
}

} // namespace tbjit::trace
```

**Step 5: Build and run test**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R test_ring
```
Expected: `test_ring  Passed`

**Step 6: Commit**

```bash
git add src/trace/trace.h src/trace/trace.cpp test/unit/test_ring.cpp
git commit -m "feat(trace): linked-list ring buffer registry with CAS prepend"
```

---

## Task 2: Futex wakeup from producer

**Files:**
- Create: `src/analysis/futex.h`
- Modify: `src/trace/trace.cpp`

**Step 1: Write the failing test**

Add to `test/unit/CMakeLists.txt`:
```cmake
add_unit_test(test_futex test_futex.cpp)
```

Create `test/unit/test_futex.cpp`:
```cpp
#include "analysis/futex.h"
#include <cassert>
#include <pthread.h>
#include <atomic>

static std::atomic<int> g_woken{0};

static void* waiter(void*) {
    tbjit::analysis::futex_wait();
    g_woken.store(1, std::memory_order_release);
    return nullptr;
}

static void test_wake() {
    pthread_t t;
    pthread_create(&t, nullptr, waiter, nullptr);
    // Give the thread time to park
    struct timespec ts{0, 5'000'000}; // 5ms
    nanosleep(&ts, nullptr);
    tbjit::analysis::futex_wake();
    pthread_join(t, nullptr);
    assert(g_woken.load() == 1);
}

int main() {
    test_wake();
    return 0;
}
```

**Step 2: Verify it fails**

```bash
cmake --build build -j$(nproc) 2>&1 | grep error
```
Expected: `analysis/futex.h` not found.

**Step 3: Create `src/analysis/futex.h`**

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>

namespace tbjit::analysis {

enum FutexState : uint32_t { AWAKE = 0, SLEEPING = 1 };

inline std::atomic<uint32_t>& futex_word() {
    static std::atomic<uint32_t> word{AWAKE};
    return word;
}

inline void futex_wait() {
    uint32_t expected = AWAKE;
    futex_word().compare_exchange_strong(expected, SLEEPING,
        std::memory_order_acq_rel, std::memory_order_relaxed);
    syscall(SYS_futex, futex_word().operator->(),
            FUTEX_WAIT, SLEEPING, nullptr, nullptr, 0);
    futex_word().store(AWAKE, std::memory_order_release);
}

inline void futex_wake() {
    futex_word().store(AWAKE, std::memory_order_release);
    syscall(SYS_futex, futex_word().operator->(),
            FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

// Call after a ring buffer push that returned true (empty->non-empty transition).
inline void notify_if_sleeping() {
    if (futex_word().load(std::memory_order_acquire) == SLEEPING)
        futex_wake();
}

} // namespace tbjit::analysis
```

**Step 4: Wire wakeup into `src/trace/trace.cpp`**

Add include at top:
```cpp
#include "analysis/futex.h"
```

In `record_alloc` and `record_free`, after `get_ring()->push(ev)`:
```cpp
if (get_ring()->push(ev))
    tbjit::analysis::notify_if_sleeping();
```

**Step 5: Build and run**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R test_futex
```
Expected: `test_futex  Passed`

**Step 6: Commit**

```bash
git add src/analysis/futex.h src/trace/trace.cpp test/unit/test_futex.cpp test/unit/CMakeLists.txt
git commit -m "feat(trace): futex wakeup on empty->non-empty ring buffer transition"
```

---

## Task 3: Exact size histogram

**Files:**
- Create: `src/analysis/histogram.h`
- Modify: `test/unit/CMakeLists.txt`
- Create: `test/unit/test_histogram.cpp`

**Step 1: Write the failing test**

Add to `test/unit/CMakeLists.txt`:
```cmake
add_unit_test(test_histogram test_histogram.cpp)
```

Create `test/unit/test_histogram.cpp`:
```cpp
#include "analysis/histogram.h"
#include <cassert>

static void test_exact_quantiles() {
    tbjit::analysis::ExactHistogram h;
    for (int i = 0; i < 1000; ++i) h.record(48);
    for (int i = 0; i < 1000; ++i) h.record(64);

    assert(h.count() == 2000);
    assert(h.quantile(0.50) == 48);
    assert(h.quantile(0.75) == 64);
    assert(h.quantile(0.99) == 64);
    assert(h.dominant_size() == 48); // most frequent
}

static void test_boundary() {
    tbjit::analysis::ExactHistogram h;
    h.record(0);
    h.record(4095);
    assert(h.quantile(0.0) == 0);
    assert(h.quantile(1.0) == 4095);
}

static void test_monomorphic() {
    tbjit::analysis::ExactHistogram h;
    for (int i = 0; i < 500; ++i) h.record(48);
    assert(h.is_monomorphic(0.95)); // 100% at size 48
}

int main() {
    test_exact_quantiles();
    test_boundary();
    test_monomorphic();
    return 0;
}
```

**Step 2: Verify it fails**

```bash
cmake --build build -j$(nproc) 2>&1 | grep error
```
Expected: `analysis/histogram.h` not found.

**Step 3: Create `src/analysis/histogram.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>

namespace tbjit::analysis {

// Exact histogram for allocation sizes 0–4095 bytes.
// Uses a 16KB fixed array — acceptable for MAX_CALL_SITES entries in the bump arena.
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
        uint64_t target = static_cast<uint64_t>(p * (total - 1));
        uint64_t cum = 0;
        for (uint32_t i = 0; i < MAX_SIZE; ++i) {
            cum += counts[i];
            if (cum > target) return i;
        }
        return MAX_SIZE - 1;
    }

    // Size with the highest count.
    uint32_t dominant_size() const {
        uint32_t best = 0;
        for (uint32_t i = 1; i < MAX_SIZE; ++i)
            if (counts[i] > counts[best]) best = i;
        return best;
    }

    // True if `fraction` of events are at a single size.
    bool is_monomorphic(double fraction) const {
        if (total == 0) return false;
        uint32_t dom = dominant_size();
        return counts[dom] >= static_cast<uint64_t>(fraction * total);
    }

    void reset() {
        for (auto& c : counts) c = 0;
        total = 0;
    }
};

} // namespace tbjit::analysis
```

**Step 4: Build and run**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R test_histogram
```
Expected: `test_histogram  Passed`

**Step 5: Commit**

```bash
git add src/analysis/histogram.h test/unit/test_histogram.cpp test/unit/CMakeLists.txt
git commit -m "feat(analysis): exact size histogram with quantile and monomorphic detection"
```

---

## Task 4: Two-phase KS stability test

**Files:**
- Create: `src/analysis/ks_test.h`
- Modify: `test/unit/CMakeLists.txt`
- Create: `test/unit/test_ks.cpp`

**Step 1: Write the failing test**

Add to `test/unit/CMakeLists.txt`:
```cmake
add_unit_test(test_ks test_ks.cpp)
```

Create `test/unit/test_ks.cpp`:
```cpp
#include "analysis/ks_test.h"
#include "analysis/histogram.h"
#include <cassert>

static void test_identical_distributions_stable() {
    tbjit::analysis::ExactHistogram a, b;
    for (int i = 0; i < 500; ++i) { a.record(48); b.record(48); }
    assert(tbjit::analysis::ks_stable(a, b, 0.05));
}

static void test_different_distributions_unstable() {
    tbjit::analysis::ExactHistogram a, b;
    for (int i = 0; i < 500; ++i) a.record(48);
    for (int i = 0; i < 500; ++i) b.record(128);
    assert(!tbjit::analysis::ks_stable(a, b, 0.05));
}

static void test_empty_histogram_unstable() {
    tbjit::analysis::ExactHistogram a, b;
    for (int i = 0; i < 500; ++i) a.record(48);
    assert(!tbjit::analysis::ks_stable(a, b, 0.05));
}

int main() {
    test_identical_distributions_stable();
    test_different_distributions_unstable();
    test_empty_histogram_unstable();
    return 0;
}
```

**Step 2: Verify it fails**

```bash
cmake --build build -j$(nproc) 2>&1 | grep error
```
Expected: `analysis/ks_test.h` not found.

**Step 3: Create `src/analysis/ks_test.h`**

```cpp
#pragma once
#include "histogram.h"
#include <cmath>

namespace tbjit::analysis {

// Two-sample KS test on ExactHistograms.
// Returns true if distributions are statistically indistinguishable at significance alpha.
// Uses the KS statistic D = max|F1(x) - F2(x)| and the asymptotic approximation.
inline bool ks_stable(const ExactHistogram& a, const ExactHistogram& b, double alpha) {
    if (a.count() == 0 || b.count() == 0) return false;

    double max_d = 0.0;
    double cum_a = 0.0, cum_b = 0.0;
    double n_a = static_cast<double>(a.count());
    double n_b = static_cast<double>(b.count());

    for (uint32_t i = 0; i < ExactHistogram::MAX_SIZE; ++i) {
        cum_a += a.counts[i] / n_a;
        cum_b += b.counts[i] / n_b;
        double d = std::abs(cum_a - cum_b);
        if (d > max_d) max_d = d;
    }

    // Critical value: c(alpha) * sqrt((n_a + n_b) / (n_a * n_b))
    // c(0.05) ≈ 1.3581
    double c_alpha = (alpha <= 0.05) ? 1.3581 : 1.2239; // c(0.10)
    double critical = c_alpha * std::sqrt((n_a + n_b) / (n_a * n_b));
    return max_d < critical;
}

} // namespace tbjit::analysis
```

**Step 4: Build and run**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R test_ks
```
Expected: `test_ks  Passed`

**Step 5: Commit**

```bash
git add src/analysis/ks_test.h test/unit/test_ks.cpp test/unit/CMakeLists.txt
git commit -m "feat(analysis): two-sample KS stability test on exact histograms"
```

---

## Task 5: CallSiteSummary + SizeWindow

**Files:**
- Modify: `src/analysis/analysis.h`
- Modify: `src/analysis/analysis.cpp`
- Modify: `test/unit/CMakeLists.txt`
- Create: `test/unit/test_analysis.cpp`

**Step 1: Write the failing test**

Add to `test/unit/CMakeLists.txt`:
```cmake
add_unit_test(test_analysis test_analysis.cpp)
```

Create `test/unit/test_analysis.cpp`:
```cpp
#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include <cassert>

static void test_prespec_stability_trigger() {
    tbjit::alloc::init();
    tbjit::analysis::init_state();

    // Feed 10 consecutive stable windows of 1000 events at size 48
    // Expected: after 10k events with stable distributions, phase -> Compiled
    for (int i = 0; i < 10'000; ++i) {
        tbjit::AllocEvent ev{1, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(1) == tbjit::analysis::Phase::Compiled);
}

static void test_prespec_unstable_no_trigger() {
    tbjit::analysis::reset_state();

    // Alternating sizes — never stable
    for (int i = 0; i < 10'000; ++i) {
        uint32_t size = (i % 2 == 0) ? 48 : 128;
        tbjit::AllocEvent ev{2, size, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    assert(tbjit::analysis::get_phase(2) == tbjit::analysis::Phase::PreSpec);
}

int main() {
    test_prespec_stability_trigger();
    test_prespec_unstable_no_trigger();
    return 0;
}
```

**Step 2: Verify it fails**

```bash
cmake --build build -j$(nproc) 2>&1 | grep error
```

**Step 3: Update `src/analysis/analysis.h`**

```cpp
#pragma once
#include "../common.h"
#include "histogram.h"

namespace tbjit::analysis {

enum class Phase : uint8_t { PreSpec, Compiled, Deopt };

struct SizeWindow {
    ExactHistogram hist;
    uint32_t       count{0};
    static constexpr uint32_t WINDOW_SIZE = 1000;

    bool full() const { return count >= WINDOW_SIZE; }
    void record(uint32_t size) { hist.record(size); ++count; }
    void reset() { hist.reset(); count = 0; }
};

struct CallSiteSummary {
    CallSiteID  id{0};
    Phase       phase{Phase::PreSpec};
    uint64_t    event_count{0};
    uint32_t    stable_windows{0};

    SizeWindow  windows[2];
    uint8_t     active{0};

    ExactHistogram baseline; // frozen at specialization time (post-spec KS)
    SizeWindow     post_window;

    Strategy    candidate{Strategy::Generic};
};

void  init();
void  init_state();   // initialize analysis tables (called from init)
void  reset_state();  // reset all summaries (for testing)
void  process_event(const AllocEvent& ev);
Phase get_phase(CallSiteID id);
void  run();          // background thread entry point

} // namespace tbjit::analysis
```

**Step 4: Update `src/analysis/analysis.cpp`**

```cpp
#include "analysis/analysis.h"
#include "analysis/ks_test.h"
#include "alloc/alloc.h"
#include <cstring>

namespace tbjit::analysis {

namespace {

constexpr size_t   MAX_CALL_SITES       = 4096;
constexpr uint32_t STABLE_WINDOWS_NEEDED = 10;
constexpr double   KS_ALPHA             = 0.05;
constexpr double   DEOPT_RATE_THRESHOLD = 0.01;

CallSiteSummary* g_summaries     = nullptr;
size_t           g_summary_count = 0;

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
    if (s->windows[prev].count > 0 &&
        ks_stable(s->windows[s->active].hist, s->windows[prev].hist, KS_ALPHA)) {
        ++s->stable_windows;
        if (s->stable_windows >= STABLE_WINDOWS_NEEDED) {
            s->baseline = s->windows[s->active].hist;
            // Infer candidate strategy
            if (s->windows[s->active].hist.is_monomorphic(0.95))
                s->candidate = Strategy::BumpAlloc;
            else
                s->candidate = Strategy::ThreadLocalFreeList;
            s->phase = Phase::Compiled;
            return;
        }
    } else {
        s->stable_windows = 0;
    }
    // Flip windows
    s->windows[s->active].reset();
    s->active = 1 - s->active;
}

void check_postspec(CallSiteSummary* s) {
    if (!ks_stable(s->post_window.hist, s->baseline, KS_ALPHA)) {
        s->phase = Phase::Deopt;
        s->stable_windows = 0;
        s->windows[0].reset();
        s->windows[1].reset();
        s->active = 0;
        s->post_window.reset();
    } else {
        s->post_window.reset();
    }
}

} // namespace

void init() {
    alloc::init();
    init_state();
}

void init_state() {
    if (!g_summaries)
        g_summaries = static_cast<CallSiteSummary*>(
            alloc::alloc(sizeof(CallSiteSummary) * MAX_CALL_SITES,
                         alignof(CallSiteSummary)));
    reset_state();
}

void reset_state() {
    g_summary_count = 0;
    for (size_t i = 0; i < MAX_CALL_SITES; ++i)
        new (&g_summaries[i]) CallSiteSummary{};
}

void process_event(const AllocEvent& ev) {
    if (ev.size == 0 || ev.size >= ExactHistogram::MAX_SIZE) return; // free events or large allocs
    CallSiteSummary* s = find_or_create(ev.call_site);
    if (!s) return;
    ++s->event_count;

    if (s->phase == Phase::PreSpec || s->phase == Phase::Deopt) {
        if (s->phase == Phase::Deopt) s->phase = Phase::PreSpec;
        SizeWindow& w = s->windows[s->active];
        w.record(ev.size);
        if (w.full()) advance_prespec(s);
    } else { // Compiled
        s->post_window.record(ev.size);
        if (s->post_window.full()) check_postspec(s);
    }
}

Phase get_phase(CallSiteID id) {
    for (size_t i = 0; i < g_summary_count; ++i)
        if (g_summaries[i].id == id) return g_summaries[i].phase;
    return Phase::PreSpec;
}

void run() {
    // Background thread loop — implemented in Task 6
}

} // namespace tbjit::analysis
```

**Step 5: Build and run**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R test_analysis
```
Expected: `test_analysis  Passed`

**Step 6: Commit**

```bash
git add src/analysis/analysis.h src/analysis/analysis.cpp test/unit/test_analysis.cpp test/unit/CMakeLists.txt
git commit -m "feat(analysis): CallSiteSummary with two-phase KS stability detection"
```

---

## Task 6: Background thread with adaptive drain loop

**Files:**
- Modify: `src/analysis/analysis.cpp`
- Modify: `src/analysis/analysis.h`
- Modify: `src/trace/trampoline.cpp`

**Step 1: Write the failing test**

Add to `test/unit/CMakeLists.txt`:
```cmake
add_unit_test(test_background test_background.cpp)
```

Create `test/unit/test_background.cpp`:
```cpp
#include "analysis/analysis.h"
#include "trace/trace.h"
#include "alloc/alloc.h"
#include <cassert>
#include <unistd.h>

static void test_background_drains_events() {
    tbjit::alloc::init();
    tbjit::analysis::init_state();
    tbjit::trace::init();
    tbjit::analysis::start_background_thread();

    for (int i = 0; i < 100; ++i)
        tbjit::trace::record_alloc(99, 48, reinterpret_cast<void*>(0x1000 + i));

    // Give background thread time to drain
    usleep(50'000); // 50ms

    assert(tbjit::analysis::events_processed() >= 100);
    tbjit::analysis::stop_background_thread();
}

int main() {
    test_background_drains_events();
    return 0;
}
```

**Step 2: Verify it fails**

```bash
cmake --build build -j$(nproc) 2>&1 | grep error
```

**Step 3: Add thread control to `src/analysis/analysis.h`**

Add to the public API section:
```cpp
void     start_background_thread();
void     stop_background_thread();
uint64_t events_processed();
```

**Step 4: Implement background thread in `src/analysis/analysis.cpp`**

Add includes:
```cpp
#include "analysis/futex.h"
#include "trace/trace.h"
#include <pthread.h>
#include <time.h>
#include <atomic>
```

Add in the anonymous namespace:
```cpp
std::atomic<bool>     g_running{false};
std::atomic<uint64_t> g_events_processed{0};
pthread_t             g_thread;

bool drain_all() {
    bool found = false;
    tbjit::trace::RingBuffer* rb = tbjit::trace::ring_head();
    while (rb) {
        AllocEvent ev;
        while (rb->pop(ev)) {
            process_event(ev);
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
            backoff_us = backoff_us * 2 < 500 ? backoff_us * 2 : 500;
        } else {
            futex_wait();
        }
    }
    drain_all(); // final drain on shutdown
    return nullptr;
}
```

Add public implementations:
```cpp
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
```

Update `run()` to call `start_background_thread()` (for production use from trampoline).

**Step 5: Wire background thread into `src/trace/trampoline.cpp`**

In `tbjit_init()`, after existing inits:
```cpp
tbjit::analysis::init();
tbjit::analysis::start_background_thread();
```

**Step 6: Build and run**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R test_background
```
Expected: `test_background  Passed`

**Step 7: Commit**

```bash
git add src/analysis/analysis.h src/analysis/analysis.cpp src/trace/trampoline.cpp test/unit/test_background.cpp test/unit/CMakeLists.txt
git commit -m "feat(analysis): adaptive background drain loop with futex parking"
```

---

## Task 7: Binary trace writer

**Files:**
- Create: `src/trace/writer.h`
- Create: `src/trace/writer.cpp`
- Modify: `CMakeLists.txt` (add writer.cpp to TBJIT_SOURCES)
- Modify: `src/analysis/analysis.cpp` (call writer in drain loop)

**Step 1: Write the failing test**

Add to `test/unit/CMakeLists.txt`:
```cmake
add_unit_test(test_writer test_writer.cpp)
```

Create `test/unit/test_writer.cpp`:
```cpp
#include "trace/writer.h"
#include "alloc/alloc.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static void test_write_and_read_back() {
    tbjit::alloc::init();
    const char* path = "/tmp/tbjit_test_trace.bin";
    tbjit::trace::writer_open(path);

    tbjit::AllocEvent ev{42, 48, 12345, 1, reinterpret_cast<void*>(0xdeadbeef)};
    tbjit::trace::writer_write(ev);
    tbjit::trace::writer_close();

    // Read back and verify
    FILE* f = fopen(path, "rb");
    assert(f != nullptr);

    tbjit::trace::TraceHeader hdr;
    assert(fread(&hdr, sizeof(hdr), 1, f) == 1);
    assert(hdr.magic == tbjit::trace::TRACE_MAGIC);

    tbjit::AllocEvent read_ev;
    assert(fread(&read_ev, sizeof(read_ev), 1, f) == 1);
    assert(read_ev.call_site == 42);
    assert(read_ev.size == 48);
    assert(read_ev.ptr == reinterpret_cast<void*>(0xdeadbeef));

    fclose(f);
    remove(path);
}

int main() {
    test_write_and_read_back();
    return 0;
}
```

**Step 2: Verify it fails**

```bash
cmake --build build -j$(nproc) 2>&1 | grep error
```

**Step 3: Create `src/trace/writer.h`**

```cpp
#pragma once
#include "../common.h"
#include <cstdint>

namespace tbjit::trace {

constexpr uint32_t TRACE_MAGIC   = 0x54424A54; // "TBJT"
constexpr uint32_t TRACE_VERSION = 1;

struct TraceHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t timestamp_ns; // process start
};

void writer_open(const char* path);
void writer_write(const AllocEvent& ev);
void writer_close();
bool writer_active();

} // namespace tbjit::trace
```

**Step 4: Create `src/trace/writer.cpp`**

```cpp
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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    TraceHeader hdr{TRACE_MAGIC, TRACE_VERSION,
                    static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec};
    fwrite(&hdr, sizeof(hdr), 1, g_file);
}

void writer_write(const AllocEvent& ev) {
    if (g_file) fwrite(&ev, sizeof(ev), 1, g_file);
}

void writer_close() {
    if (g_file) { fflush(g_file); fclose(g_file); g_file = nullptr; }
}

bool writer_active() { return g_file != nullptr; }

} // namespace tbjit::trace
```

**Step 5: Add `writer.cpp` to `CMakeLists.txt`**

```cmake
set(TBJIT_SOURCES
  ...
  src/trace/writer.cpp
  ...
)
```

**Step 6: Wire writer into drain loop in `src/analysis/analysis.cpp`**

In `drain_all()`, after `process_event(ev)`:
```cpp
if (tbjit::trace::writer_active())
    tbjit::trace::writer_write(ev);
```

**Step 7: Open writer in `tbjit_init()` in `trampoline.cpp`**

```cpp
const char* trace_path = getenv("TBJIT_TRACE");
if (trace_path) tbjit::trace::writer_open(trace_path);
```

**Step 8: Build and run**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R test_writer
```
Expected: `test_writer  Passed`

**Step 9: Commit**

```bash
git add src/trace/writer.h src/trace/writer.cpp CMakeLists.txt src/analysis/analysis.cpp src/trace/trampoline.cpp test/unit/test_writer.cpp test/unit/CMakeLists.txt
git commit -m "feat(trace): binary trace writer, enabled via TBJIT_TRACE=<path>"
```

---

## Task 8: TBJIT_DUMP human summary

**Files:**
- Modify: `src/analysis/analysis.h` (add `dump_stats()`)
- Modify: `src/analysis/analysis.cpp`
- Modify: `src/trace/trampoline.cpp` (destructor)

**Step 1: Write the failing test**

Add to `test/unit/CMakeLists.txt`:
```cmake
add_unit_test(test_dump test_dump.cpp)
```

Create `test/unit/test_dump.cpp`:
```cpp
#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include <cassert>
#include <cstdio>

static void test_dump_does_not_crash() {
    tbjit::alloc::init();
    tbjit::analysis::init_state();
    for (int i = 0; i < 500; ++i) {
        tbjit::AllocEvent ev{7, 48, 0, 0, nullptr};
        tbjit::analysis::process_event(ev);
    }
    // Redirect stderr to /dev/null, call dump, assert no crash
    FILE* old = stderr;
    stderr = fopen("/dev/null", "w");
    tbjit::analysis::dump_stats();
    fclose(stderr);
    stderr = old;
}

int main() {
    test_dump_does_not_crash();
    return 0;
}
```

**Step 2: Verify it fails**

```bash
cmake --build build -j$(nproc) 2>&1 | grep error
```

**Step 3: Add `dump_stats()` to `src/analysis/analysis.h`**

```cpp
void dump_stats();
```

**Step 4: Implement `dump_stats()` in `src/analysis/analysis.cpp`**

```cpp
void dump_stats() {
    fprintf(stderr, "\n%-18s %10s %6s %6s %6s  %-8s  %s\n",
            "CALL SITE", "EVENTS", "P50", "P95", "P99", "STABLE", "STRATEGY");
    fprintf(stderr, "%-18s %10s %6s %6s %6s  %-8s  %s\n",
            "----------", "------", "---", "---", "---", "------", "--------");
    for (size_t i = 0; i < g_summary_count; ++i) {
        const CallSiteSummary& s = g_summaries[i];
        const ExactHistogram& h  = (s.phase == Phase::Compiled)
                                   ? s.baseline
                                   : s.windows[s.active].hist;
        const char* stable   = (s.phase == Phase::Compiled) ? "YES" : "NO";
        const char* strategy = [&]() -> const char* {
            switch (s.candidate) {
                case Strategy::BumpAlloc:           return "BumpAlloc";
                case Strategy::ThreadLocalFreeList: return "TLFreeList";
                case Strategy::EpochArena:          return "EpochArena";
                case Strategy::PairedStack:         return "PairedStack";
                default:                            return "—";
            }
        }();
        fprintf(stderr, "0x%-16lx %10lu %6u %6u %6u  %-8s  %s\n",
                static_cast<unsigned long>(s.id),
                static_cast<unsigned long>(s.event_count),
                h.quantile(0.50),
                h.quantile(0.95),
                h.quantile(0.99),
                stable, strategy);
    }
}
```

**Step 5: Wire dump into trampoline destructor**

In `src/trace/trampoline.cpp`, add:
```cpp
__attribute__((destructor))
void tbjit_fini() {
    tbjit::analysis::stop_background_thread();
    tbjit::trace::writer_close();
    if (getenv("TBJIT_DUMP"))
        tbjit::analysis::dump_stats();
}
```

**Step 6: Build and run**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R test_dump
```
Expected: `test_dump  Passed`

**Step 7: Commit**

```bash
git add src/analysis/analysis.h src/analysis/analysis.cpp src/trace/trampoline.cpp test/unit/test_dump.cpp test/unit/CMakeLists.txt
git commit -m "feat(analysis): TBJIT_DUMP human-readable per-call-site summary on exit"
```

---

## Task 9: Integration test — throughput bench under LD_PRELOAD

**Files:**
- Modify: `test/integration/CMakeLists.txt`
- Modify: `test/integration/hello.cpp`

**Step 1: Expand `hello.cpp` to produce a dumpable workload**

```cpp
#include <cstdlib>
#include <cstring>
#include <cassert>

int main() {
    // Hot call site: monomorphic 48-byte allocs — should trigger BumpAlloc
    for (int i = 0; i < 12'000; ++i) {
        void* p = malloc(48);
        assert(p != nullptr);
        free(p);
    }
    // Cold call site: mixed sizes — should stay in PreSpec
    for (int i = 0; i < 100; ++i) {
        void* p = malloc((i % 3 == 0) ? 64 : 128);
        assert(p != nullptr);
        free(p);
    }
    return 0;
}
```

**Step 2: Add dump-checking integration test**

In `test/integration/CMakeLists.txt`:
```cmake
add_test(
  NAME int_dump_output
  COMMAND ${CMAKE_COMMAND} -E env
    LD_PRELOAD=$<TARGET_FILE:tbjit>
    TBJIT_DUMP=1
    $<TARGET_FILE:int_hello>
)
```

**Step 3: Build and run**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V -R int_dump_output
```
Expected: test passes (exit 0) and stderr shows the call-site table.

**Step 4: Manual verification**

```bash
LD_PRELOAD=build/libtbjit.so TBJIT_DUMP=1 build/test/integration/int_hello
```
Expected output (approximate):
```
CALL SITE           EVENTS        P50    P95    P99  STABLE    STRATEGY
----------          ------        ---    ---    ---  ------    --------
0x...               12000          48     48     48  YES       BumpAlloc
0x...                 100          64    128    128  NO        —
```

**Step 5: Commit**

```bash
git add test/integration/hello.cpp test/integration/CMakeLists.txt
git commit -m "test(integration): LD_PRELOAD end-to-end dump verification"
```

---

## All tasks complete

Run full test suite:
```bash
cmake --build build -j$(nproc) && ctest --test-dir build -V
```
All tests should pass.
