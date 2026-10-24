# Helpers for the concurrency test suites.  Source after init.sh.
#
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# Copying and distribution of this file, with or without modification,
# are permitted in any medium without royalty provided the copyright
# notice and this notice are preserved.
#
# pl_check CATEGORY INPUT ARGS... runs grep ARGS once as the reference
# (src/grep with GREP_THREADS=1), once with benchmarks/grep-baseline if
# it exists, and once per candidate in PL_CANDS, and requires the same
# stdout, stderr and exit status from all of them.  Each binary is run
# as "grep" via PATH, so that diagnostics agree.  INPUT is "-" for no
# standard input, FILE to redirect standard input from FILE, or "|FILE"
# to pipe FILE into grep.  CATEGORY names the paths the case exercises;
# the case is skipped unless PL_ONLY is empty or shares a word with it.
#
# A candidate is BIN:THREADS:MODE, where BIN is rel, hooks or shrunk
# (src/grep, src/grep-hooks, src/grep-shrunk) and MODE is one of
#   plain        run as is
#   cpu2         run on two processors only (forces preemption)
#   chaosN       run under chaos-shim.so with seed N
#   ioN          run under chaos-shim.so with short reads from pipes and
#                FIFOs and EINTR from every read, seed N
#   ioregN       likewise, with short reads from regular files too
#   delay=SPEC   run with GREP_DELAY_AT=SPEC (SPEC uses ";" for ",")
# A failing case prints a command line that replays it.

pl_fail=0
pl_cases=0
pl_runs=0
PL_ONLY=${PL_ONLY-}
PL_CANDS=${PL_CANDS-}

pl_timeout=pl_with_sig_dfl
if (timeout 10s true) >/dev/null 2>&1; then
  pl_timeout="timeout ${PL_TIMEOUT:-60}"
fi

pl_have_base=no
pl_have_shim=no

pl_setup_bins ()
{
  mkdir -p pl_bin/rel pl_bin/hooks pl_bin/shrunk pl_bin/base \
    || framework_failure_
  ln -sf "$abs_top_builddir/src/grep" pl_bin/rel/grep || framework_failure_
  for b in hooks shrunk; do
    if test -x "$abs_top_builddir/src/grep-$b"; then
      ln -sf "$abs_top_builddir/src/grep-$b" pl_bin/$b/grep \
        || framework_failure_
    fi
  done
  if test -x "$abs_top_srcdir/benchmarks/grep-baseline"; then
    ln -sf "$abs_top_srcdir/benchmarks/grep-baseline" pl_bin/base/grep \
      || framework_failure_
    pl_have_base=yes
  fi
}

# Return success if test binary BIN exists and honors GREP_DELAY_AT.
pl_have_bin ()
{
  test -x pl_bin/$1/grep || return 1
  case $1 in
    hooks|shrunk)
      test "$(env GREP_DELAY_AT=probe pl_bin/$1/grep x </dev/null)" \
        = grep-test-hooks;;
  esac
}

# Build chaos-shim.so and check that grep still works under it.  This
# fails under sanitizers that insist on preloading their own runtime,
# and on platforms such as macOS where LD_PRELOAD is ignored.
pl_build_shim ()
{
  ${CC:-cc} -shared -fPIC -O2 -o chaos-shim.so "$abs_srcdir/chaos-shim.c" \
    -ldl -lpthread 2>/dev/null || return 1
  rm -f chaos-probe
  out=$(echo pl_probe \
        | env LD_PRELOAD="$PWD/chaos-shim.so" \
              CHAOS_PROBE_FILE="$PWD/chaos-probe" CHAOS_YIELD=1 \
              CHAOS_SHORT_READ=1 CHAOS_SHORT_READ_REG=1 GREP_THREADS=2 \
              pl_bin/rel/grep pl_probe 2>&1)
  test "$out" = pl_probe && test -f chaos-probe || {
    rm -f chaos-probe
    return 1
  }
  rm -f chaos-probe
  pl_have_shim=yes
}

