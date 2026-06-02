# Scripting-runtime workload: Perl (preinstalled on Ubuntu, no apt needed).
# Hash- and array-heavy churn exercises Perl's SV/HV allocator and arena
# recycling -- a string-and-hash allocation pattern distinct from the V8 and
# Lua object loops.

WORKLOAD_NAME="perl_bench"
WORKLOAD_DESC="perl hash/array/string churn (SV/HV arena recycling)"

_PERL_TMP=""

workload_preconditions() {
  command -v perl >/dev/null 2>&1 || { echo "perl missing" >&2; return 1; }
  return 0
}

workload_setup() {
  _PERL_TMP=$(mktemp -d -t tbjit-perl-XXXXXX)
  export _PERL_TMP
  cat > "$_PERL_TMP/work.pl" <<'PL'
my $sink = 0;
for my $round (1 .. 400) {
    my %h;
    for my $i (0 .. 49999) {
        my $k = "key_" . ($i % 1024);
        push @{ $h{$k} }, "val_$i";
    }
    for my $k (keys %h) { $sink += scalar @{ $h{$k} }; }
    # %h goes out of scope each round -> SVs recycled.
}
print "$sink\n";
PL
}

workload_cmd() {
  echo "perl '$_PERL_TMP/work.pl' >/dev/null"
}

workload_teardown() {
  [[ -n "$_PERL_TMP" && -d "$_PERL_TMP" ]] && rm -rf "$_PERL_TMP"
}
