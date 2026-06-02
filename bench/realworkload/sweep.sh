#!/usr/bin/env bash
# Generalized single-knob sensitivity sweep: run the real-workload suite once
# per value of one env-configurable picker knob and collect per-value
# compile/blacklist/jit counts.
#
# Usage: sweep.sh <env-var> <out-subdir> <val1> <val2> ...
#   e.g. sweep.sh TBJIT_WINDOW_SIZE window 100 500 1000 2000 5000
#
# Output: bench-out/sweep/<out-subdir>/<val>/ per value, plus a combined
# bench-out/sweep/<out-subdir>/sweep-summary.tsv whose first column is `value`.
#
# The knob env var (TBJIT_WINDOW_SIZE / TBJIT_KS_ALPHA / TBJIT_STABLE_WINDOWS /
# TBJIT_DEOPT_LIMIT) is read by tbjit at init (src/analysis/analysis.cpp) and
# propagates down run_suite.sh -> run_workload.sh -> the workload via normal
# environment inheritance, so only the tbjit allocator run is affected.
#
# NOTE: analyze_dumps.py --format tsv emits a *long* (section/key/value) schema,
# not per-workload columns. We pivot the `workload:<name>` rows back into one
# row per workload here (pivot copied from sweep_stable_windows.sh).
set -euo pipefail

if [ "$#" -lt 3 ]; then
  echo "usage: $0 <env-var> <out-subdir> <val1> [val2 ...]" >&2
  exit 2
fi

ENV_VAR="$1"; shift
SUBDIR="$1"; shift
VALUES=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_ROOT="$REPO_ROOT/bench-out/sweep/$SUBDIR"
mkdir -p "$OUT_ROOT"
SUMMARY="$OUT_ROOT/sweep-summary.tsv"
printf 'value\tworkload\tsites\tcompiled\tblacklisted\tjit_pct\n' > "$SUMMARY"

# Pivot analyze_dumps.py --format tsv (section\tkey\tvalue long format) into
# one tab-separated row per workload. Verbatim from sweep_stable_windows.sh.
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

for V in "${VALUES[@]}"; do
  echo "=== $ENV_VAR=$V ==="
  OUT="$OUT_ROOT/$V"
  env "$ENV_VAR=$V" "$SCRIPT_DIR/run_suite.sh" "$OUT"
  python3 "$REPO_ROOT/tools/analyze_dumps.py" "$OUT/json" --format tsv \
    | pivot_per_workload \
    | sed "s/^/$V\t/" >> "$SUMMARY"
done

echo "Combined sweep summary ($ENV_VAR):"
column -t -s$'\t' < "$SUMMARY"