# Run a command with SIGPIPE, SIGHUP and SIGTERM unblocked and restored
# to SIG_DFL, in case the parent environment blocked or ignored them.
pl_with_sig_dfl ()
{
  { local is_tracing=`{ :; } 2>&1`; } 2>/dev/null
  { set +x; } 2>/dev/null
  $PERL -MPOSIX -e '
    POSIX::sigprocmask (POSIX::SIG_UNBLOCK (),
      POSIX::SigSet->new (POSIX::SIGPIPE (), POSIX::SIGHUP (),
                          POSIX::SIGTERM (), POSIX::SIGALRM ()));
    alarm shift; $SIG{PIPE} = "DEFAULT"; $SIG{HUP} = "DEFAULT";
    $SIG{TERM} = "DEFAULT"; exec @ARGV or die $!' \
    "${PL_TIMEOUT:-60}" "$@"
  local rc=$?
  test -n "$is_tracing" && set -x
  { return $rc; } 2>/dev/null
}

# Run a command with SIGPIPE set to SIG_IGN and an alarm timeout,
# without an external timeout wrapper that might reset SIGPIPE.
pl_with_sig_ign ()
{
  { local is_tracing=`{ :; } 2>&1`; } 2>/dev/null
  { set +x; } 2>/dev/null
  $PERL -MPOSIX -e '
    POSIX::sigprocmask (POSIX::SIG_UNBLOCK (),
      POSIX::SigSet->new (POSIX::SIGHUP (), POSIX::SIGTERM (),
                          POSIX::SIGALRM ()));
    alarm shift; $SIG{PIPE} = "IGNORE"; $SIG{HUP} = "DEFAULT";
    $SIG{TERM} = "DEFAULT"; exec @ARGV or die $!' \
    "${PL_TIMEOUT:-60}" "$@"
  local rc=$?
  test -n "$is_tracing" && set -x
  { return $rc; } 2>/dev/null
}

# Close inherited file descriptors >= 3, set RLIMIT_NOFILE to $1, and
# exec the remaining arguments.
pl_with_nofile ()
{
  { local is_tracing=`{ :; } 2>&1`; } 2>/dev/null
  { set +x; } 2>/dev/null
  $PERL -MPOSIX -e 'POSIX::close($_) for 3..1024; exec @ARGV or die $!' \
    sh -c 'ulimit -n "$1" && shift && exec "$@"' sh "$@"
  local rc=$?
  test -n "$is_tracing" && set -x
  { return $rc; } 2>/dev/null
}

