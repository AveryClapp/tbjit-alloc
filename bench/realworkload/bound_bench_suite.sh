#!/usr/bin/env bash
# Bound-replay benchmark suite (Comparison A of the 2026-06-02 plan).
#
# For each workload in the corpus: capture a complete unspecialized trace
# (capture_trace.sh), pick the dominant process's trace (largest file), then
# run tbjit_bound_bench over it for every backend (glibc / bound / sim),
# appending to a results TSV:
#
#   backend  workload  p50_ns  ci95_ns  peak_rss_kb  matched_free_frac
#
# Favorable corpus (size-stable, allocation-bound): openssl_crypto, perl_bench.
# Control: gcc_compile (polymorphic) — bound should land ~= glibc, proving we
# are not cherry-picking a hot loop.
#
# Usage: bound_bench_suite.sh [build-dir] [out-dir]
# Exits 0 on partial results (a skipped workload is not fatal); non-zero only on
# infrastructure failure (no binary).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"
OUT_DIR="${2:-$REPO_ROOT/bench-out/boundbench}"
PASSES="${BOUND_BENCH_PASSES:-15}"

BENCH="$BUILD_DIR/tools/tbjit_bound_bench"
if [[ ! -x "$BENCH" ]]; then
  echo "bound_bench not built: $BENCH" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
TRACE_DIR="$OUT_DIR/traces"
TSV="$OUT_DIR/bound_results.tsv"
printf "backend\tworkload\tp50_ns\tci95_ns\tpeak_rss_kb\tmatched_free_frac\n" > "$TSV"

CORPUS=(openssl_crypto perl_bench gcc_compile)
BACKENDS=(glibc bound sim)

for name in "${CORPUS[@]}"; do
  spec="$SCRIPT_DIR/workloads/${name}.sh"
  echo "============================================================"
  if [[ ! -f "$spec" ]]; then
    echo "[skip $name] no spec at $spec" >&2
    continue
  fi

  if ! "$SCRIPT_DIR/capture_trace.sh" "$spec" "$TRACE_DIR"; then
    rc=$?
    [[ $rc -eq 2 ]] && echo "[skip $name] precondition" >&2 \
                    || echo "[skip $name] capture failed (rc=$rc)" >&2
    continue
  fi

  # Dominant process = largest trace file for this workload.
  shopt -s nullglob
  traces=("$TRACE_DIR/${name}".*.trace)
  shopt -u nullglob
  if [[ ${#traces[@]} -eq 0 ]]; then
    echo "[skip $name] no trace produced" >&2
    continue
  fi
  trace=$(ls -S "${traces[@]}" | head -1)
  echo "[bench $name] trace=$trace ($(du -h "$trace" | cut -f1))"

  for backend in "${BACKENDS[@]}"; do
    # --profile appends a PROFILE line (alloc vs free cyc/op, segment count,
    # jit-served fraction) to the per-backend log for root-cause analysis.
    if ! "$BENCH" "$trace" --backend "$backend" --label "$name" --passes "$PASSES" \
         --profile >> "$TSV" 2> "$OUT_DIR/${name}.${backend}.log"; then
      echo "[bench $name/$backend] failed (see ${name}.${backend}.log)" >&2
    fi
  done

  # Traces are large and regenerable; drop them so the artifact stays small.
  rm -f "${traces[@]}"
done

rmdir "$TRACE_DIR" 2>/dev/null || true

echo "============================================================"
echo "Results: $TSV"
column -t -s$'\t' < "$TSV" || cat "$TSV"

# Fidelity gate: flag favorable workloads whose matched-free fraction is low
# (dropped events on ring overflow, or frees of pre-capture pointers).
awk -F'\t' 'NR>1 && ($2=="openssl_crypto" || $2=="perl_bench") && $6+0 < 0.98 {
  printf "WARNING: %s/%s matched_free_frac=%s < 0.98 (fidelity)\n", $2,$1,$6 > "/dev/stderr"
}' "$TSV"
