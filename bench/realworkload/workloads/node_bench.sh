# Scripting-runtime workload: Node.js (V8). A long object/string-churn loop
# forces V8's heap to grow and collect repeatedly. V8 backs its spaces with
# malloc-family calls, so this is a GC-driven allocation pattern distinct from
# the compiler (bursty, parse-tree) and database (B-tree node) workloads.

WORKLOAD_NAME="node_bench"
WORKLOAD_DESC="node/V8 object + string churn (GC-driven heap growth)"

_NODE_TMP=""

workload_preconditions() {
  command -v node >/dev/null 2>&1 || { echo "node missing" >&2; return 1; }
  return 0
}

workload_setup() {
  _NODE_TMP=$(mktemp -d -t tbjit-node-XXXXXX)
  export _NODE_TMP
  cat > "$_NODE_TMP/work.js" <<'JS'
let sink = 0;
for (let round = 0; round < 300; round++) {
  const a = [];
  for (let i = 0; i < 50000; i++) {
    a.push({ k: "key_" + (i % 1024), v: i, tags: [i, i + 1, i + 2] });
  }
  for (const o of a) sink += o.v + o.tags.length;
  // Drop references each round so the next round triggers collection.
}
process.stdout.write(String(sink) + "\n");
JS
}

workload_cmd() {
  echo "node '$_NODE_TMP/work.js' >/dev/null"
}

workload_teardown() {
  [[ -n "$_NODE_TMP" && -d "$_NODE_TMP" ]] && rm -rf "$_NODE_TMP"
}
