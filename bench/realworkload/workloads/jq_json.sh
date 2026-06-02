# Data-processing workload: jq over a large generated JSON document. Parsing a
# big array of objects and running a group/aggregate filter produces a flood of
# small, short-lived string/object allocations -- a parse-and-transform pattern
# closer to the database workload than the interpreter loops, but JSON-shaped.

WORKLOAD_NAME="jq_json"
WORKLOAD_DESC="jq group/aggregate over a ~200k-object JSON array"

_JQ_TMP=""

workload_preconditions() {
  command -v jq >/dev/null 2>&1 || { echo "jq missing" >&2; return 1; }
  return 0
}

workload_setup() {
  _JQ_TMP=$(mktemp -d -t tbjit-jq-XXXXXX)
  export _JQ_TMP
  # One JSON object per line, then slurp into an array. ~200k records.
  awk 'BEGIN {
    print "[";
    for (i = 0; i < 200000; i++) {
      sep = (i == 199999) ? "" : ",";
      printf "{\"k\":\"key_%d\",\"v\":%d,\"w\":%.3f}%s\n", i % 1024, i, i * 0.5, sep;
    }
    print "]";
  }' > "$_JQ_TMP/data.json"
}

workload_cmd() {
  # Group by key and reduce to per-key sums: forces jq to materialize many
  # intermediate objects/strings.
  echo "jq -c 'group_by(.k) | map({k: .[0].k, n: length, sum: (map(.v) | add)})' '$_JQ_TMP/data.json' >/dev/null"
}

workload_teardown() {
  [[ -n "$_JQ_TMP" && -d "$_JQ_TMP" ]] && rm -rf "$_JQ_TMP"
}
