# tbjit-alloc

> An allocator that traces its own behavior, identifies hot allocation patterns, and emits specialized x86-64 routines for them at runtime — deoptimizing and respecializing when workload assumptions break.

General-purpose allocators (jemalloc, mimalloc, tcmalloc) fix their internal policies at compile time. tbjit-alloc treats those policies as JIT compilation targets: observe, specialize, invalidate, repeat.

---

## Architecture

```
malloc(n)
    │
    ▼
[Trampoline]  ── return-address hash ──►  [Dispatch Table]
                                                │
                ┌───────────────┬───────────────┼───────────────┐
                ▼               ▼               ▼               ▼
         [BumpAlloc]   [ThreadLocalFreeList] [EpochArena]   [Generic]
                │               │               │
           guard fail?     guard fail?     guard fail?
                ▼               ▼               ▼
                └───────► [Deopt Handler] ──► revert ──► respecialize
                                │
                                ▼
                  [Background Analysis Thread]
                  drains trace ring, drives KS test,
                  picks strategy, calls codegen,
                  installs into dispatch table.
```

**Tracing** — `LD_PRELOAD` interposition on `malloc` / `free`. The interceptor hashes the return address to a `CallSiteID`, looks up a JIT routine in the dispatch table, and falls through to glibc on miss. Per-thread SPSC ring buffers fan events into a single background analysis thread; allocator threads never block on the analyzer.

**Analysis** — One `CallSiteSummary` per call site: exact size histogram (0..4096 bytes), two rotating size windows of 1000 events each, KS two-sample test for stability, deopt counter, blacklist flag. After 10 consecutive stable windows the analyzer picks a strategy and triggers codegen.

**Codegen** — Raw x86-64 byte emitter targeting three strategies (a fourth is reserved in `common.h` but not implemented):

| Strategy | When picked | Fast path | Slow path |
|---|---|---|---|
| `BumpAlloc` | monomorphic site, ≥95% one size | 7 instructions: guard, load bump ptr, advance, bounds check, store, ret | mmap region on first call; deopt on exhaust |
| `ThreadLocalFreeList` | non-monomorphic stable site | 5 instructions: guard, load head, test, pop next, store, ret | mmap region and chop into linked chunks |
| `EpochArena` | (force only) | same as BumpAlloc fast path | recycles region in place instead of deopting |
| `PairedStack` | reserved | — | not implemented |

The exhaust path matters: `BumpAlloc` deopts when its 256 KiB region fills, `EpochArena` recycles, `ThreadLocalFreeList` only deopts on a wrong-size guard failure.

**Deoptimization** — Guard-fail in a JIT routine calls `deopt::handle`, which atomically reverts the dispatch entry to the generic path and enqueues the call site for background reset. The background thread is the sole writer of summary state and the pending-reclaim list, so the malloc hot path takes no locks. Code pages are reclaimed only when every registered allocator thread has passed a `mark_safe_point()` past the deopt's epoch — guarantees no thread is executing the freed page.

**Blacklist** — A call site that deopts ≥3 times is marked ineligible for recompilation. Avoids pathological compile/deopt loops on truly polymorphic sites.

**Shadow Validator** — `libtbjit_shadow.so` (compiled with `-DTBJIT_SHADOW`) records every JIT-served allocation in an open-addressing hash table and verifies on free. Aborts on null returns, misaligned pointers, double-allocs into a live slot. ~2× overhead; not for production.

---

## Research Questions

1. **Pattern regularity** — What fraction of real-program allocation behavior is stable enough to specialize? If hot call sites show high size/lifetime variance, the premise fails.
2. **Stability window** — Is 10×1000-event windows the right threshold? Smaller wastes compilation on transient patterns; larger leaves the JIT cold for too long.
3. **Trampoline break-even** — At ~5–10 ns dispatch cost (per malloc), does TLFreeList's allocator-level work amortize?
4. **Phase transition correctness** — Worst-case respecialization latency during a workload phase shift. Currently bounded by 11k events + compile time.

---

## Building

```bash
git clone https://github.com/AveryClapp/tbjit-alloc
cd tbjit-alloc
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requires: Linux x86-64 for the full JIT path (TLS offsets via `__builtin_thread_pointer`). macOS x86-64 / arm64 builds and unit-tests pass, but the `LD_PRELOAD` integration tests no-op there.

GCC 12+ or Clang 15+, CMake 3.20+.

### Usage

```bash
# Basic: route every malloc through tbjit
LD_PRELOAD=build/libtbjit.so ./your-program

# Dump per-call-site analysis on exit
LD_PRELOAD=build/libtbjit.so TBJIT_DUMP=1 ./your-program

# Force a specific JIT strategy (bypasses is_monomorphic)
LD_PRELOAD=build/libtbjit.so TBJIT_FORCE_STRATEGY=arena ./your-program

# Record allocation events to a trace file
LD_PRELOAD=build/libtbjit.so TBJIT_TRACE=alloc.trace ./your-program

# Shadow validator
LD_PRELOAD=build/libtbjit_shadow.so ./your-program
```

### Tools

```bash
# Replay a recorded trace through the analyzer offline
build/tools/tbjit_replay alloc.trace
```

### Benchmarks

```bash
# Four workload patterns × every strategy, side-by-side ns/op table
bash bench/run_bench.sh build
```

Patterns: `monomorphic` (tight churn), `arena` (alloc many → free many → repeat), `mixed` (rotating sizes — polymorphic), `pulse` (bursts with quiet periods).

---

## Layout

```
src/
  trace/        LD_PRELOAD trampoline, SPSC ring buffer, trace writer
  analysis/     CallSiteSummary, KS test, exact histogram, background loop
  codegen/      x86-64 emitter, TLS slots, slow-init / refill / reset helpers
  dispatch/     atomic dispatch table; install / revert / lookup
  deopt/        epoch tracking, deopt queue, page reclamation
  alloc/        internal bump arena (libtbjit's own metadata — never glibc)
  shadow/       optional validator (libtbjit_shadow only)
test/
  unit/         per-module tests; runs on Linux + macOS
  integration/  LD_PRELOAD scenarios; Linux-only ctest tests
bench/          four pattern binaries + run_bench.sh
tools/          tbjit_replay
docs/plans/     design docs (gitignored)
```

---

## Prior Art

| Work | Mechanism | Gap |
|---|---|---|
| jemalloc prof | Offline profile-guided size class tuning | Static, no runtime codegen |
| Region inference (Tofte & Talpin '97) | Compiler-inserted region annotations | Not runtime-adaptive |
| Mesh (Curtsinger '19) | Compacting via virtual memory remapping | Unrelated mechanism |
| HotSpot adaptive compilation | JIT respecialization of application code | One layer up from allocator |

The idea of applying trace-based JIT compilation to the allocator itself — rather than to the program it serves — does not appear in the literature we've reviewed.

---

## Status

- **Working**: trampoline, trace pipeline, analysis with KS stability, BumpAlloc / TLFreeList / EpochArena codegen + deopt loop, page reclamation, shadow validator, four-pattern bench suite.
- **Known limits**: cross-thread free pushes onto the freeing thread's list (imbalanced but correct); arena/bump regions leak until process exit; `PairedStack` strategy reserved but not implemented; multi-size-class free lists deferred.
- **CI**: GitHub Actions builds + runs the full ctest suite on Ubuntu, then captures the bench output and `int_hello` dump as artifacts in the workflow log.

---

## License

MIT
