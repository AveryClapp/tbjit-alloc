#!/usr/bin/env bash
# End-to-end (LD_PRELOAD) validation of the conditional-THP policy: does
# TBJIT_THP=auto cut the *deployment* max-RSS vs the default (always) on the real
# corpus? This is the realistic-deployment counterpart to bound_bench's
# allocator-level RSS -- it runs the whole program under libtbjit and measures
# process max-RSS with GNU time, so it includes the trampoline + learning + live
# set, not just the allocator.
#
# Usage: thp_e2e.sh [build-dir] [out-dir]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=bench/realworkload/lib.sh
source "$SCRIPT_DIR/lib.sh"
BUILD="${1:-$ROOT/build}"
OUT="${2:-$ROOT/bench-out/thp_e2e}"
mkdir -p "$OUT"

LIB="$(resolve_allocator tbjit)" || { echo "tbjit not built" >&2; exit 1; }
TIME=/usr/bin/time
command -v "$TIME" >/dev/null || { echo "GNU /usr/bin/time required" >&2; exit 1; }

TSV="$OUT/thp_e2e.tsv"
printf "config\tworkload\twall_s\tmax_rss_kb\n" > "$TSV"

CORPUS=(openssl_crypto perl_bench gcc_compile)
SETARCH=(); command -v setarch >/dev/null 2>&1 && SETARCH=(setarch -R)

run_cfg() {  # label  env_prefix   (env applies only to the workload, not bash)
  local label=$1 envp=$2 tf
  tf="$(mktemp)"
  "${SETARCH[@]}" "$TIME" -f '%e %M' -o "$tf" \
    bash -c "${envp}${CMD_STR}" >/dev/null 2>&1 || true
  local wall rss; read -r wall rss < "$tf" 2>/dev/null || true
  rm -f "$tf"
  printf "%s\t%s\t%s\t%s\n" "$label" "$WORKLOAD_NAME" "${wall:-NA}" "${rss:-NA}" >> "$TSV"
}

for name in "${CORPUS[@]}"; do
  spec="$SCRIPT_DIR/workloads/${name}.sh"
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
  echo "[run $WORKLOAD_NAME]"
  run_cfg glibc  ""
  run_cfg always "LD_PRELOAD=$LIB TBJIT_THP=always "
  run_cfg auto   "LD_PRELOAD=$LIB TBJIT_THP=auto "
  declare -F workload_teardown >/dev/null && workload_teardown
done

echo "==== END-TO-END (max RSS kB / wall s) ===="
column -t -s$'\t' < "$TSV"
# tbjit auto vs always RSS ratio per workload (the headline).
awk -F'\t' 'NR>1{r[$1"\t"$2]=$4}
END{
  for (k in r) { split(k,a,"\t"); wl[a[2]]=1 }
  print "---- auto/always max-RSS ratio ----"
  for (w in wl) {
    al=r["always\t"w]; au=r["auto\t"w]; gl=r["glibc\t"w]
    if (al+0>0 && au+0>0)
      printf "%s: always=%s auto=%s glibc=%s  auto/always=%.2f\n", w, al, au, gl, au/al
  }
}' "$TSV"
