#!/usr/bin/env bash
# Gate 1 (option-2 kill-shot): is there an RSS/THP operating point that a
# lifetime-aware policy could reach but jemalloc's own THP handling does not?
#
# Pre-registered decision rule (RSS half; TLB half needs bare-metal perf):
#   For each workload compute, over jemalloc thp:{never,always,default}:
#       bloat   = (rss_always  - rss_never) / rss_never   # is THP slack real here?
#       residue = (rss_default - rss_never) / rss_never   # does the heuristic leave it?
#   GO   (gap exists)  if on the synthetic AND >=1 real workload: bloat>0.15 and residue>0.15
#                      (always bloats, default does NOT recover it -> room for lifetime policy)
#   NOGO (heuristic wins) if residue<=0.10 on the real workloads
#                      (default already sits at never's RSS -> nothing to add)
# The synthetic carries the mechanism evidence (known live bytes -> exact slack);
# the real workloads carry external validity (process Max-RSS under LD_PRELOAD).
#
# Configs: glibc and mimalloc as references; jemalloc never/always/default as the
# axis under test. jemalloc decay/metadata knobs are pinned so only `thp` varies.
# TLB counters are attempted via perf and degrade to NA where the PMU is absent
# (GitHub VMs) -- that NA is itself the Gate 2 hardware signal.
#
# Usage: gate1.sh [out-dir]   (CI: Linux x86-64; allocators via LD_PRELOAD)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RWL="$SCRIPT_DIR/realworkload"
# shellcheck source=bench/realworkload/lib.sh
source "$RWL/lib.sh"

OUT="${1:-$ROOT/bench-out/gate1}"
mkdir -p "$OUT"
LOG="$OUT/logs"; mkdir -p "$LOG"

REPS_SYN="${GATE1_REPS_SYN:-5}"
REPS_APP="${GATE1_REPS_APP:-3}"
# jemalloc base config: hold decay + metadata-THP constant so only opt.thp moves.
JE_BASE="background_thread:true,dirty_decay_ms:10000,muzzy_decay_ms:0,metadata_thp:disabled"
SETARCH=(); command -v setarch >/dev/null 2>&1 && SETARCH=(setarch -R)
TIME=/usr/bin/time

# ---- system THP mode: record, best-effort set, record actual ---------------
THP_SYS=/sys/kernel/mm/transparent_hugepage/enabled
want_sys="${GATE1_THP_SYS:-always}"
if [[ -e "$THP_SYS" ]]; then
  if echo "$want_sys" | sudo tee "$THP_SYS" >/dev/null 2>&1; then :; fi
  sys_mode="$(grep -oE '\[[a-z]+\]' "$THP_SYS" | tr -d '[]')"
else
  sys_mode="none"
fi
echo "[gate1] system THP mode = ${sys_mode:-unknown} (requested $want_sys)" | tee "$LOG/env.txt"

# ---- resolve allocators ----------------------------------------------------
JE="$(resolve_allocator jemalloc || true)"
MI="$(resolve_allocator mimalloc || true)"
[[ -n "$JE" ]] || { echo "[gate1] FATAL: jemalloc not installed (apt: libjemalloc-dev)" >&2; exit 1; }
echo "[gate1] jemalloc = $JE" | tee -a "$LOG/env.txt"
echo "[gate1] mimalloc = ${MI:-<absent>}" | tee -a "$LOG/env.txt"

# Record what jemalloc actually does with opt.thp on this build (semantics vary
# by version: some apply MADV_HUGEPAGE to user extents, some only defer to sys).
for m in never always default; do
  MALLOC_CONF="${JE_BASE},thp:${m},stats_print:true" LD_PRELOAD="$JE" \
    true 2>"$LOG/je_opts_${m}.txt" || true
  v="$(grep -E 'opt\.thp' "$LOG/je_opts_${m}.txt" | head -1 || true)"
  echo "[gate1] jemalloc thp:${m} -> ${v:-<no stats>}" | tee -a "$LOG/env.txt"
done

# ---- config table: label -> (preload, extra-env) ---------------------------
CFG_LABELS=(glibc)
declare -A CFG_PRELOAD=([glibc]="") CFG_ENV=([glibc]="")
if [[ -n "$MI" ]]; then
  CFG_LABELS+=(mimalloc); CFG_PRELOAD[mimalloc]="$MI"; CFG_ENV[mimalloc]=""
fi
for m in never always default; do
  l="je_${m}"; CFG_LABELS+=("$l")
  CFG_PRELOAD[$l]="$JE"; CFG_ENV[$l]="MALLOC_CONF=${JE_BASE},thp:${m}"
