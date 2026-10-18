<!-- Copyright (C) 2026 David Lin <dtwlin@gmail.com> -->
# Linux Kernel-Tree Benchmark

Reproducible, high-fidelity benchmark harness comparing multi-threaded GNU `grep` against stock upstream GNU `grep`, `ripgrep`, `ugrep`, distro GNU `grep`, and `git grep` on the Linux kernel codebase.

## 1. Overview and Contestants

| Contestant | Binary | Provenance | Parallelism | Notes |
| :--- | :--- | :--- | :---: | :--- |
| **G0** | `benchmarks/grep-baseline` | Upstream GNU grep `v3.12-41-ge187ca5` | 1 thread | Baseline reference built with identical flags (`-O3 -march=native`) |
| **G1** | `src/grep` | Multi-threaded GNU grep fork (`master`) | Auto (32) | 128 MiB slab arena, lockless SIMD scanners, intra-file chunking, Sequence Reorder Buffer |
| **R0** | `/usr/bin/rg` | ripgrep 15.1.0 | Auto (12) | Stock Debian package (`+pcre2`, runtime AVX2) |
| **U0** | `ugrep` (`--bin-U0`) | ugrep 7.8.5 | Auto (12) | Upstream release built with its `./build.sh` defaults (`-O2`, runtime AVX2, PCRE2 JIT); default threads `min(cores − 1, 12)` |
| **G2** | `/usr/bin/grep` | Distro GNU grep 3.12 | 1 thread | System release binary (Debian generic build) |
| **X1** | `/usr/bin/git` | Git grep (`git grep`) | Auto (128) | Searches tracked files in working tree |

## 2. Directory Structure

```text
benchmarks/kernel/
├── README.md                 # Benchmark guide and fairness documentation
├── kernel_bench.py           # Benchmark runner -> results/<timestamp>/raw.jsonl + env.json
├── scenarios.py              # Complete scenario matrix (T, B, M, L, X, U, P)
├── gen_kernel_corpus.py      # Generates $KBENCH_DIR/{linux-all.c, big-24M.h, files.z}
├── gen_patterns.py           # Generates patterns/sym_{100,1000,10000}.txt and regex variants
├── evict_cache.py            # posix_fadvise(DONTNEED) page cache eviction utility
├── report.py                 # Generates summary.md, scaling.csv, equivalence.md
├── merge_results.py          # Merges one session's runs of some tools into a results directory
├── publish.py                # Generates BENCHMARK_RESULTS.md and the README headline table
├── patterns/                 # Committed deterministic pattern files (seed 42)
└── results/                  # Benchmark output directories
```

## 3. Preparation

### 3.1 Corpus Generation

Generate the derived single-file and operand list corpora into `$KBENCH_DIR`:

```bash
python3 benchmarks/kernel/gen_kernel_corpus.py \
    --ksrc /path/to/linux-stable \
    --out-dir /path/to/kbench
```

This generates:
- `linux-all.c` (`$BIG`, ~1.4 GB): Concatenation of all tracked `*.c` and `*.h` files in `git ls-files` order.
- `big-24M.h` (24 MB): Largest real source file in the tree (`dcn_3_2_0_sh_mask.h`).
- `files.z` (~3.8 MB): Null-delimited list of all tracked kernel source files.

### 3.2 Pattern Generation

Generate deterministic pattern files sampled from `EXPORT_SYMBOL*` definitions:

```bash
python3 benchmarks/kernel/gen_patterns.py \
    --ksrc /path/to/linux-stable \
    --out-dir benchmarks/kernel/patterns
```

## 4. Running Benchmarks

### 4.1 Quick Mode (`--quick`)

Runs the core scenario subset (17 scenarios) with 1 warm-up + 3 measured rounds and thread sweeps `{1, 4, 16, 32, 64, 128}`:

```bash
python3 benchmarks/kernel/kernel_bench.py --quick \
    --ksrc /path/to/linux-stable \
    --kbench-dir /path/to/kbench
```

### 4.2 Dry Run Mode (`--dry-run`)

Runs 1 pass across all selected scenarios to verify flag parity, command invocations, and exit codes:

```bash
python3 benchmarks/kernel/kernel_bench.py --dry-run --core-only \
    --ksrc /path/to/linux-stable \
    --kbench-dir /path/to/kbench
```

### 4.3 Custom Execution

```bash
python3 benchmarks/kernel/kernel_bench.py \
    --ksrc /path/to/linux-stable \
    --kbench-dir /path/to/kbench \
    --tools G0,G1,R0,U0,G2,X1 \
    --bin-U0 /path/to/ugrep \
    --categories T,B \
    --threads-sweep 1,4,16,32,64,128 \
    --rounds 5
```

`--tools` defaults to all six contestants; U0 is skipped with a note when no
`ugrep` is found and `--bin-U0` is not given.  `--fixed-threads N` runs
phases 1 and 2 with `N` threads for G1, R0, U0, and X1 (`GREP_THREADS=N`,
`-j N`, `-J N`, `--threads N`) instead of each tool's default; the published
thread-matched columns (`rg -j 32`, `ugrep -J 32`) come from such a pass.

### 4.4 Adding a Contestant to Published Results

`kernel_bench.py` rewrites `env.json` in its output directory, so a new
contestant is measured in a session directory of its own and then merged:

