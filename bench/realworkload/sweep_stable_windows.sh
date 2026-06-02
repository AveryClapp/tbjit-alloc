#!/usr/bin/env bash
# Run the real-workload suite once per TBJIT_STABLE_WINDOWS value and collect
# per-value compile/blacklist counts. Output: bench-out/sweep/<N>/ per value
# plus a combined sweep-summary.tsv.
#
# Relies on TBJIT_STABLE_WINDOWS being read by tbjit at init (see
# src/analysis/analysis.cpp). The env var propagates down through
# run_suite.sh -> run_workload.sh -> the workload process via normal
# environment inheritance, so only the tbjit allocator run is affected.
#
# NOTE: analyze_dumps.py --format tsv emits a *long* (section/key/value)
# schema, not per-workload columns. We pivot the `workload:<name>` rows
# back into one row per workload here.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_ROOT="${1:-$REPO_ROOT/bench-out/sweep}"
mkdir -p "$OUT_ROOT"
SUMMARY="$OUT_ROOT/sweep-summary.tsv"
printf 'stable_windows\tworkload\tsites\tcompiled\tblacklisted\tjit_pct\n' > "$SUMMARY"

# Pivot analyze_dumps.py --format tsv (section\tkey\tvalue long format) into
# one tab-separated row per workload: workload sites compiled blacklisted jit_pct
pivot_per_workload() {
  awk -F'\t' '
    $1 ~ /^workload:/ {
      wl = substr($1, 10)           # strip "workload:" (9 chars)
      if (!(wl in seen)) { order[++n] = wl; seen[wl] = 1 }
      val[wl, $2] = $3
    }
    END {
      for (i = 1; i <= n; i++) {
        wl = order[i]
        printf "%s\t%s\t%s\t%s\t%s\n", wl,
          val[wl,"sites"], val[wl,"compiled"],
          val[wl,"blacklisted"], val[wl,"jit_pct"]
      }
    }'
}

for N in 3 5 10; do
  echo "=== STABLE_WINDOWS=$N ==="
  OUT="$OUT_ROOT/$N"
  TBJIT_STABLE_WINDOWS="$N" "$SCRIPT_DIR/run_suite.sh" "$OUT"
  python3 "$REPO_ROOT/tools/analyze_dumps.py" "$OUT/json" --format tsv \
    | pivot_per_workload \
    | sed "s/^/$N\t/" >> "$SUMMARY"
done

echo "Combined sweep summary:"
column -t -s$'\t' < "$SUMMARY"
