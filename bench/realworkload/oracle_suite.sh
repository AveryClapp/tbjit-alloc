#!/usr/bin/env bash
# Per-workload replay-oracle headroom (phase3-results §6 follow-up).
#
# For each workload spec under workloads/: run it once under LD_PRELOAD=libtbjit
# with the default online picker (JSON dump -> online JIT-served fraction), then
# capture a complete unspecialized trace (capture_trace.sh) and replay the
# dominant process's trace through `tbjit_replay --oracle` (never-deopt/never-
# blacklist picker) for the rigorous capturable upper bound. Both halves run in
# the same job so the online/oracle pairing is within-run.
#
# Output: <out-dir>/oracle_results.tsv
#   workload  online_jit_pct  online_total_allocs  oracle_pct  oracle_captured  oracle_total
#
# Traces are large and regenerable; each is deleted right after its replay.
#
# Usage: oracle_suite.sh [build-dir] [out-dir]
# Exits 0 on partial results (skipped workloads are not fatal); non-zero only
# on infrastructure failure (no replay binary, no specs).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=bench/realworkload/lib.sh
source "$SCRIPT_DIR/lib.sh"

BUILD="${1:-$BUILD_DIR}"
OUT="${2:-$REPO_ROOT/bench-out/oracle}"
SPECS_DIR="$SCRIPT_DIR/workloads"
JSON_DIR="$OUT/json"
TRACE_DIR="$OUT/traces"
LOG_DIR="$OUT/logs"
mkdir -p "$JSON_DIR" "$TRACE_DIR" "$LOG_DIR"

REPLAY="$BUILD/tools/tbjit_replay"
[[ -x "$REPLAY" ]] || { echo "tbjit_replay not built: $REPLAY" >&2; exit 1; }
LIB="$(resolve_allocator tbjit)" || { echo "tbjit not built" >&2; exit 1; }

SETARCH=(); command -v setarch >/dev/null 2>&1 && SETARCH=(setarch -R)
TIMEOUT=(); command -v timeout >/dev/null 2>&1 \
  && TIMEOUT=(timeout --foreground --kill-after=5s "${WORKLOAD_TIMEOUT_S:-180}s")

# oracle replay results keyed by workload: "captured total pct"
declare -A ORACLE_OF

shopt -s nullglob
SPECS=()
for s in "$SPECS_DIR"/*.sh; do
  [[ "$(basename "$s")" == _* ]] && continue
  SPECS+=("$s")
done
shopt -u nullglob
[[ ${#SPECS[@]} -gt 0 ]] || { echo "no workload specs under $SPECS_DIR" >&2; exit 1; }

for spec in "${SPECS[@]}"; do
  echo "============================================================"
  WORKLOAD_NAME=""
  unset -f workload_preconditions workload_setup workload_cmd workload_teardown 2>/dev/null || true
  # shellcheck source=/dev/null
  source "$spec"
  name="$WORKLOAD_NAME"
  [[ -n "$name" ]] || { echo "spec did not set WORKLOAD_NAME: $spec" >&2; continue; }

  if declare -F workload_preconditions >/dev/null && ! workload_preconditions; then
    echo "[skip $name] precondition" >&2
    continue
  fi

  # ---- online half: default picker, JSON dump -> online jit% ----
  declare -F workload_setup >/dev/null && workload_setup
  CMD_STR="$(workload_cmd)"
  echo "[online $name] $CMD_STR"
  env_prefix="LD_PRELOAD=$LIB TBJIT_DUMP=1 TBJIT_DUMP_JSON=$JSON_DIR/$name.%p.json "
  "${SETARCH[@]}" "${TIMEOUT[@]}" bash -c "${env_prefix}${CMD_STR}" \
    > /dev/null 2> "$LOG_DIR/$name.online.log" \
    || echo "[online $name] exited $? (dump may still be usable)" >&2
  declare -F workload_teardown >/dev/null && workload_teardown

  # ---- oracle half: full trace capture + never-deopt replay ----
  if ! "$SCRIPT_DIR/capture_trace.sh" "$spec" "$TRACE_DIR" 2>> "$LOG_DIR/$name.capture.log"; then
    rc=$?
    [[ $rc -eq 2 ]] && echo "[skip $name] capture precondition" >&2 \
                    || echo "[skip $name] capture failed (rc=$rc)" >&2
    continue
  fi
  shopt -s nullglob
  traces=("$TRACE_DIR/${name}".*.trace)
  shopt -u nullglob
  if [[ ${#traces[@]} -eq 0 ]]; then
    echo "[skip $name] no trace produced" >&2
    continue
  fi
  trace=$(ls -S "${traces[@]}" | head -1)
  echo "[oracle $name] replay $(basename "$trace") ($(du -h "$trace" | cut -f1))"
  olog="$LOG_DIR/$name.oracle.log"
  "$REPLAY" "$trace" --oracle > /dev/null 2> "$olog" \
    || echo "[oracle $name] replay failed (see $olog)" >&2
  rm -f "${traces[@]}"

  # stderr line: oracle: capturable C / T events = P% (upper bound)
  line=$(grep -E '^oracle: capturable' "$olog" | tail -1 || true)
  if [[ -n "$line" ]]; then
    read -r captured total pct <<< "$(awk '{gsub(/%/,"",$8); print $3, $5, $8}' <<< "$line")"
    ORACLE_OF[$name]="$captured $total $pct"
  else
    echo "[oracle $name] no capturable line in replay output" >&2
  fi
done

rmdir "$TRACE_DIR" 2>/dev/null || true

# ---- join: online jit% (analyze_dumps long tsv) + oracle bound ----
ONLINE_TSV="$OUT/online_aggregate.tsv"
if compgen -G "$JSON_DIR/*.json" > /dev/null; then
  python3 "$REPO_ROOT/tools/analyze_dumps.py" "$JSON_DIR" --format tsv > "$ONLINE_TSV"
else
  echo "no JSON dumps produced" >&2
  : > "$ONLINE_TSV"
fi

TSV="$OUT/oracle_results.tsv"
printf "workload\tonline_jit_pct\tonline_total_allocs\toracle_pct\toracle_captured\toracle_total\n" > "$TSV"
for name in $(printf '%s\n' "${!ORACLE_OF[@]}" | sort); do
  read -r captured total pct <<< "${ORACLE_OF[$name]}"
  online_pct=$(awk -F'\t' -v w="workload:$name" '$1==w && $2=="jit_pct" {print $3}' "$ONLINE_TSV")
  online_total=$(awk -F'\t' -v w="workload:$name" '$1==w && $2=="total_allocs" {print $3}' "$ONLINE_TSV")
  printf "%s\t%s\t%s\t%s\t%s\t%s\n" \
    "$name" "${online_pct:-NA}" "${online_total:-NA}" "$pct" "$captured" "$total" >> "$TSV"
done

echo "============================================================"
echo "Results: $TSV"
column -t -s$'\t' < "$TSV" || cat "$TSV"

# Sanity: the oracle is an upper bound; flag any workload where online > oracle
# (would indicate a capture/replay mismatch, e.g. different dominant process).
awk -F'\t' 'NR>1 && $2!="NA" && $2+0 > $4+0 + 0.5 {
  printf "WARNING: %s online %s%% > oracle %s%% (capture/replay mismatch?)\n", $1,$2,$4 > "/dev/stderr"
}' "$TSV"
