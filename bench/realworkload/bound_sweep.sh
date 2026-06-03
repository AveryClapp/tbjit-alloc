#!/usr/bin/env bash
# Memory-frontier sweep (RSS vs per-alloc latency) over the three memory knobs:
#
#   TBJIT_HIST_CAP   (compile-time) sparse-histogram capacity -> profiling RSS
#   TBJIT_SEG_SHIFT  (compile-time) segment granularity       -> segment slack
#   TBJIT_REAP_MODE  (runtime)      conservative|eager|madvise -> segment return
#
# We do NOT search for a single "optimal" config: with a 3-workload corpus that
# would overfit. Instead we trace the frontier ONE KNOB AT A TIME from a
# baseline (CAP=64, SHIFT=21, reap=conservative), which shows each knob's RSS
# contribution and its latency/coverage cost without a combinatorial grid.
#
# Traces are knob-independent (captured in TBJIT_TRACE_ONLY with the default
# build), so we capture once, then rebuild only tbjit_bound_bench per
# compile-config and replay the cached traces.
#
# Output TSV (one row per config x workload x backend):
#   hist_cap seg_shift reap_mode backend workload p50_ns ci95_ns peak_rss_kb jit_served segments
#
# Usage: bound_sweep.sh [build-dir] [out-dir]
# Exits 0 on partial results; non-zero only on infrastructure failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"           # has libtbjit for capture
OUT_DIR="${2:-$REPO_ROOT/bench-out/memsweep}"
SWEEP_BUILD="$REPO_ROOT/build-sweep"         # scratch, reconfigured per config
PASSES="${BOUND_BENCH_PASSES:-15}"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"

CORPUS=(openssl_crypto perl_bench gcc_compile)

# Compile-config -> reap modes to measure under it. The baseline build (64,21)
# carries the whole reap axis; the other builds vary one compile knob and run
# conservative only. One line: "<cap> <shift> <reap...>".
CONFIGS=(
  "64 21 conservative eager madvise"   # baseline + reap axis
  "32 21 conservative"                 # CAP axis
  "16 21 conservative"
  "8  21 conservative"
  "64 20 conservative"                 # SEG_SHIFT axis
  "64 19 conservative"
  "64 18 conservative"
)

mkdir -p "$OUT_DIR"
TRACE_DIR="$OUT_DIR/traces"
LOG_DIR="$OUT_DIR/logs"
mkdir -p "$TRACE_DIR" "$LOG_DIR"
TSV="$OUT_DIR/sweep_results.tsv"
printf "hist_cap\tseg_shift\treap_mode\tbackend\tworkload\tp50_ns\tci95_ns\tpeak_rss_kb\tjit_served\tsegments\n" > "$TSV"

# ---- capture traces once (knob-independent) --------------------------------
declare -A TRACE_OF
for name in "${CORPUS[@]}"; do
  spec="$SCRIPT_DIR/workloads/${name}.sh"
  [[ -f "$spec" ]] || { echo "[skip $name] no spec" >&2; continue; }
  if ! "$SCRIPT_DIR/capture_trace.sh" "$spec" "$TRACE_DIR"; then
    echo "[skip $name] capture failed/precondition" >&2; continue
  fi
  shopt -s nullglob; traces=("$TRACE_DIR/${name}".*.trace); shopt -u nullglob
  [[ ${#traces[@]} -eq 0 ]] && { echo "[skip $name] no trace" >&2; continue; }
  TRACE_OF[$name]="$(ls -S "${traces[@]}" | head -1)"
  echo "[trace $name] ${TRACE_OF[$name]} ($(du -h "${TRACE_OF[$name]}" | cut -f1))"
done
[[ ${#TRACE_OF[@]} -gt 0 ]] || { echo "no traces captured; aborting" >&2; exit 1; }

# ---- glibc baseline (knob-independent reference) ----------------------------
GLIBC_BENCH="$BUILD_DIR/tools/tbjit_bound_bench"
if [[ -x "$GLIBC_BENCH" ]]; then
  for name in "${!TRACE_OF[@]}"; do
    row="$("$GLIBC_BENCH" "${TRACE_OF[$name]}" --backend glibc --label "$name" \
           --passes "$PASSES" 2>/dev/null | tail -1)" || continue
    IFS=$'\t' read -r _be _wl p50 ci95 rss _mf <<<"$row"
    printf "NA\tNA\tNA\tglibc\t%s\t%s\t%s\t%s\t1.000\tNA\n" \
      "$name" "$p50" "$ci95" "$rss" >> "$TSV"
  done
fi

# ---- sweep -----------------------------------------------------------------
run_one() {  # cap shift reap workload
  local cap=$1 shift=$2 reap=$3 wl=$4
  local trace="${TRACE_OF[$wl]}"
  local log="$LOG_DIR/c${cap}_s${shift}_${reap}_${wl}.log"
  local row js segs
  if ! row="$(TBJIT_REAP_MODE="$reap" "$SWEEP_BUILD/tools/tbjit_bound_bench" \
        "$trace" --backend bound --label "$wl" --passes "$PASSES" --profile \
        2>"$log" | tail -1)"; then
    echo "[fail c$cap s$shift $reap $wl] see $log" >&2; return
  fi
  IFS=$'\t' read -r _be _wl p50 ci95 rss _mf <<<"$row"
  js="$(grep -oE 'jit_served=[0-9.]+' "$log" | tail -1 | cut -d= -f2)"
  segs="$(grep -oE 'segments=[0-9]+' "$log" | tail -1 | cut -d= -f2)"
  printf "%s\t%s\t%s\tbound\t%s\t%s\t%s\t%s\t%s\t%s\n" \
    "$cap" "$shift" "$reap" "$wl" "$p50" "$ci95" "$rss" "${js:-NA}" "${segs:-NA}" \
    >> "$TSV"
}

for cfg in "${CONFIGS[@]}"; do
  read -r cap shift reaps <<<"$cfg"
  echo "============================================================"
  echo "[build] HIST_CAP=$cap SEG_SHIFT=$shift"
  cmake -S "$REPO_ROOT" -B "$SWEEP_BUILD" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-DTBJIT_HIST_CAP=$cap -DTBJIT_SEG_SHIFT=$shift" \
        >/dev/null
  cmake --build "$SWEEP_BUILD" --target tbjit_bound_bench --parallel "$JOBS" \
        >/dev/null
  for reap in $reaps; do
    for wl in "${!TRACE_OF[@]}"; do
      echo "[run] c$cap s$shift $reap $wl"
      run_one "$cap" "$shift" "$reap" "$wl"
    done
  done
done

# Traces are large and regenerable; drop them, keep the TSV + logs.
rm -f "$TRACE_DIR"/*.trace; rmdir "$TRACE_DIR" 2>/dev/null || true

echo "============================================================"
echo "Results: $TSV"
column -t -s$'\t' < "$TSV" || cat "$TSV"
