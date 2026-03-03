# tbjit-alloc

> A memory allocator that traces its own behavior, identifies hot allocation patterns, and emits specialized x86-64 routines for them at runtime — deoptimizing and respecializing when workload assumptions break.

General-purpose allocators (jemalloc, mimalloc, tcmalloc) fix their internal policies at compile time. tbjit-alloc treats those policies as JIT compilation targets: observe, specialize, invalidate, repeat.

---

## Architecture

```
malloc(n)
    │
    ▼
[Trampoline] ── return address hash ──► [Dispatch Table]
                                               │
                         ┌─────────────────────┼─────────────────────┐
                         ▼                     ▼                     ▼
                  [JIT'd Routine]        [JIT'd Routine]       [Generic Alloc]
                  BumpAlloc/48b      ThreadLocalFreeList          (fallback)
                         │
                    guard fail?
                         ▼
                  [Deopt Handler] ──► invalidate ──► respecialize
```

**Tracing** — LD_PRELOAD interposition on malloc/free. Per-thread lock-free ring buffers flushed to a background analyzer. The tracer never calls malloc. Target: <50ns per event.

**Analysis** — Per-call-site summaries: size distributions (exact histogram <4KB, DDSketch above), lifetime distributions, thread affinity, free-site correlation, temporal clustering. Stability via two-sample KS test every 1k events. Specialization triggers after 10k stable allocations.

**Codegen** — x86-64 emitter targeting four strategies:

| Strategy | Condition | Fast-path cost |
|---|---|---|
| `BumpAlloc` | monomorphic size, short lifetime, thread-local free | 2 instructions + bounds check |
| `ThreadLocalFreeList` | high thread affinity, variance in lifetime | push/pop, no atomics |
| `EpochArena` | strong temporal clustering | bump + epoch counter |
| `PairedStack` | alloc/free call sites strongly correlated | matched push/pop |

Guards at the top of every compiled routine. Trampoline dispatch costs ~5-10ns when the dispatch table is cache-hot.

**Deoptimization** — Deopt rate >1% over 1k calls invalidates the compiled routine, reverts the dispatch entry to the generic path, and restarts analysis. Code pages are reclaimed via epoch-based safe-point tracking — a page is freed only when all threads have exited the invalidated routine.

**Shadow Validator** — Development mode runs the generic allocator in parallel and asserts size, alignment, and free acceptance match the JIT'd path. ~2x overhead. Not for production.

---

## Research Questions

1. **Pattern regularity** — What fraction of real-program allocation behavior is stable enough to specialize? If hot call sites show high size/lifetime variance, the premise fails.
2. **Stability window** — What threshold minimizes wasted JIT work on transient patterns while capturing durable ones? Currently 10k events; empirically unknown optimal.
3. **Trampoline break-even** — For cheap strategies (ThreadLocalFreeList), does the 5-10ns dispatch cost dominate savings? If so, direct call-site patching may be required.
4. **Phase transition correctness** — What is the worst-case respecialization latency during a workload phase shift, and is deopt/reinstrumentation fast enough to not crater tail latency?

---

## Building

```bash
git clone https://github.com/your-handle/tbjit-alloc
cd tbjit-alloc
make
```

Requires: Linux x86-64, GCC 12+ or Clang 15+, kernel 5.4+ (`perf_event_open`).

```bash
# Basic usage
LD_PRELOAD=./libtbjit.so ./your-program

# Shadow validation (development)
LD_PRELOAD=./libtbjit.so TBJIT_SHADOW=1 ./your-program

# Dump per-call-site analysis
LD_PRELOAD=./libtbjit.so TBJIT_DUMP=1 ./your-program 2>trace.txt
```

---

## Evaluation

Baseline comparisons: jemalloc 5.x, mimalloc 2.x, and a hand-tuned arena for the Redis workload (upper bound for offline profiling). Primary workload: Redis under YCSB read-heavy.

Metrics: allocation throughput, P99/P999 latency, peak RSS. Results pending.

---

## Prior Art

| Work | Mechanism | Gap |
|---|---|---|
| jemalloc prof | Offline profile-guided size class tuning | Static, no runtime codegen |
| Region inference (Tofte & Talpin '97) | Compiler-inserted region annotations | Not runtime-adaptive |
| Mesh (Curtsinger '19) | Compacting via virtual memory remapping | Unrelated mechanism |
| HotSpot adaptive compilation | JIT respecialization of application code | One layer up from allocator |

The idea of applying trace-based JIT compilation to the allocator itself — rather than to the program it serves — does not appear in the literature.

---

## License

MIT
