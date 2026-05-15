#!/usr/bin/env bash
# Compare tbjit auto vs glibc baseline vs mimalloc vs jemalloc on every
# bench pattern. Prints one row per pattern: pattern, glibc ns/op,
# mimalloc ns/op, jemalloc ns/op, tbjit ns/op. Each cell is the median
# of REPS runs (default 3) to smooth CI VM noise. Reports steady-state
# (or warmup if a pattern has no steady measurement).
#
# Usage: bench/run_alloc_comparison.sh [build-dir]
# Env:   REPS=N  (default 3)  — number of runs per cell; median reported.
#
# If mimalloc/jemalloc aren't installed (ldconfig finds nothing), that
# column shows "-" and the row continues. Useful for paper-quality
# comparisons against state-of-the-art allocators in the bench
# workflow without making the local dev loop depend on them.

set -euo pipefail

BUILD="${1:-build}"
REPS="${REPS:-3}"

if [[ ! -d "$BUILD" ]]; then
  echo "build dir not found: $BUILD" >&2
  exit 1
fi

LIB_TBJIT="$BUILD/libtbjit.so"
if [[ "$(uname)" == "Darwin" ]]; then
  LIB_TBJIT="$BUILD/libtbjit.dylib"
fi
if [[ ! -f "$LIB_TBJIT" ]]; then
  echo "tbjit library not built at $LIB_TBJIT" >&2
  exit 1
fi

# Discover allocator shared objects. ldconfig -p lists all libraries on
# the loader path; grep for known names. Empty if not installed.
MIMALLOC=""
JEMALLOC=""
if command -v ldconfig >/dev/null 2>&1; then
  MIMALLOC=$(ldconfig -p 2>/dev/null | grep -E 'libmimalloc\.so(\.[0-9]+)+$' | head -1 | awk '{print $NF}' || true)
  JEMALLOC=$(ldconfig -p 2>/dev/null | grep -E 'libjemalloc\.so(\.[0-9]+)+$' | head -1 | awk '{print $NF}' || true)
fi

PATTERNS=(monomorphic arena mixed pulse bimodal lifo producer_consumer hold)

extract_steady() {
  local out="$1"
  local ns
  ns=$(awk '/^steady:/ {print $2; exit}' <<< "$out")
  [[ -z "$ns" ]] && ns=$(awk '/^warmup:/ {print $2; exit}' <<< "$out")
  [[ -z "$ns" ]] && ns="-"
  echo "$ns"
}

# Run an LD_PRELOAD'd bench REPS times and print the median steady ns/op.
# Args: $1=lib (or empty for no preload), $2=binary
median_steady() {
  local lib="$1" bin="$2"
  local samples=()
  for ((r = 0; r < REPS; ++r)); do
    local out v
    if [[ -z "$lib" ]]; then
      out=$("$bin")
    else
      out=$(LD_PRELOAD="$lib" "$bin")
    fi
    v=$(extract_steady "$out")
    [[ "$v" == "-" ]] && { echo "-"; return; }
    samples+=("$v")
  done
  printf '%s\n' "${samples[@]}" | sort -n | \
    awk -v n="$REPS" 'NR == int((n + 1) / 2)'
}

printf '\n%-20s %12s %12s %12s %12s   (median of %d runs)\n' \
  "pattern" "glibc" "mimalloc" "jemalloc" "tbjit_auto" "$REPS"
printf '%-20s %12s %12s %12s %12s\n' \
  "-------" "-----" "--------" "--------" "----------"

[[ -n "$MIMALLOC" ]] && echo "# mimalloc: $MIMALLOC" >&2
[[ -n "$JEMALLOC" ]] && echo "# jemalloc: $JEMALLOC" >&2

for p in "${PATTERNS[@]}"; do
  bin="$BUILD/bench/bench_${p}"
  [[ -x "$bin" ]] || continue

  glibc_ns=$(median_steady "" "$bin")
  if [[ -n "$MIMALLOC" ]]; then
    mim_ns=$(median_steady "$MIMALLOC" "$bin")
  else
    mim_ns="-"
  fi
  if [[ -n "$JEMALLOC" ]]; then
    jem_ns=$(median_steady "$JEMALLOC" "$bin")
  else
    jem_ns="-"
  fi
  tbjit_ns=$(median_steady "$LIB_TBJIT" "$bin")

  printf '%-20s %12s %12s %12s %12s\n' \
    "$p" "$glibc_ns" "$mim_ns" "$jem_ns" "$tbjit_ns"
done
