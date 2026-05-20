# Scripting-language workload: CPython running an allocation-heavy
# script. CPython's object model produces enormous numbers of small,
# short-lived allocations (every int boxing, every dict probe, every
# string concat) — a stress test for the picker's ability to handle
# polymorphic-size churn without thrashing.
#
# Script is inlined as a heredoc so no fixture file is needed at rest.

WORKLOAD_NAME="python_alloc"
WORKLOAD_DESC="CPython running dict/list/json churn (small-object stress)"

_PY_TMP=""

workload_preconditions() {
  command -v python3 >/dev/null 2>&1 || { echo "python3 missing" >&2; return 1; }
  return 0
}

workload_setup() {
  _PY_TMP=$(mktemp -d -t tbjit-py-XXXXXX)
  export _PY_TMP
  cat > "$_PY_TMP/script.py" <<'PY'
# Three loops, each targeting a distinct allocation pattern:
#   1. dict insertion + lookup (hashed small-string keys)
#   2. list slicing + concat (transient buffers)
#   3. json round-trip (parser allocations + dict reconstruction)
import json

ITER = 60_000

def dict_churn():
    d = {}
    for i in range(ITER):
        d["k" + str(i & 0x3ff)] = (i, i * 3, "v" + str(i))
    return sum(v[0] for v in d.values())

def list_slice():
    base = list(range(2048))
    acc = 0
    for i in range(ITER):
        s = base[(i & 0xff) : (i & 0xff) + 256]
        acc ^= sum(s) ^ len(s)
    return acc

def json_roundtrip():
    obj = {"items": [{"id": i, "tag": "t" + str(i & 0xf), "vals": [i, -i, i*i]}
                     for i in range(256)]}
    s = json.dumps(obj)
    acc = 0
    for _ in range(ITER // 256):
        parsed = json.loads(s)
        acc ^= len(parsed["items"])
    return acc

print(dict_churn(), list_slice(), json_roundtrip())
PY
}

workload_cmd() {
  echo "python3 '$_PY_TMP/script.py' >/dev/null"
}

workload_teardown() {
  [[ -n "$_PY_TMP" && -d "$_PY_TMP" ]] && rm -rf "$_PY_TMP"
}
