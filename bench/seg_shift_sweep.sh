#!/usr/bin/env bash
# Latency cost of shrinking SEGMENT_SIZE on the TLB-sensitive alloc-churn
# microbenches (hold, monomorphic). The memory sweep showed smaller segments cut
# allocator RSS 50-75% for free on replay traces; this checks whether the
# THP/hugepage TLB benefit -- the original 2 MiB rationale (the mimalloc `hold`
# gap) -- makes them slower on the workload that motivated it.
#
# The bench binaries are shift-independent (they call malloc, routed via
# LD_PRELOAD), so we rebuild only libtbjit per shift and re-preload.
#
# Usage: seg_shift_sweep.sh [out-dir]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/bench-out/segshift}"
mkdir -p "$OUT"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"

SHIFTS=(21 20 19 18)         # 2 MiB (default) down to 256 KiB
BENCHES=(hold monomorphic)
STRATS=(auto bump)

# setarch -R (ASLR off) for stable site ids across the runs; degrade gracefully.
SETARCH=()
command -v setarch >/dev/null 2>&1 && SETARCH=(setarch -R)

TSV="$OUT/segshift_hold.tsv"
printf "seg_shift\tbench\tstrategy\tphase\tns_per_op\n" > "$TSV"

emit() {  # shift bench strategy   (reads bench stdout: "phase:  N ns/op (...)")
    local sh=$1 b=$2 s=$3
    while read -r ph ns _; do
        [[ -n "$ns" ]] && printf "%s\t%s\t%s\t%s\t%s\n" "$sh" "$b" "$s" "${ph%:}" "$ns" >> "$TSV"
    done
}

# glibc baseline (shift-independent), once.
for b in "${BENCHES[@]}"; do
    bin="$ROOT/build/bench/bench_${b}"
    [[ -x "$bin" ]] || { echo "skip $b: no binary" >&2; continue; }
    "${SETARCH[@]}" "$bin" | emit NA "$b" glibc
done

SB="$ROOT/build-segshift"
for sh in "${SHIFTS[@]}"; do
    echo "[build] SEG_SHIFT=$sh"
    cmake -S "$ROOT" -B "$SB" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_FLAGS="-DTBJIT_SEG_SHIFT=$sh" >/dev/null
    cmake --build "$SB" --target tbjit --parallel "$JOBS" >/dev/null
    LIB="$SB/libtbjit.so"
    for b in "${BENCHES[@]}"; do
        bin="$ROOT/build/bench/bench_${b}"
        [[ -x "$bin" ]] || continue
        for s in "${STRATS[@]}"; do
            pre=(LD_PRELOAD="$LIB")
            [[ "$s" == bump ]] && pre+=(TBJIT_FORCE_STRATEGY=bump)
            echo "[run] s$sh $b $s"
            "${SETARCH[@]}" env "${pre[@]}" "$bin" | emit "$sh" "$b" "$s"
        done
    done
done

echo "============================================================"
echo "Results: $TSV"
column -t -s$'\t' < "$TSV"