# Write the corpus used by the standard cases.
pl_make_corpus ()
{
  cat <<'EOF' > pl_gen.pl || framework_failure_
use strict;
use warnings;
my $seed = 12345;
sub rnd { $seed = ($seed * 1103515245 + 12345) % 2147483648; return $seed % $_[0]; }
sub put { my ($f, $s) = @_; open my $h, '>', $f or die "$f: $!"; print $h $s; close $h; }
sub big {
  my ($n, $long_at, $nul_at) = @_;
  my $s = '';
  for my $i (1 .. $n) {
    if (defined $nul_at && $i == $nul_at) { $s .= "nul\0here MATCH_ALPHA$i tail\n"; }
    elsif ($i == $long_at) { $s .= ('L' x 200000) . " MATCH_ALPHA$i tail\n"; }
    elsif ($i % 2500 == 0) { $s .= "line $i MATCH_ALPHA$i tail\n"; }
    elsif ($i % 3333 == 0) { $s .= "line $i MATCH_BETA$i ab ab x\n"; }
    else { $s .= "line $i filler " . ('x' x 50) . "\n"; }
  }
  return $s;
}
sub small {
  my $n = 3 + rnd (25);
  my $s = '';
  for my $i (1 .. $n) {
    my $r = rnd (100);
    if ($r < 6) { $s .= "row $i MATCH_ALPHA" . rnd (1000) . " tail\n"; }
    elsif ($r < 10) { $s .= "row $i MATCH_BETA" . rnd (1000) . " ab ab\n"; }
    else { $s .= "row $i filler data " . rnd (100000) . "\n"; }
  }
  return $s;
}
put ('big.txt', big (40000, 20000, undef));
put ('bignul.txt', big (40000, 20000, 34000));
put ('stdin.txt', big (24000, 12000, undef));
mkdir 'tree'; mkdir 'ops';
put ('tree/a_big.txt', big (40000, 20000, undef));
for my $d (0 .. 11) {
  mkdir "tree/d$d";
  put (sprintf ("tree/d$d/f%02d.txt", $_), small ()) for 0 .. 39;
}
mkdir 'tree/d3/sub'; mkdir 'tree/d3/sub/subsub';
put ("tree/d3/sub/subsub/s$_.txt", small ()) for 0 .. 19;
mkdir 'tree/wide';
put (sprintf ("tree/wide/w%03d.txt", $_), small ()) for 0 .. 299;
my $p = 'tree/deep';
mkdir $p;
for my $l (1 .. 10) { $p .= "/l$l"; mkdir $p; }
put ("$p/leaf.txt", "leaf MATCH_ALPHA999 tail\n");
put ('tree/bin.dat', "MATCH_ALPHA1 before\n" . ("\0" x 10) . "\nMATCH_BETA2 after\n");
put ('tree/empty.txt', '');
mkdir 'tree/emptydir';
put ('tree/nonl.txt', 'last MATCH_ALPHA7 without newline');
put (sprintf ("ops/o%02d", $_), small ()) for 0 .. 23;
EOF
  $PERL pl_gen.pl || framework_failure_
}

pl_bin_dir ()
{
  echo "$PWD/pl_bin/$1"
}

# pl_exec BIN PREFIX INPUT ENV -- ARGS...
pl_exec ()
{
  pe_bin=$1 pe_out=$2 pe_in=$3 pe_env=$4
  shift 5
  pe_path="$(pl_bin_dir $pe_bin):$PATH"
  pl_runs=$((pl_runs + 1))
  case $pe_in in
    -) $pl_timeout env $pe_env PATH="$pe_path" grep "$@" \
         < /dev/null > "$pe_out.out" 2> "$pe_out.err"
       echo $? > "$pe_out.rc";;
    \|*) pl_with_sig_dfl cat "${pe_in#|}" 2>/dev/null \
         | $pl_timeout env $pe_env PATH="$pe_path" grep "$@" \
             > "$pe_out.out" 2> "$pe_out.err"
       echo $? > "$pe_out.rc";;
    *) $pl_timeout env $pe_env PATH="$pe_path" grep "$@" \
         < "$pe_in" > "$pe_out.out" 2> "$pe_out.err"
       echo $? > "$pe_out.rc";;
  esac
}

# Print the environment for candidate mode $1 with $2 threads.
pl_mode_env ()
{
  pm_env="GREP_THREADS=$2"
  case $1 in
    plain) ;;
    cpu2) ;;
    chaos*) pm_env="$pm_env LD_PRELOAD=$PWD/chaos-shim.so CHAOS_SEED=${1#chaos}"
            pm_env="$pm_env CHAOS_YIELD=0.3 CHAOS_SLEEP=0.02:400"
            pm_env="$pm_env CHAOS_SPURIOUS=0.25";;
    ioreg*) pm_env="$pm_env LD_PRELOAD=$PWD/chaos-shim.so CHAOS_SEED=${1#ioreg}"
            pm_env="$pm_env CHAOS_SHORT_READ=0.5 CHAOS_SHORT_READ_REG=0.5"
            pm_env="$pm_env CHAOS_EINTR=0.2 CHAOS_YIELD=0.1";;
    io*) pm_env="$pm_env LD_PRELOAD=$PWD/chaos-shim.so CHAOS_SEED=${1#io}"
         pm_env="$pm_env CHAOS_SHORT_READ=0.5 CHAOS_EINTR=0.2 CHAOS_YIELD=0.1";;
    delay=*) pm_env="$pm_env GREP_DELAY_AT=$(echo "${1#delay=}" | tr ';' ',')";;
  esac
  echo "$pm_env $PL_ENV"
}

