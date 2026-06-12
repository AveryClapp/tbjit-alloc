#!/usr/bin/env bash
# THP isolation: at fixed SEGMENT_SIZE (2 MiB, default build), is the segment RSS
# slack caused by transparent hugepages faulting whole 2 MiB regions for
# partially-filled segments, or by raw segment size? Compare TBJIT_THP on vs
# never on two axes:
#   RSS      — bound_bench over the replay traces: does no-THP cut RSS the way a
#              smaller segment did?
#   latency  — hold/monomorphic microbenches: does no-THP regress the TLB-bound
#              hold pattern the way a smaller segment did?
# If both hold, THP is the slack source and a lifetime-conditional THP policy
# (Hold sites keep THP, others drop it) gets the RSS win without the hold cost.
# THP is a runtime knob, so no rebuilds: one default build serves every cell.
#
# Usage: thp_isolation.sh [build-dir] [out-dir]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${1:-$ROOT/build}"
OUT="${2:-$ROOT/bench-out/thp}"
PASSES="${BOUND_BENCH_PASSES:-15}"
RWL="$SCRIPT_DIR/realworkload"
BENCH="$BUILD/tools/tbjit_bound_bench"
LIB="$BUILD/libtbjit.so"
mkdir -p "$OUT"
TRACE_DIR="$OUT/traces"; LOG="$OUT/logs"; mkdir -p "$TRACE_DIR" "$LOG"

CORPUS=(openssl_crypto perl_bench gcc_compile)
SETARCH=(); command -v setarch >/dev/null 2>&1 && SETARCH=(setarch -R)

# ---------- RSS: capture traces once, replay with THP on/off ----------
RSS="$OUT/thp_rss.tsv"
printf "thp\tbackend\tworkload\tp50_ns\tpeak_rss_kb\tjit_served\n" > "$RSS"

declare -A TRACE_OF
for name in "${CORPUS[@]}"; do
  spec="$RWL/workloads/${name}.sh"
  [[ -f "$spec" ]] || { echo "[skip $name] no spec" >&2; continue; }
  "$RWL/capture_trace.sh" "$spec" "$TRACE_DIR" || { echo "[skip $name] capture" >&2; continue; }
  shopt -s nullglob; t=("$TRACE_DIR/${name}".*.trace); shopt -u nullglob
  [[ ${#t[@]} -gt 0 ]] && TRACE_OF[$name]="$(ls -S "${t[@]}" | head -1)"
done

for name in "${!TRACE_OF[@]}"; do
  row="$("$BENCH" "${TRACE_OF[$name]}" --backend glibc --label "$name" \
         --passes "$PASSES" 2>/dev/null | tail -1)" || true
  IFS=$'\t' read -r _b _w p50 _c rss _m <<<"$row"
  printf "NA\tglibc\t%s\t%s\t%s\t1.000\n" "$name" "$p50" "$rss" >> "$RSS"
  for thp in always never auto; do
    lg="$LOG/rss_${name}_${thp}.log"
    pre=(TBJIT_THP="$thp")  # explicit: the library default is now "auto"
    row="$(env "${pre[@]}" "$BENCH" "${TRACE_OF[$name]}" --backend bound \
           --label "$name" --passes "$PASSES" --profile 2>"$lg" | tail -1)" || {
      echo "[fail rss $name $thp] see $lg" >&2; continue; }
    IFS=$'\t' read -r _b _w p50 _c rss _m <<<"$row"
    js="$(grep -oE 'jit_served=[0-9.]+' "$lg" | tail -1 | cut -d= -f2)"
    printf "%s\tbound\t%s\t%s\t%s\t%s\n" "$thp" "$name" "$p50" "$rss" "${js:-NA}" >> "$RSS"
  done
done
rm -f "$TRACE_DIR"/*.trace; rmdir "$TRACE_DIR" 2>/dev/null || true

# ---------- latency: hold/monomorphic with THP on/off ----------
# Reps are interleaved (glibc/always/never within each rep) so any runner drift
# hits all configs equally -- defeats the time-ordering confound that made the
# earlier segshift trend suspect. Aggregate with median offline.
LAT="$OUT/thp_lat.tsv"
printf "thp\tbench\trep\tphase\tns_per_op\n" > "$LAT"
emit_lat() { local thp=$1 b=$2 rep=$3
  while read -r ph ns _; do
    [[ -n "$ns" ]] && printf "%s\t%s\t%s\t%s\t%s\n" "$thp" "$b" "$rep" "${ph%:}" "$ns" >> "$LAT"
  done
}
REPS="${THP_LAT_REPS:-5}"
for rep in $(seq 1 "$REPS"); do
  for b in hold monomorphic; do
    bin="$BUILD/bench/bench_${b}"
    [[ -x "$bin" ]] || continue
    "${SETARCH[@]}" "$bin" | emit_lat glibc "$b" "$rep"
    for thp in always never auto; do
      pre=(LD_PRELOAD="$LIB" TBJIT_THP="$thp")
      "${SETARCH[@]}" env "${pre[@]}" "$bin" | emit_lat "$thp" "$b" "$rep"
    done
  done
done

# Median/min/max per (thp,bench) for the steady phase, to expose CI noise.
SUM="$OUT/thp_lat_summary.tsv"
if command -v python3 >/dev/null 2>&1; then
  python3 - "$LAT" "$SUM" <<'PY'
import sys, statistics, collections
lat, out = sys.argv[1], sys.argv[2]
g = collections.defaultdict(list)
with open(lat) as f:
    h = f.readline().rstrip("\n").split("\t"); i = {k: n for n, k in enumerate(h)}
    for ln in f:
        r = ln.rstrip("\n").split("\t")
        if len(r) < len(h) or r[i["phase"]] != "steady":
            continue
        g[(r[i["thp"]], r[i["bench"]])].append(float(r[i["ns_per_op"]]))
with open(out, "w") as w:
    w.write("thp\tbench\tmedian_ns\tmin_ns\tmax_ns\tn\n")
    for (thp, b), v in sorted(g.items()):
        w.write(f"{thp}\t{b}\t{statistics.median(v):.1f}\t{min(v):.1f}\t{max(v):.1f}\t{len(v)}\n")
PY
fi

echo "==== RSS (kB) ===="; column -t -s$'\t' < "$RSS"
echo "==== LATENCY median steady (ns/op) ===="; [[ -f "$SUM" ]] && column -t -s$'\t' < "$SUM"
