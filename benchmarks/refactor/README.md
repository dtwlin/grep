# Refactoring gates

These tools check that a change to the C sources leaves the installed
`grep` as fast as before.  A refactor is classified by what it may
change, and each class has a gate that proves its claim:

- **Byte-identical:** `src/grep`, `src/grep-hooks` and `src/grep-shrunk`
  load the same bytes.  Gate: `elfcmp.py`.  No timing is needed.
- **Instruction-identical:** every function keeps its instructions and
  every data object its contents; only addresses change.  Gate:
  `asmdiff.py`.  If `asmdiff.py` reports that writable objects now
  share cache lines differently, also run a short A/B timing: moving
  data can change speed without changing any instruction.
- **Cold path:** only code that runs once per run, operand or pattern
  changes.  Gates: `route_matrix.py` or another oracle that pins the
  behavior, `asmdiff.py --fail-on watch`, and A/B timing if a hot
  function changes.

Wall-clock timing is the only speed signal on the test host, which
exposes no hardware performance counters.

## Tools

- [`elfcmp.py`](elfcmp.py): two ELF files, or the three programs of two
  build trees, load the same bytes.
- [`asmdiff.py`](asmdiff.py): two builds have the same normalized
  instructions in every function and the same contents in every data
  object.
- [`hot_functions.txt`](hot_functions.txt): the functions that run per
  line, buffer, chunk or file, for `asmdiff.py --watch`.
- [`route_matrix.py`](route_matrix.py): two test builds take the same
  search route and print the same output, errors and exit status in
  2,742 cases, which take every route.
- [`sc_diff.py`](sc_diff.py): a change adds no `make syntax-check`
  violation.
- [`../kernel/ab_bench.py`](../kernel/ab_bench.py): interleaved A/B
  timing on the kernel scenarios, with the decision rule below.
- [`selftest.py`](selftest.py): the tools above catch real changes and
  ignore moves.

The tools need Python 3, and `elfcmp.py` and `asmdiff.py` need GNU
binutils; `asmdiff.py` reads x86-64 code only.  Run
`python3 benchmarks/refactor/selftest.py` after changing any of them.

### elfcmp.py

`elfcmp.py A B` compares the name, type, flags, address, size and
contents of every allocated section, the entry point and the program
headers; for object files, also the relocations of the allocated
sections.  Debug information, symbols and `.comment` are not loaded
and are ignored.  So is `.note.gnu.build-id`: the linker hashes the
whole file into it, so it changes with the debug information even when
the code does not.  `--strict` compares it too.  Given two directories,
it compares `src/grep`, `src/grep-hooks` and `src/grep-shrunk` in each.

### asmdiff.py

`asmdiff.py A B` disassembles both files with GNU objdump and compares
each function after normalizing what moving code changes:

- A branch target becomes the index of the target instruction, with
  the function name if it is in another function.
- A RIP-relative operand becomes what it addresses: a symbol plus the
  offset into it, a pointer slot such as a GOT entry (named by what it
  points to), the targets of a jump table, a string, a constant (its
  bytes, when the instruction shows their size), or else the section.
- A thread-local operand becomes the thread-local symbol, also when
  the offset comes from the thread pointer in `%fs:0x0` or from an
  immediate loaded into the register of `%fs:(REG)`.
- Alignment padding is dropped, and so are GCC clone numbers
  (`.part.N`, `.isra.N`) and LTO partition names (`.lto_priv.N`).

In object files, relocations name the referenced symbols.  It also
compares every named data object, with pointers written as what they
point to, and lists the pairs of writable objects that start or stop
sharing a 64-byte cache line.  It does not check anonymous constants
whose size no instruction shows, or code and data reached only through
computed addresses.  It refuses stripped files.

`--fail-on any` (the default) fails on any changed, added or removed
function or data object.  `--fail-on watch` fails only if a function
in the `--watch` file changed, was inlined, or moved out of line; the
report names the watch patterns that match no out-of-line function.
`--diff NAME` prints the normalized instructions (or data) of `NAME`
from A and B as a unified diff.

Moving a function within `src/grep.c` can change inlining anywhere in
the program, even with no other edit, and can reorder the writable
global variables.  For example, moving `producer_run` next to the
worker pool code changed 12 functions under LTO, among them
`re_search_internal`, `walk_read_dir` and `walk_participate`.  So check
every move with `asmdiff.py`.

### route_matrix.py