# Compare the results with prefix $1 against those with prefix $2.
pl_same ()
{
  cmp -s "$1.out" "$2.out" && cmp -s "$1.err" "$2.err" \
    && cmp -s "$1.rc" "$2.rc"
}

pl_report ()
{
  pr_what=$1 pr_got=$2 pr_ref=$3
  shift 3
  echo "FAIL [$pr_what]: grep $*" >&2
  echo "  replay: (cd $PWD && $pr_replay)" >&2
  echo "  rc: got $(cat $pr_got.rc), expected $(cat $pr_ref.rc)" >&2
  for s in out err; do
    if ! cmp -s "$pr_got.$s" "$pr_ref.$s"; then
      echo "  std$s differs:" >&2
      diff "$pr_ref.$s" "$pr_got.$s" | head -n 8 >&2
    fi
  done
  pl_fail=1
}

pl_in_only ()
{
  test -z "$PL_ONLY" && return 0
  for pi_c in $1; do
    for pi_o in $PL_ONLY; do
      test "$pi_c" = "$pi_o" && return 0
    done
  done
  return 1
}

# pl_check CATEGORY INPUT ARGS...
pl_check ()
{
  pc_cat=$1 pc_in=$2
  shift 2
  pl_in_only "$pc_cat" || return 0
  # With PL_STOP_ON_FAIL=yes, stop at the first failure.
  test "${PL_STOP_ON_FAIL-}" = yes && test $pl_fail = 1 && return 0
  pl_cases=$((pl_cases + 1))
  pc_dir=pl_res/c$pl_cases
  mkdir -p $pc_dir || framework_failure_
  pl_exec rel $pc_dir/ref "$pc_in" "GREP_THREADS=1 $PL_ENV" -- "$@"
  case $(cat $pc_dir/ref.rc) in
    124|142)
      pr_replay="GREP_THREADS=1 grep $*"
      echo "FAIL [reference timed out]: grep $*" >&2
      pl_fail=1
      return;;
  esac
  if test $pl_have_base = yes && test "${PL_NO_BASE-}" != yes; then
    pl_exec base $pc_dir/base "$pc_in" "$PL_ENV" -- "$@"
    pr_replay="$abs_top_srcdir/benchmarks/grep-baseline $*"
    pl_same $pc_dir/base $pc_dir/ref \
      || pl_report "baseline vs GREP_THREADS=1" $pc_dir/ref $pc_dir/base "$@"
  fi
  for pc_cand in $PL_CANDS; do
    pc_bin=${pc_cand%%:*}
    pc_rest=${pc_cand#*:}
    pc_thr=${pc_rest%%:*}
    pc_mode=${pc_rest#*:}
    case $pc_bin in
      hooks|shrunk) test "$(eval echo \$pl_have_$pc_bin)" = yes || continue;;
    esac
    case $pc_mode in
      chaos*|io*) test $pl_have_shim = yes || continue;;
    esac
    pc_env=$(pl_mode_env "$pc_mode" "$pc_thr")
    pc_out=$pc_dir/$(echo "$pc_cand" | tr ':=;*,' '_____')
    case $pc_mode in
      cpu2) pl_taskset_exec $pc_bin $pc_out "$pc_in" "$pc_env" "$@";;
      *) pl_exec $pc_bin $pc_out "$pc_in" "$pc_env" -- "$@";;
    esac
    pr_replay="env $pc_env PATH=$(pl_bin_dir $pc_bin):\$PATH grep $*"
    case $pc_in in
      -) ;;
      \|*) pr_replay="cat ${pc_in#|} | $pr_replay";;
      *) pr_replay="$pr_replay < $pc_in";;
    esac
    case $(cat $pc_out.rc) in
      124|142)
        echo "FAIL [hang: timed out after ${PL_TIMEOUT:-60}s, $pc_cand]: grep $*" >&2
        echo "  replay: (cd $PWD && $pr_replay)" >&2
        pl_fail=1
        continue;;
    esac
    pl_same $pc_out $pc_dir/ref \
      || pl_report "$pc_cand" $pc_out $pc_dir/ref "$@"
  done
  test $pl_fail = 1 || rm -rf $pc_dir
}