done

run_with_cfg() {  # label cmd...  -> runs `cmd` under that allocator config
  local l="$1"; shift
  local pre=() ; [[ -n "${CFG_PRELOAD[$l]}" ]] && pre+=(LD_PRELOAD="${CFG_PRELOAD[$l]}")
  [[ -n "${CFG_ENV[$l]}" ]] && pre+=(${CFG_ENV[$l]})
  env "${pre[@]}" "$@"
}

# ---- build the synthetic probe ---------------------------------------------
PROBE="$OUT/gate1_probe"
cc -O2 -Wall -o "$PROBE" "$SCRIPT_DIR/gate1_lifetime_probe.c" \
  || { echo "[gate1] FATAL: probe build failed" >&2; exit 1; }

# ---- detect perf PMU availability (Gate 2 signal) --------------------------
PERF_OK=0
if command -v perf >/dev/null 2>&1 && \
   perf stat -x, -e cycles true >/dev/null 2>"$LOG/perf_probe.txt"; then
  PERF_OK=1
fi
echo "[gate1] perf hardware counters: $([[ $PERF_OK == 1 ]] && echo available || echo UNAVAILABLE)" \
  | tee -a "$LOG/env.txt"

# ===========================================================================
# Measurements
# ===========================================================================
TSV="$OUT/gate1.tsv"
printf "workload\tconfig\trep\tlive_kb\trss_kb\tanonhuge_kb\tslack_kb\twall_s\tdtlb_miss\n" > "$TSV"

# --- synthetic: probe self-reports live/rss/anonhuge/slack from smaps -------
echo "[gate1] === synthetic ground-truth probe ==="
for rep in $(seq 1 "$REPS_SYN"); do
  for l in "${CFG_LABELS[@]}"; do
    dtlb="NA"
    if [[ $PERF_OK == 1 ]]; then
      run_with_cfg "$l" perf stat -x, -e dTLB-load-misses -o "$LOG/perf_${l}_${rep}.txt" \
        "${SETARCH[@]}" "$PROBE" >"$LOG/probe_${l}_${rep}.out" 2>>"$LOG/probe_${l}_${rep}.err" || true
      dtlb="$(grep -E ',dTLB-load-misses' "$LOG/perf_${l}_${rep}.txt" 2>/dev/null | head -1 | cut -d, -f1)"
      [[ -n "$dtlb" ]] || dtlb="NA"
    else
      run_with_cfg "$l" "${SETARCH[@]}" "$PROBE" \
        >"$LOG/probe_${l}_${rep}.out" 2>>"$LOG/probe_${l}_${rep}.err" || true
    fi
    IFS=$'\t' read -r live rss anon thp slack < "$LOG/probe_${l}_${rep}.out" 2>/dev/null || continue
    printf "synthetic\t%s\t%s\t%s\t%s\t%s\t%s\tNA\t%s\n" \
      "$l" "$rep" "${live:-NA}" "${rss:-NA}" "${anon:-NA}" "${slack:-NA}" "$dtlb" >> "$TSV"
  done
done

# --- real workloads: process Max-RSS via /usr/bin/time ----------------------
run_app_cfg() {  # workload label
  local wl="$1" l="$2" tf wall rss
  tf="$(mktemp)"
  local pre=(); [[ -n "${CFG_PRELOAD[$l]}" ]] && pre+=("LD_PRELOAD=${CFG_PRELOAD[$l]}")
  [[ -n "${CFG_ENV[$l]}" ]] && pre+=("${CFG_ENV[$l]}")
  "${SETARCH[@]}" "$TIME" -f '%e %M' -o "$tf" \
    env "${pre[@]}" bash -c "$CMD_STR" >/dev/null 2>&1 || true
  read -r wall rss < "$tf" 2>/dev/null || true; rm -f "$tf"
  printf "%s\t%s\t%s\tNA\t%s\tNA\tNA\t%s\tNA\n" \
    "$wl" "$l" "$REP" "${rss:-NA}" "${wall:-NA}" >> "$TSV"
}

