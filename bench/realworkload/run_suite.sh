#!/usr/bin/env bash
# Drive every workload spec under bench/realworkload/workloads/ through
# run_workload.sh, then aggregate the tbjit JSON dumps with
# tools/analyze_dumps.py. Designed for the weekly Phase-1 GitHub
# workflow but identical when run locally.
#
# Usage:
#   bench/realworkload/run_suite.sh [out-dir]
#
# Exits 0 even if individual workloads fail to skip on missing deps —
# the manifest captures partial results and the aggregate step is fine
# with sparse JSON. The script returns non-zero only on infrastructure
# failure (no build, no specs, analyzer crash).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=bench/realworkload/lib.sh
source "$SCRIPT_DIR/lib.sh"

OUT_DIR=$(init_out_dir "${1:-}")
SPECS_DIR="$SCRIPT_DIR/workloads"
JSON_DIR="$OUT_DIR/json"
mkdir -p "$JSON_DIR"

# Centralized JSON dir: each workload writes its tbjit dump into
# $W_DIR/json/<name>.json under run_workload.sh. We copy/symlink into a
# flat directory here so analyze_dumps.py sees one workload per file
# without having to crawl the per-workload subdirs.
collect_json_dumps() {
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    cp -f "$f" "$JSON_DIR/"
  done < <(find "$OUT_DIR" -mindepth 3 -maxdepth 3 -name '*.json' -path '*/json/*')
}

if [[ ! -d "$SPECS_DIR" ]]; then
  echo "no workload specs dir: $SPECS_DIR" >&2
  exit 1
fi

shopt -s nullglob
# Files starting with _ are shared helpers sourced by specs (e.g.
# _cxx_fixture.sh) — never treat them as workloads themselves.
SPECS=()
for s in "$SPECS_DIR"/*.sh; do
  base=$(basename "$s")
  [[ "$base" == _* ]] && continue
  SPECS+=("$s")
done
shopt -u nullglob

if [[ ${#SPECS[@]} -eq 0 ]]; then
  echo "no workload specs found under $SPECS_DIR" >&2
  exit 1
fi

echo "## real-workload suite: ${#SPECS[@]} specs, out=$OUT_DIR"
echo

for spec in "${SPECS[@]}"; do
  echo "------------------------------------------------------------"
  if ! "$SCRIPT_DIR/run_workload.sh" "$spec" "$OUT_DIR"; then
    rc=$?
    if [[ $rc -eq 2 ]]; then
      echo "(skipped: precondition)"
    else
      echo "(workload failed with rc=$rc — continuing)"
    fi
  fi
  echo
done

collect_json_dumps

echo "============================================================"
echo "Manifest:"
column -t -s$'\t' < "$OUT_DIR/manifest.tsv" || cat "$OUT_DIR/manifest.tsv"
echo

if compgen -G "$JSON_DIR/*.json" > /dev/null; then
  echo "Picker aggregate (cross-workload):"
  python3 "$REPO_ROOT/tools/analyze_dumps.py" "$JSON_DIR" --format text \
    | tee "$OUT_DIR/aggregate.txt"
  python3 "$REPO_ROOT/tools/analyze_dumps.py" "$JSON_DIR" --format md \
    > "$OUT_DIR/aggregate.md"
  python3 "$REPO_ROOT/tools/analyze_dumps.py" "$JSON_DIR" --format tsv \
    > "$OUT_DIR/aggregate.tsv"
else
  echo "no tbjit JSON dumps produced; skipping aggregate"
fi
