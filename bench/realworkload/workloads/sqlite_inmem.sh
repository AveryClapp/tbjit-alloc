# Database workload: sqlite3 in-memory CREATE / INSERT / SELECT.
# In-memory so disk I/O doesn't dominate wall time, but the query
# planner still does its full job — mixed allocation sizes (B-tree
# nodes, parser state, row buffers) make this a meaningfully different
# pattern from the compiler and scripting workloads.

WORKLOAD_NAME="sqlite_inmem"
WORKLOAD_DESC="sqlite3 in-memory: CREATE + 100k INSERT + aggregate SELECT"

_SQL_TMP=""

workload_preconditions() {
  command -v sqlite3 >/dev/null 2>&1 || { echo "sqlite3 missing" >&2; return 1; }
  return 0
}

workload_setup() {
  _SQL_TMP=$(mktemp -d -t tbjit-sql-XXXXXX)
  export _SQL_TMP
  # Generate a SQL script. Wrapping inserts in a single transaction
  # is crucial: without it sqlite fsyncs per row and disk dominates;
  # with it the bulk of time is in B-tree node allocation, which is
  # what we want to characterize.
  {
    echo "PRAGMA synchronous = OFF;"
    echo "PRAGMA journal_mode = MEMORY;"
    echo "CREATE TABLE t (id INTEGER PRIMARY KEY, k TEXT, v INTEGER, w REAL);"
    echo "BEGIN;"
    awk 'BEGIN {
      for (i = 0; i < 100000; ++i) {
        k = sprintf("key_%d", i % 1024);
        printf "INSERT INTO t(k,v,w) VALUES (\x27%s\x27, %d, %.4f);\n", \
          k, i, i * 0.001;
      }
    }'
    echo "COMMIT;"
    echo "SELECT k, COUNT(*), SUM(v), AVG(w) FROM t GROUP BY k ORDER BY k LIMIT 16;"
    echo "SELECT COUNT(*) FROM t WHERE v % 7 = 0;"
  } > "$_SQL_TMP/work.sql"
}

workload_cmd() {
  # :memory: keeps the DB in RAM. -bail aborts on first error so a
  # surprise schema problem fails fast rather than skewing the timing.
  echo "sqlite3 -bail ':memory:' < '$_SQL_TMP/work.sql' >/dev/null"
}

workload_teardown() {
  [[ -n "$_SQL_TMP" && -d "$_SQL_TMP" ]] && rm -rf "$_SQL_TMP"
}
