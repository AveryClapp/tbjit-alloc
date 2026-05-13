#!/usr/bin/env bash
# Runs all four bench patterns: first standalone (glibc baseline), then
# under LD_PRELOAD with libtbjit for each forced strategy that fits the
# pattern. Output is a flat table so it's easy to screenshot for the
# writeup.
#
# Usage: bench/run_bench.sh [build-dir]
# Default build-dir: build

set -euo pipefail

BUILD="${1:-build}"

if [[ ! -d "$BUILD" ]]; then
  echo "build dir not found: $BUILD" >&2
  exit 1
fi

LIB="$BUILD/libtbjit.so"
if [[ "$(uname)" == "Darwin" ]]; then
  LIB="$BUILD/libtbjit.dylib"
fi

if [[ ! -f "$LIB" ]]; then
  echo "tbjit library not built at $LIB" >&2
  echo "run: cmake --build $BUILD" >&2
  exit 1
fi

PATTERNS=(monomorphic arena mixed pulse)

# Strategies to try per pattern. "" means run baseline (no LD_PRELOAD).
declare -A STRATEGIES
STRATEGIES[monomorphic]="baseline bump arena freelist"
STRATEGIES[arena]="baseline bump arena freelist"
STRATEGIES[mixed]="baseline freelist"
STRATEGIES[pulse]="baseline bump arena freelist"

printf '\n%-14s %-10s %-10s %-12s\n' "pattern" "strategy" "phase" "ns/op"
printf '%-14s %-10s %-10s %-12s\n'  "-------" "--------" "-----" "-----"

run_one() {
  local pattern="$1" strategy="$2"
  local bin="$BUILD/bench/bench_${pattern}"
  if [[ ! -x "$bin" ]]; then
    echo "skip ${pattern}: binary missing" >&2
    return
  fi

  local out
  if [[ "$strategy" == "baseline" ]]; then
    out=$("$bin")
  else
    out=$(LD_PRELOAD="$LIB" TBJIT_FORCE_STRATEGY="$strategy" "$bin")
  fi

  while IFS= read -r line; do
    # "warmup:   123.4 ns/op  (...)" → printable row
    local phase ns
    phase=$(awk '{print $1}' <<<"$line" | tr -d ':')
    ns=$(awk '{print $2}' <<<"$line")
    [[ -n "$ns" ]] && printf '%-14s %-10s %-10s %-12s\n' "$pattern" "$strategy" "$phase" "$ns"
  done <<<"$out"
}

for p in "${PATTERNS[@]}"; do
  for s in ${STRATEGIES[$p]}; do
    run_one "$p" "$s"
  done
  echo
done
