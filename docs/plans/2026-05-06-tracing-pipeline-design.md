# Tracing Pipeline Design

**Goal:** End-to-end tracing — trampoline → ring buffer → analyzer → `TBJIT_DUMP` outputs per-call-site stats. No codegen yet. Answers Research Question 1: what fraction of real-program allocation behavior is stable enough to specialize?

---

## Decisions

| Topic | Decision | Rationale |
|---|---|---|
| Ring buffer registry | Lock-free intrusive linked list (CAS prepend) | No thread count limit; cleaner story for real systems |
| Background thread wakeup | Adaptive polling + futex parking | Sub-100µs drain latency at zero hot-path overhead in steady state |
| TBJIT_DUMP output | Binary trace file + human-readable summary | Binary enables offline replay for paper evaluation; human summary for development |
| KS test phasing | Two-phase: rolling windows pre-spec, frozen baseline post-spec | Pre-spec asks "has this settled?"; post-spec asks "has this drifted?" — different questions, different tests |

---

## Section 1: Ring Buffer Registry + Background Thread Lifecycle

Each `RingBuffer` has an atomic `next` pointer making it a node in a singly-linked list. Global `std::atomic<RingBuffer*> g_ring_head` is the list head.

On first malloc from a new thread, `get_ring()` allocates a `RingBuffer` from the bump arena and CAS-prepends:

```cpp
do {
    node->next = g_ring_head.load(relaxed);
} while (!g_ring_head.compare_exchange_weak(node->next, node, release, relaxed));
```

No deregistration on thread exit — buffer stays in list, bump arena owns it, background thread drains it harmlessly. Eliminates thread-exit lifecycle complexity entirely.

Background thread: created once in `tbjit_init()` as `pthread` with `PTHREAD_CREATE_DETACHED`. Never joins, never exits.

---

## Section 2: Adaptive Drain Loop

```
while (true) {
    bool found_work = drain_all_buffers();
    if (found_work) {
        backoff = 1µs;
        continue;
    }
    sleep(backoff);
    backoff = min(backoff * 2, 500µs);
    if (backoff == 500µs)
        futex_wait(&g_futex, SLEEPING);
}
```

`drain_all_buffers()` walks the linked list, reads `head` and `tail` atomically per buffer, processes pending events. Returns true if any events consumed.

Producer side: after `head` store, check if buffer transitioned empty→non-empty (`old_head == tail`). If so and background thread is parked, `futex_wake(1)`. Syscall only on cold path.

---

## Section 3: Per-Call-Site Analysis State

```cpp
enum class Phase : uint8_t { PreSpec, Compiled, Deopt };

struct CallSiteSummary {
    CallSiteID          id;
    Phase               phase;
    uint64_t            event_count;

    // Pre-spec: two rotating 1k-event windows, KS test on flip
    SizeWindow          window[2];
    uint8_t             active_window;

    // Post-spec: frozen baseline from compile time
    SizeDistribution    baseline;
    SizeWindow          current_window;

    Strategy            candidate_strategy;
};
```

**Pre-spec:** events fill `window[active_window]`. Every 1k events, KS test against `window[1 - active_window]`. p-value > 0.05 increments stability counter. After 10 consecutive stable windows (10k events), trigger codegen → `Compiled`.

**Post-spec:** events fill `current_window`. Every 1k events, KS test against frozen `baseline`. Deopt rate > 1% over window → `deopt::handle()` → back to `PreSpec`.

**Size distributions:**
- < 4096 bytes: exact count array `uint32_t counts[4096]` — zero-error quantiles, 16KB per call site
- ≥ 4096 bytes: DDSketch, ε=0.01, logarithmic bucket mapping
- 4KB boundary keeps total memory: `MAX_CALL_SITES * 16KB = 64MB` in bump arena

---

## Section 4: Binary Trace + TBJIT_DUMP Output

**Binary trace** (`TBJIT_TRACE=<path>`): file header + packed `AllocEvent` records written by the background thread after draining. Replayable offline for re-running analysis with different threshold parameters without re-running the target program.

**Human summary** (`TBJIT_DUMP=1`): per-call-site table on stderr at process exit.

```
CALL SITE        EVENTS    P50    P95    P99    STABLE    STRATEGY
0x4a1f3c         142,301   48     48     64     YES       BumpAlloc
0x3b2e11          18,442   128    512    1024   NO        —
...
```

Columns: return address, total event count, size p50/p95/p99, stability flag, suggested strategy.

---

## Implementation Order

1. `alloc/` — already complete
2. `trace/` — ring buffer with linked-list registry, futex wakeup signal
3. `analysis/` — background thread, drain loop, `CallSiteSummary`, exact histogram, KS test stub
4. `dispatch/` — already complete
5. `deopt/` — epoch tracking (stub until codegen)
6. Binary trace writer
7. `TBJIT_DUMP` human summary
8. Integration test: run `bench/throughput` under `LD_PRELOAD`, verify dump output