REAL=(openssl_crypto sqlite_inmem)
for name in "${REAL[@]}"; do
  spec="$RWL/workloads/${name}.sh"
  [[ -f "$spec" ]] || { echo "[skip $name] no spec" >&2; continue; }
  unset -f workload_preconditions workload_setup workload_cmd workload_teardown 2>/dev/null || true
  WORKLOAD_NAME=""
  # shellcheck source=/dev/null
  source "$spec"
  if declare -F workload_preconditions >/dev/null && ! workload_preconditions; then
    echo "[skip $name] precondition" >&2; continue
  fi
  declare -F workload_setup >/dev/null && workload_setup
  CMD_STR="$(workload_cmd)"
  echo "[gate1] === real workload: $WORKLOAD_NAME ==="
  for REP in $(seq 1 "$REPS_APP"); do
    for l in "${CFG_LABELS[@]}"; do run_app_cfg "$WORKLOAD_NAME" "$l"; done
  done
  declare -F workload_teardown >/dev/null && workload_teardown
done

# ===========================================================================
# Verdict: median per (workload,config), then the pre-registered rule
# ===========================================================================
VERDICT="$OUT/gate1_verdict.txt"
if command -v python3 >/dev/null 2>&1; then
  python3 - "$TSV" "$VERDICT" "$sys_mode" "$PERF_OK" <<'PY'
import sys, statistics, collections
tsv, out, sys_mode, perf_ok = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
rows = collections.defaultdict(lambda: collections.defaultdict(list))  # (wl)->cfg->[rss]
slack = collections.defaultdict(lambda: collections.defaultdict(list))
with open(tsv) as f:
    h = f.readline().rstrip("\n").split("\t"); i = {k: n for n, k in enumerate(h)}
    for ln in f:
        r = ln.rstrip("\n").split("\t")
        if len(r) < len(h): continue
        wl, cfg = r[i["workload"]], r[i["config"]]
        try: rows[wl][cfg].append(float(r[i["rss_kb"]]))
        except ValueError: pass
        try: slack[wl][cfg].append(float(r[i["slack_kb"]]))
        except ValueError: pass
def med(xs): return statistics.median(xs) if xs else float("nan")
L = []
L.append(f"system THP mode : {sys_mode}")
L.append(f"perf PMU        : {'available' if perf_ok=='1' else 'UNAVAILABLE (TLB half needs bare metal)'}")
L.append("")
L.append(f"{'workload':<12} {'never':>10} {'always':>10} {'default':>10} {'bloat':>7} {'residue':>8}")
go_syn = go_app = nogo_app = False
for wl in rows:
    nv, al, df = med(rows[wl].get("je_never",[])), med(rows[wl].get("je_always",[])), med(rows[wl].get("je_default",[]))
    if not (nv==nv and al==al and df==df and nv>0):
        L.append(f"{wl:<12}  (incomplete jemalloc triple)"); continue
    bloat   = (al-nv)/nv
    residue = (df-nv)/nv
    L.append(f"{wl:<12} {nv:>10.0f} {al:>10.0f} {df:>10.0f} {bloat:>6.0%} {residue:>7.0%}")
    gap = (bloat>0.15 and residue>0.15)
    if wl=="synthetic":
        go_syn = gap
        s_nv, s_al, s_df = med(slack[wl].get("je_never",[])), med(slack[wl].get("je_always",[])), med(slack[wl].get("je_default",[]))
        L.append(f"{'  slack_kb':<12} {s_nv:>10.0f} {s_al:>10.0f} {s_df:>10.0f}")
    else:
        if gap: go_app = True
        if residue <= 0.10: nogo_app = True
L.append("")
# reference rows (glibc / mimalloc) for context
L.append("reference Max-RSS (median kB):")
for wl in rows:
    g = med(rows[wl].get("glibc",[])); m = med(rows[wl].get("mimalloc",[]))
    L.append(f"  {wl:<12} glibc={g:.0f} mimalloc={m if m==m else float('nan'):.0f}")
L.append("")
if go_syn and go_app:
    verdict = "GO -- synthetic + >=1 real workload show jemalloc default leaving THP slack (>15%) that a lifetime-aware policy could shed. Proceed to bare-metal TLB half."
elif nogo_app and not go_app:
    verdict = "NO-GO -- jemalloc's heuristic already sits at never's RSS on the real workloads. Fold back to option 1."
else:
    verdict = "INCONCLUSIVE -- synthetic and real signals disagree, or THP slack not reproduced (check system THP mode / jemalloc opt.thp semantics in env.txt)."
L.append("VERDICT (RSS half): " + verdict)
open(out,"w").write("\n".join(L)+"\n")
print("\n".join(L))
PY
else
  echo "python3 unavailable; raw TSV at $TSV" | tee "$VERDICT"
fi

echo "[gate1] done. tsv=$TSV verdict=$VERDICT"