pl_taskset_exec ()
{
  pt_bin=$1 pt_out=$2 pt_in=$3 pt_env=$4
  shift 4
  if (taskset -c 0,1 true) >/dev/null 2>&1; then
    pl_runs=$((pl_runs + 1))
    pt_path="$(pl_bin_dir $pt_bin):$PATH"
    case $pt_in in
      -) $pl_timeout taskset -c 0,1 env $pt_env PATH="$pt_path" grep "$@" \
           < /dev/null > "$pt_out.out" 2> "$pt_out.err";;
      \|*) pl_with_sig_dfl cat "${pt_in#|}" 2>/dev/null \
           | $pl_timeout taskset -c 0,1 env $pt_env PATH="$pt_path" grep "$@" \
               > "$pt_out.out" 2> "$pt_out.err";;
      *) $pl_timeout taskset -c 0,1 env $pt_env PATH="$pt_path" grep "$@" \
           < "$pt_in" > "$pt_out.out" 2> "$pt_out.err";;
    esac
    echo $? > "$pt_out.rc"
  else
    pl_exec $pt_bin $pt_out "$pt_in" "$pt_env" -- "$@"
  fi
}

# The standard cases: every parallel input path crossed with the options
# that change how work is split, ordered and committed.
pl_standard_cases ()
{
  re='MATCH_(ALPHA|BETA)[0-9]+'
  # Recursive traversal (walk_run_dirs).
  pl_check "tree" - -r MATCH_ALPHA tree
  pl_check "tree" - -r -n MATCH_ALPHA tree
  pl_check "tree" - -r -c MATCH_BETA tree
  pl_check "tree" - -r -l MATCH_ALPHA tree
  pl_check "tree" - -r -L MATCH_ALPHA tree
  pl_check "tree" - -r -o -b -E "$re" tree
  pl_check "tree" - -r -n -C2 MATCH_BETA tree
  pl_check "tree" - -r -v -c MATCH_ALPHA tree
  pl_check "tree" - -r -m2 MATCH_ALPHA tree
  pl_check "tree" - -r -Z -l -F -e MATCH_ALPHA -e MATCH_BETA tree
  pl_check "tree" - -R -h -w -E '(ab) \1' tree
  pl_check "tree quiet" - -r -q MATCH_ALPHA tree
  pl_check "tree quiet" - -r -q NEVER_PRESENT tree
  pl_check "tree quiet" - -r -q -F NEVER_PRESENT tree
  pl_check "tree" - -r -l MATCH_ALPHA tree/d1 ops/o01 tree/d2
  # Many operands (walk_run_cmd_files).
  pl_check "cmd" - MATCH_ALPHA ops/o00 ops/o01 ops/o02 ops/o03 ops/o04 \
    ops/o05 ops/o06 ops/o07 ops/o08 ops/o09 ops/o10 ops/o11 ops/o12 \
    ops/o13 ops/o14 ops/o15 ops/o16 ops/o17 ops/o18 ops/o19 ops/o20
  pl_check "cmd" - -c MATCH_BETA ops/o0? ops/o1? ops/o2?
  pl_check "cmd" - -l -F -e MATCH_ALPHA -e MATCH_BETA ops/o0? ops/o1? ops/o2?
  pl_check "cmd" - -n -C1 MATCH_ALPHA ops/o0? ops/o1? ops/o2?
  pl_check "cmd quiet" - -q MATCH_BETA ops/o0? ops/o1? ops/o2?
  # Few operands (worker pool), mixing small and chunked files.
  pl_check "pool chunk" - -n MATCH_ALPHA ops/o01 big.txt ops/o02
  pl_check "pool chunk" - -c MATCH_ALPHA big.txt bignul.txt ops/o03
  # A single large file (chunked_grep_fd in each mode).
  pl_check "chunk" - MATCH_ALPHA big.txt
  pl_check "chunk" - -c MATCH_ALPHA big.txt
  pl_check "chunk" - -n MATCH_ALPHA big.txt
  pl_check "chunk" - -n -C2 MATCH_BETA big.txt
  pl_check "chunk" - -o -b -n -E "$re" big.txt
  pl_check "chunk" - -n filler big.txt
  pl_check "chunk" - -l MATCH_ALPHA big.txt
  pl_check "chunk" - -L MATCH_ALPHA big.txt
  pl_check "chunk" - -v -c filler big.txt
  pl_check "chunk" - MATCH_ALPHA bignul.txt
  pl_check "chunk" - -c MATCH_BETA bignul.txt
  pl_check "chunk" - -n -a MATCH_ALPHA bignul.txt
  pl_check "chunk quiet" - -q MATCH_BETA big.txt
  # Standard input: regular file, pipe, and mixed with other operands.
  pl_check "stdin" big.txt -n MATCH_ALPHA
  pl_check "stdin" big.txt -c MATCH_BETA
  pl_check "pipe" "|stdin.txt" MATCH_ALPHA
  pl_check "pipe" "|stdin.txt" -n -b MATCH_BETA
  pl_check "pipe" "|stdin.txt" -c filler
  pl_check "pipe" "|stdin.txt" -v -c MATCH_ALPHA
  pl_check "pipe quiet" "|stdin.txt" -q MATCH_BETA
  pl_check "pool stdin" stdin.txt -n MATCH_ALPHA ops/o04 - big.txt
  pl_check "pool pipe" "|stdin.txt" -H -c MATCH_ALPHA - tree/d2/f01.txt
  if test "$PCRE_WORKS" = 1; then
    pl_check "tree" - -r -P 'MATCH_ALPHA\d+' tree
    pl_check "chunk" - -n -P 'MATCH_ALPHA\d+(?= tail)' big.txt
  fi
}

