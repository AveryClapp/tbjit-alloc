# Compression workload: multi-threaded xz. `-T0` spawns one worker thread per
# core, each with its own large LZMA dictionary/buffer -- a multi-threaded,
# large-allocation pattern that exercises the thread-aware strategies
# (ThreadLocalFreeList / ProducerConsumer) the single-threaded compiler and
# scripting workloads rarely trigger.

WORKLOAD_NAME="xz_compress"
WORKLOAD_DESC="xz -T0 -6 multi-threaded compression of a ~48 MiB corpus"

_XZ_TMP=""

workload_preconditions() {
  command -v xz >/dev/null 2>&1 || { echo "xz missing" >&2; return 1; }
  return 0
}

workload_setup() {
  _XZ_TMP=$(mktemp -d -t tbjit-xz-XXXXXX)
  export _XZ_TMP
  # Semi-compressible corpus: repeated-but-varying text so xz does real work
  # (pure random would barely compress; pure zeros would be trivial).
  awk 'BEGIN {
    for (i = 0; i < 700000; i++)
      printf "line %d the quick brown fox %d jumps over lazy dog %d\n", i, i % 997, i % 31;
  }' > "$_XZ_TMP/corpus.txt"
}

workload_cmd() {
  # Block size forces multiple independent compression streams -> more threads
  # actually used even on a small core count.
  echo "xz -T0 --block-size=4MiB -6 -c '$_XZ_TMP/corpus.txt' >/dev/null"
}

workload_teardown() {
  [[ -n "$_XZ_TMP" && -d "$_XZ_TMP" ]] && rm -rf "$_XZ_TMP"
}
