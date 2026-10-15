<!-- Copyright (C) 2026 David Lin <dtwlin@gmail.com> -->
# Linux Kernel-Tree 3-Way Benchmark

Reproducible, high-fidelity benchmark harness comparing multi-threaded GNU `grep` against stock upstream GNU `grep`, `ripgrep`, distro GNU `grep`, and `git grep` on the Linux kernel codebase.

## 1. Overview and Contestants

| Contestant | Binary | Provenance | Parallelism | Notes |
| :--- | :--- | :--- | :---: | :--- |
| **G0** | `benchmarks/grep-baseline` | Upstream GNU grep `v3.12-41-ge187ca5` | 1 thread | Baseline reference built with identical flags (`-O3 -march=native`) |
| **G1** | `src/grep` | Multi-threaded GNU grep fork (`master`) | Auto (32) | 128 MiB slab arena, lockless SIMD scanners, intra-file chunking, Sequence Reorder Buffer |
| **R0** | `/usr/bin/rg` | ripgrep 15.1.0 | Auto (12) | Stock Debian package (`+pcre2`, runtime AVX2) |
| **G2** | `/usr/bin/grep` | Distro GNU grep 3.12 | 1 thread | System release binary (Debian generic build) |
| **X1** | `/usr/bin/git` | Git grep (`git grep`) | Auto (128) | Searches tracked files in working tree |

## 2. Directory Structure

```text
benchmarks/kernel/
├── README.md                 # Benchmark guide and fairness documentation
├── kernel_bench.py           # 3-way runner -> results/<timestamp>/raw.jsonl + env.json
├── scenarios.py              # Complete scenario matrix (T, B, M, L, X, U, P)
├── gen_kernel_corpus.py      # Generates $KBENCH_DIR/{linux-all.c, big-24M.h, files.z}
├── gen_patterns.py           # Generates patterns/sym_{100,1000,10000}.txt and regex variants
├── evict_cache.py            # posix_fadvise(DONTNEED) page cache eviction utility
├── report.py                 # Generates summary.md, scaling.csv, equivalence.md
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
    --tools G0,G1,R0,G2,X1 \
    --categories T,B \
    --threads-sweep 1,4,16,32,64,128 \
    --rounds 5
```

## 5. Fairness and Rigor Rules

1. **Identical File Sets**: `.git` directory is explicitly excluded for all recursive scans (`--exclude-dir=.git` for grep, `-uu -g '!.git'` for ripgrep).
2. **Flag Parity**: All tools run with `--color=never`; ripgrep runs with `--no-heading`; line number and count flags are matched 1:1.
3. **No `/dev/null` Output**: Standard output is redirected to independent regular files on a `tmpfs` RAM disk (`/dev/shm/kbench_out/`) to prevent GNU grep from triggering `dev_null_output` early exits.
4. **No Pipe Backpressure**: Invocations do not use Python pipe readers, avoiding 64 KiB pipe buffer distortions during high-volume output.
5. **Environment Hygiene**: A scrubbed minimal environment (`PATH`, `HOME`, `LC_ALL`) is passed to child processes; tool-specific environment variables like `GREP_OPTIONS` or `RIPGREP_CONFIG_PATH` are unset.
6. **Interleaved Rotating Rounds**: Rounds rotate tool execution order (`G0, G1, R0, ...` then `G1, R0, G0, ...`) to eliminate thermal and background bias.
7. **Load Average Gate**: The harness requires 1-minute load average < 2.0 before proceeding to a scenario.
8. **Deterministic Verification**: Output SHA-256 is tracked on every run for hard drop-in equivalence (Gate A).

## 6. Correctness Gates

- **Gate A (Hard)**: G1 byte-exact match against G0 on stdout, normalized stderr, and exit code across every run.
- **Gate B (Soft)**: Normalized match set equivalence against G0 (sorted output, stripping binary file notices).
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
