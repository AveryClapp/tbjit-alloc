# Build-system workload: `cmake -S . -B <tmp>` reconfiguring the tbjit
# repo itself. Hits the kind of pattern the paper roadmap calls out as
# representative of build tooling — hash maps, string interning, list
# manipulation, polymorphic sizes. The fresh build dir guarantees a
# real configure rather than an incremental check.

WORKLOAD_NAME="cmake_configure"
WORKLOAD_DESC="cmake configure pass over the tbjit repo (fresh build dir)"

_CMAKE_TMP=""

workload_preconditions() {
  command -v cmake >/dev/null 2>&1 || { echo "cmake missing" >&2; return 1; }
  [[ -f "$REPO_ROOT/CMakeLists.txt" ]] || {
    echo "no top-level CMakeLists.txt" >&2; return 1; }
  return 0
}

workload_setup() {
  _CMAKE_TMP=$(mktemp -d -t tbjit-cmake-XXXXXX)
  export _CMAKE_TMP
}

workload_cmd() {
  # -Wno-dev silences advisories that would flood stderr and obscure
  # the time-v output we append there.
  echo "cmake -S '$REPO_ROOT' -B '$_CMAKE_TMP' -DCMAKE_BUILD_TYPE=Release -Wno-dev >/dev/null"
}

workload_teardown() {
  [[ -n "$_CMAKE_TMP" && -d "$_CMAKE_TMP" ]] && rm -rf "$_CMAKE_TMP"
}