`route_matrix.py --a A/src/grep-shrunk --b B/src/grep-shrunk` runs both
test builds on every combination of standard input (closed,
`/dev/null`, a small file, a large file at offset 0 and at offset 1, a
pipe, a pipe with a NUL byte), operands (none, `-`, a small file and
`-`, a small file, a large file, a file with a NUL byte, a directory
with `-r`, 16 files), 16 option sets and `GREP_THREADS` 1, 2 and 32.
It adds runs whose output is the searched file, and runs aimed at the
routes for binary files, NUL bytes, many matches and encoding errors.
It compares the sorted `GREP_ROUTE_LOG` and the output, errors and exit
status of each run, and fails unless every route is taken, like
`tests/parallel-route-trace`.  On a 128-CPU host it takes about 12
seconds.  The input sizes suit `grep-shrunk`; use `--large-size 4M`
with `grep-hooks`.

### sc_diff.py

`make syntax-check` already fails on the fork, mostly on long lines in
`benchmarks/` and `src/`.  So run it with `-k` in both trees and
compare:

```sh
make -k syntax-check > A.log 2>&1        # in tree A
make -k syntax-check > B.log 2>&1        # in tree B
benchmarks/refactor/sc_diff.py A.log B.log
```

Each violation is keyed by rule and text, without file name and line
number, so moving an existing violation does not count as new.  Only
committed files are checked: gnulib's `vc-list-files` lists `HEAD`, not
the index, so commit B (in the copy, if need be) before running it.
`maint.mk` sets `LC_ALL=C`, so `sc_long_lines` counts bytes, not
characters: keep new lines within 80 bytes.

`make distcheck` runs `make syntax-check` first when the tree has a
`.git` directory, so it fails on the fork for the same reason; run it
in a copy whose `.git` is moved away.

## Building A and B

Commit the candidate first, so that the tree is clean.  Then build
the parent commit (A) and the candidate (B) in separate copies of the
bootstrapped tree, with the version string pinned in both so that it
does not differ by commit hash.  The pin must keep the form that
`tests/parallel-env-limits` checks:

```sh
C1='-O3 -march=native -flto=auto -fno-plt -fomit-frame-pointer'
C1="$C1 -falign-functions=32 -falign-loops=32"
for t in A B; do
  rsync -a --delete ./ /dev/shm/refactor/$t/
done
git -C /dev/shm/refactor/A checkout -q HEAD~1
for t in A B; do
  (cd /dev/shm/refactor/$t &&
   printf 'v2000.01.01+g0000000\n' > .sc-version &&
   ./configure CFLAGS="$C1" && make clean && make -j32 &&
   make -j32 -C src grep-hooks grep-shrunk)
done
```

Run `configure` in a copy before any `make`: the copied Makefiles still
name the original tree.  Do not use `make -B`, which reruns
`config.status`.  Check each gate in three configurations:

- **C1:** `$C1` above, the published Linux build (LTO).
- **C2:** `$C1` without `-flto=auto`, like builds without LTO on macOS
  and in distributions.
- **C3:** `-g -O2`, the default `./configure && make`.

For sanitizer runs, use separate copies configured with
`CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'`
or `CFLAGS='-O1 -g -fsanitize=thread'`.

## Commands

```sh
benchmarks/refactor/elfcmp.py A B
benchmarks/refactor/asmdiff.py A/src/grep B/src/grep \
  --watch benchmarks/refactor/hot_functions.txt
benchmarks/refactor/route_matrix.py --a A/src/grep-shrunk \
  --b B/src/grep-shrunk
make check -j32
python3 benchmarks/run_benchmarks.py --conformance-only
python3 benchmarks/tree_conformance.py --quick --baseline /usr/bin/grep \
  --threads 1,2,7,16,64
python3 benchmarks/kwset_diff.py --sweep --baseline /usr/bin/grep
python3 benchmarks/exhaustive_gap_fuzzer.py
benchmarks/refactor/sc_diff.py A.log B.log
```

## A/B timing decision rule

`benchmarks/kernel/ab_bench.py --a A/src/grep --b B/src/grep --out DIR
--scenarios IDS --precise --summarize` applies this rule:

- **Sample:** at least 20 interleaved A/B pairs per scenario, in C1
  and C2.
- **Statistic:** the median of the per-pair ratios B/A and its 95%
  bootstrap confidence interval (10,000 resamples).
- **Same:** the interval lies within [1/1.015, 1.015].
- **Block:** only if the interval lies entirely above 1.015 both for
  B and for a rebuild of B with `-falign-functions=64`.  A slowdown
  that goes away with another alignment is code layout, not a
  regression.
- **More pairs:** if an interval is wider than ±3%, or neither within
  the band nor above it, run 20 more pairs into the same directory and
  summarize again; `ab_bench.py` exits with status 3 until every
  scenario is settled.

Run an A/A comparison of two builds of the same commit first to see
the noise of the host.  On the 128-CPU test host, A/A intervals with
20 pairs were ±3 to 6% wide and contained 1, and the 2 ms scenario
`L-01` needed 100 pairs for ±3%.