# Check the case where the output file is inside the searched tree, so
# that the output must be committed exactly (see EXACT_STDOUT).
pl_check_outfile ()
{
  pl_in_only "tree" || return 0
  pl_cases=$((pl_cases + 1))
  env GREP_THREADS=1 PATH="$(pl_bin_dir rel):$PATH" grep "$@" \
    > tree/zz_out.txt 2> pl_of_ref.err
  echo $? > pl_of_ref.rc
  mv tree/zz_out.txt pl_of_ref.out || framework_failure_
  for po_cand in $PL_CANDS; do
    po_bin=${po_cand%%:*}
    po_rest=${po_cand#*:}
    po_thr=${po_rest%%:*}
    po_mode=${po_rest#*:}
    case $po_bin in
      hooks|shrunk) test "$(eval echo \$pl_have_$po_bin)" = yes || continue;;
    esac
    case $po_mode in
      chaos*|io*) test $pl_have_shim = yes || continue;;
      cpu2) po_mode=plain;;
    esac
    po_env=$(pl_mode_env "$po_mode" "$po_thr")
    $pl_timeout env $po_env PATH="$(pl_bin_dir $po_bin):$PATH" grep "$@" \
      > tree/zz_out.txt 2> pl_of_got.err
    echo $? > pl_of_got.rc
    mv tree/zz_out.txt pl_of_got.out || framework_failure_
    pr_replay="env $po_env grep $* > tree/zz_out.txt"
    pl_same pl_of_got pl_of_ref \
      || pl_report "$po_cand, output in tree" pl_of_got pl_of_ref "$@"
  done
}

pl_summary ()
{
  echo "pl: $pl_cases cases, $pl_runs runs, fail=$pl_fail" >&2
}