```bash
python3 benchmarks/kernel/kernel_bench.py --tools U0 --bin-U0 /path/to/ugrep \
    --out /tmp/kbench-session ...
python3 benchmarks/kernel/merge_results.py --src /tmp/kbench-session \
    --dst benchmarks/kernel/results/<published> --tools U0 \
    --equiv /tmp/kbench-equiv --note "why and when"
```

`merge_results.py` appends only the selected tools' runs (tagged with the
session name), copies only their `env.json` and `diff_samples.json` entries,
and regenerates the reports.  `--equiv` names the directory of a
`--dry-run --tools G0,U0` pass, whose `diff_samples.json` holds the Gate B
diff samples against G0.  Measure `R0` again in the same session (on a few
scenarios, into a separate directory) to check that the host has not
drifted since the published runs.

### 4.5 Publishing

```bash
python3 benchmarks/kernel/publish.py                 # writes BENCHMARK_RESULTS.md
python3 benchmarks/kernel/publish.py --check         # fails if it is out of date
python3 benchmarks/kernel/publish.py --readme-table  # README headline table
```

`publish.py` computes every table and figure of `BENCHMARK_RESULTS.md` from
the newest results directory (or `--results-dir`), except the host and corpus
description, build flags, and the synthetic-suite section.

## 5. Fairness and Rigor Rules

1. **Identical File Sets**: `.git` directory is explicitly excluded for all recursive scans (`--exclude-dir=.git` for grep and ugrep, `-uu -g '!.git'` for ripgrep). ugrep skips hidden files and directories by default, so its recursive scans also pass `--hidden`, like ripgrep's `-uu`.
2. **Flag Parity**: All tools run with `--color=never`; ripgrep runs with `--no-heading`; line number and count flags are matched 1:1. ugrep arguments are derived from the GNU grep arguments (never from ripgrep's: ugrep's `-N` is a negative pattern, not "no line numbers"); options that mean something else in ugrep (`-z`, `-Z`, `-0`, `-y`, `-u`, `-U`, `-X`, `-W`, `-NUM`) are rejected. ripgrep and ugrep use `-P` for backreferences (`P-01`). The `U-01`/`U-02` out-of-the-box scenarios run `ugrep -r` without `--hidden`.
3. **No `/dev/null` Output**: Standard output is redirected to independent regular files on a `tmpfs` RAM disk (`/dev/shm/kbench_out/`) to prevent GNU grep and ugrep, which both stop at the first match when writing to `/dev/null`, from exiting early.
4. **No Pipe Backpressure**: Invocations do not use Python pipe readers, avoiding 64 KiB pipe buffer distortions during high-volume output.
5. **Environment Hygiene**: A scrubbed minimal environment (`PATH`, `HOME`, `LC_ALL`) is passed to child processes; tool-specific environment variables like `GREP_OPTIONS` or `RIPGREP_CONFIG_PATH` are unset. The `ugrep` command (not `ug`) reads no configuration file. ripgrep and ugrep ignore `LC_ALL` when matching.
6. **Interleaved Rotating Rounds**: Rounds rotate tool execution order (`G0, G1, R0, ...` then `G1, R0, G0, ...`) to eliminate thermal and background bias.
7. **Load Average Gate**: The harness requires 1-minute load average < 2.0 before proceeding to a scenario.
8. **Deterministic Verification**: Output SHA-256 is tracked on every run for hard drop-in equivalence (Gate A).

## 6. Correctness Gates

- **Gate A (Hard)**: G1 byte-exact match against G0 on stdout, normalized stderr, and exit code across every run.
- **Gate B (Soft)**: Normalized match set equivalence of R0, U0, G2, and X1 against G0 (sorted output, stripping binary file notices). Differences explained by documented behavior of the other tool (for example ripgrep and ugrep stripping a UTF-8 BOM) are classified `KNOWN-SEMANTIC` in `kernel_bench.py` and explained in `equivalence.md`.
- **Gate C**: Exit code consistency (0 = match, 1 = no match, 2 = syntax/error).
- **Gate D**: G1 deterministic thread invariance across thread counts `{1, 4, 16, 32, 64, 128}`.

Input read from a pipe is a known exception to byte-exact comparison.
GNU grep checks each read buffer for null bytes and switches to binary
mode from the buffer that holds the first one, and after omitting a
line with an encoding error it prints context that depends on where its
buffers end.  With a pipe, how much each read returns depends on the
pipe capacity and on how fast the writer is, so in these two cases the
output of stock grep itself varies.  The parallel grep assumes a writer
that keeps the pipe full (reads of 96 KiB or the pipe capacity,
whichever is smaller), which is what stock grep sees with a fast
writer.  Counts and file names (`-c`, `-l`, `-L`, `-q`) do not depend on
the buffers and must match in every case.

## 7. Reports and Artifacts

Every benchmark run writes:
- `raw.jsonl`: Complete per-run execution log with rusage, timings, and cryptographic hashes.
- `env.json`: Environment metadata (CPU model, NUMA layout, cache sizes, git SHAs, binary versions) with all local paths scrubbed.
- `summary.md`: Human-readable summary tables, headline speedups, and hypothesis scorecard.
- `scaling.csv`: Machine-readable thread scaling metrics.
- `equivalence.md`: Full Gate A–D validation record.
