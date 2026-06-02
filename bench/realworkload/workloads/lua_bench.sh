# Scripting-runtime workload: Lua. Repeatedly builds and discards tables and
# strings, exercising Lua's incremental GC and its many small table/string
# allocations -- an object-lifecycle pattern with high free/alloc turnover,
# a good candidate for the reaping / free-list strategies.

WORKLOAD_NAME="lua_bench"
WORKLOAD_DESC="lua table + string churn (incremental-GC object lifecycle)"

_LUA_BIN=""
_LUA_TMP=""

workload_preconditions() {
  for c in lua5.4 lua5.3 lua; do
    if command -v "$c" >/dev/null 2>&1; then _LUA_BIN="$c"; export _LUA_BIN; return 0; fi
  done
  echo "lua missing" >&2; return 1
}

workload_setup() {
  _LUA_TMP=$(mktemp -d -t tbjit-lua-XXXXXX)
  export _LUA_TMP
  cat > "$_LUA_TMP/work.lua" <<'LUA'
local sink = 0
for round = 1, 400 do
  local t = {}
  for i = 1, 50000 do
    t[i] = { k = "key_" .. (i % 1024), v = i, s = tostring(i * 3) }
  end
  for i = 1, #t do sink = sink + t[i].v + #t[i].s end
  t = nil  -- drop for the collector
end
io.write(sink, "\n")
LUA
}

workload_cmd() {
  echo "$_LUA_BIN '$_LUA_TMP/work.lua' >/dev/null"
}

workload_teardown() {
  [[ -n "$_LUA_TMP" && -d "$_LUA_TMP" ]] && rm -rf "$_LUA_TMP"
}
