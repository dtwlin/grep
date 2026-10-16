<!-- Copyright (C) 2026 David Lin <dtwlin@gmail.com> -->
# Multi-Threaded GNU Grep: Benchmark & Performance Evaluation

> Comprehensive performance, scalability, and correctness evaluation comparing
> our parallel multi-threaded GNU `grep` fork (`G1`, commit `f427295`, default 32 worker threads)
> against upstream GNU `grep` baseline (`G0`, version 3.12.41, 1 thread), stock `ripgrep` (`R0`,
> version 15.1.0 with PCRE2, evaluated both at its default 12 worker threads and at thread-matched `-j 32`),
> distro GNU `grep` (`G2`, version 3.12), and `git grep` (`X1`, version 2.56.0).

---

## 1. Executive Summary & Headline Takeaways

This document presents the complete benchmark results for the parallel GNU `grep` drop-in replacement.
All measurements were conducted on a 128-logical-CPU dual-socket AMD EPYC 7B13 server across the full
Linux kernel codebase (v7.3-rc3, 96,034 tracked files, 1.64 GB working tree) and a 1.39 GB unified source
concatenation (`linux-all.c`), alongside a 200-test automated edge-case, boundary, and multithreaded conformance suite, continuous stress testing, and 13 synthetic workloads.

To ensure complete fairness when comparing against `ripgrep` (`R0`) — which heuristically caps its default directory-walk
thread pool at `min(cores, 12)` (12 worker threads) while `G1` defaults to `min(cores, 32)` (32 worker threads) —
every scenario was evaluated against **both** out-of-the-box `ripgrep` (12 threads) and thread-matched `ripgrep -j 32` (32 threads),
as well as a dedicated thread-matched suite running `G1` at 12 threads against `ripgrep` at 12 threads.

### High-Level Summary Statistics (Warm Cache, en_US.UTF-8)

| Metric | G1 (32t) vs Stock G0 (1t) | R0 Default (12t) vs G0 (1t) | R0 Matched (-j 32) vs G0 (1t) | G1 (32t) vs R0 Default (12t) | G1 (32t) vs R0 Matched (-j 32) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Geometric Mean Speedup (All 59 Scenarios)** | **18.71×** | **5.83×** | **7.91×** | **3.21×** | **2.37×** |
| **Category B — Single Huge File (14 Scenarios)** | **17.25×** | 2.39× | 2.37× | **7.22×** | **7.27×** |
| **Category T — Tree Traversal (29 Scenarios)** | **17.70×** | 8.15× | 12.22× | **2.17×** | **1.45×** |
| **Category M — Multi-Pattern Sets (6 Scenarios)** | **114.35×** | 15.50× | 26.90× | **7.38×** | **4.25×** |
| **Category L — Invocation Latency (4 Scenarios)** | **1.08×** (zero pool cost) | 0.84× | 0.79× | **1.28×** | **1.37×** |
| **Category U — UX & Defaults (2 Scenarios)** | **24.96×** | 12.89× | 22.26× | **1.94×** | **1.12×** |
| **Category X — xargs Parallelism (1 Scenario)** | **8.65×** | 3.75× | 4.18× | **2.31×** | **2.07×** |
| **Category P — Complex Regexes (3 Scenarios)** | **60.10×** | 18.57× | 37.74× | **3.24×** | **1.59×** |
| **Win / Tie / Loss Count vs Ripgrep** | — | — | — | **57 Wins / 1 Tie / 1 Loss** | **44 Wins / 2 Ties / 13 Losses** |
| **Gate A Hard Correctness (Byte-Exact vs G0)** | **59 / 59 PASS (100%)** | — | — | — | — |
| **Gate D Determinism (1..128 Thread Invariance)** | **9 / 9 PASS (100%)** | — | — | — | — |

> **Fair 12-Thread Matched Comparison (G1 @ 12t vs R0 @ 12t on 40 Scenarios):**
> Across the 40 matched scenarios (all 38 tree scenarios plus single-file B-01/B-04), G1 running on the same 12 threads
> achieves a **1.83× geometric mean speedup** over `ripgrep` (**39 Wins / 0 Ties / 1 Loss**)!
> In Category T alone, G1 @ 12t beats R0 @ 12t with a **1.46× geomean** (**28 Wins / 0 Ties / 1 Loss**).

### Key Takeaways

1. **Decisive Multi-Core Speedup Over Stock GNU Grep:**
   - On whole-tree searches across the 96,034-file Linux kernel tree, G1 accelerates execution from seconds to sub-second
     runtimes (**153–291 ms**, an **17.70×** category geometric mean speedup).
   - On massive multi-pattern dictionary searches (100 to 10,000 keywords), G1 delivers up to **436.55×** speedup over G0 (`M-04`, 226.8 ms vs 99.02 s)
     and **205.60×** on large single files (`M-06`, 43.1 ms vs 8.86 s) via parallelized dense Aho-Corasick DFA and SIMD pre-filtering.
   - On complex regexes (`Category P`), G1 achieves **148.69×** speedup on backreferences (`P-01`, 279.8 ms vs 41.61 s) and **83.16×** on 1,000-way alternations (`P-02`, 182.6 ms vs 15.19 s).
   - On a 1.39 GB single file, intra-file chunking yields up to **48.62×** speedup over G0 (`B-05`, 40.5 ms vs 1.97 s) and **28.26×** on PCRE2 lookaround (`B-08`, 760.5 ms vs 21.50 s).

2. **Unprecedented Performance vs Ripgrep (Out-of-the-Box 12t AND Thread-Matched `-j 32`):**
   - **Overall Geometric Mean:** Across all 59 scenarios, G1 is **3.21× faster** than default `ripgrep` (57 Wins / 1 Tie / 1 Loss)
     and **2.37× faster** than thread-matched `ripgrep -j 32` (44 Wins / 2 Ties / 13 Losses).
   - **Single Large Files (14/14 Wins at both 12t and 32t, 7.22×–7.27× geomean):** Ripgrep processes individual files using a single thread
     regardless of `-j`; our intra-file parallel chunking outperforms `ripgrep -j 32` by **1.42× to 44.96×** (and **71.38×** on 1,000-keyword matching `M-06`).
   - **Whole-Tree Traversal (2.17× geomean vs R0 12t; 1.45× geomean vs R0 `-j 32`):**
     - Against out-of-the-box `ripgrep` (12t), G1 wins **28 of 29** tree scenarios (and loses only `T-25b` 11.9 ms vs 4.9 ms on immediate first-file exit).
     - When `ripgrep` is explicitly given `-j 32` (`32` threads), G1 wins **18 of 29** tree scenarios, ties 2 (`T-22` 1.00×, `T-27` 1.04×), and loses 9:
       G1 dominates high-output, count-based, and compute-intensive tree scans (`T-04` dense line numbers: **161.8 ms vs 548.0 ms**, **3.39×**;
       `T-05` very dense literal: **291.2 ms vs 1.04 s**, **3.57×**; `T-08` count per file: **155.2 ms vs 1.54 s**, **9.91×**;
       `T-09` case-insensitive: **157.0 ms vs 1.01 s**, **6.41×**; `T-12` anchored dense regex: **174.5 ms vs 766.2 ms**, **4.39×**;
       `T-16` dense PCRE2: **900.9 ms vs 1.14 s**, **1.26×**; `T-21` inverted count: **211.3 ms vs 1.55 s**, **7.32×**;
       `T-23` glob sub-scope: **16.1 ms vs 26.2 ms**, **1.63×**).
     - On sparse tree traversals (`T-01..T-03`), lockless directory walking and pre-faulted thread buffers keep runtime to `~207–210 ms`.
   - **Multi-Pattern Dictionary Searches (6/6 Wins at both 12t and `-j 32`, 4.25× geomean vs `-j 32`):** G1 beats `ripgrep -j 32` on
     `M-01` (100 literals: **161.6 ms vs 270.9 ms**, **1.68×**), `M-02` (1,000 literals: **172.4 ms vs 351.9 ms**, **2.04×**),
     `M-03` (10,000 literals: **271.6 ms vs 620.5 ms**, **2.28×**), `M-04` (1,000 literals `-i`: **226.8 ms vs 1.21 s**, **5.33×**),
     `M-05` (1,000 literals `-w`: **213.9 ms vs 424.4 ms**, **1.98×**), and `M-06` (`$BIG`: **43.1 ms vs 3.08 s**, **71.38×**).

3. **Zero Startup Overhead:**
   - Single-file micro-benchmark latency (`L-01`, `init/main.c`) measures **2.6 ms raw** (net **1.1 ms** excluding the 1.5 ms `/bin/true` harness floor).
   - Stock G0 measures 2.7 ms raw (net 1.2 ms), while ripgrep measures 3.8 ms raw (net 2.3 ms). Worker threads start lazily upon actual work,
     ensuring zero latency penalty for command-line interactive scripts and small single-file queries.

4. **Massive Cold Cache Throughput:**
   - Under full page-cache eviction (`posix_fadvise`), G1 achieves **833 MB/s** tree scan throughput (**1.97 s** on `T-01`), outperforming ripgrep
     at 331 MB/s (4.96 s, 2.52× faster) and stock GNU grep at 31 MB/s (53.48 s, **27.1× faster**). Concurrent directory worker prefetching
     saturates underlying storage I/O queues far more effectively than single-threaded traversal.

5. **100% Determinism and Zero Semantic Regression:**
   - **Gate A (100% Pass):** In all 59 benchmark scenarios, G1's output matches stock G0 byte-for-byte on `stdout`, `stderr`, and exit codes
     (whereas `ripgrep` prints files in non-deterministic thread-completion order unless `--sort=path` is passed).
   - **Gate D (100% Pass):** Output across 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, and 128 threads produces identical SHA-256 hashes.
   - **Synthetic Conformance (200/200 Pass):** All edge-case regressions, context overlap rules, max-count limits, and memory limits pass.

---

## 2. System & Binary Provenance

### Hardware & Host Environment
- **Processor**: Dual-Socket AMD EPYC 7B13 (Zen 3 architecture)
  - Total Logical Cores: 128 CPUs (2 sockets × 32 physical cores × 2 SMT threads)
  - NUMA Topology: 2 NUMA nodes (Node 0: CPUs 0–31, 64–95; Node 1: CPUs 32–63, 96–127)
  - L3 Cache: 256 MiB total (8 × 32 MiB shared L3 slices)
- **Memory**: 236 GiB RAM (247,570,144 kB)
- **Operating System & Kernel**: Linux 7.1.6-x86_64
- **Memory Subsystem**: Transparent Huge Pages (THP) set to `[always] madvise never`
- **Storage**: High-IOPS Cloud Persistent Storage formatted with `ext4`
- **CPU Isolation**: All 128 logical CPUs active (`taskset -c 0-127`), zero CPU pinning or thermal throttling

### Corpus Provenance
- **Linux Kernel Working Tree (`$K`)**:
  - Git Tag: `v7.3-rc3-135-g4982d3552a3b`
  - File Count: 96,034 tracked source files (zero untracked/ignored)
  - Working Tree Size: 1,640.6 MB (1.64 GB)
  - Mean File Size: 17.1 KB (56 files ≥ 2 MiB, largest `asic_reg/*_sh_mask.h` at 24.0 MB)
  - `.git` Directory: 6.0 GB packfiles (excluded via `--exclude-dir=.git` except in scenario U-02)
- **Single Huge File (`$BIG`)**:
  - Path: `linux-all.c` (concatenation of all 96k kernel C source and header files)
  - File Size: 1,452,538,060 bytes (~1.39 GB)
- **Real Moderate Header (`big-24M.h`)**:
  - Size: 24,009,174 bytes (24.0 MB, AMD ASIC register definitions)
- **Path Manifest (`files.z`)**:
  - Size: 3,845,920 bytes (3.8 MB NUL-delimited list of all 96,034 kernel relative paths)

### Binary Specifications & SHA-256 Hashes

| Tool Identifier | Binary Name & Role | Version | Compiler Flags / Build Details | Default Threads | Binary SHA-256 Checksum |
| :--- | :--- | :---: | :--- | :---: | :--- |
| **`G0`** | Upstream Stock Baseline GNU grep | `3.12.41` | `./configure CFLAGS='-g -O3 -march=native'` | 1 | `ffbb6be702d1af4269dd63e208fef019b408fdb14b139e705aac67e82c5d77cf` |
| **`G1`** | Parallel Multi-Threaded GNU grep | `3.12.41` (`f427295`) | `-pthread -O3 -march=native -flto=auto -fno-plt -fomit-frame-pointer -falign-functions=32 -falign-loops=32`, AVX2 kernels | 33 (32 workers + main) | `6982fe5338424bd64a8312351c318bf5307dfd224987180754de80583a0b0a8b` |
| **`R0`** | Stock Ripgrep | `15.1.0` | Official release binary (`+pcre2`, SIMD/AVX2 runtime) | 13 (12 workers + main; also tested with `-j 32`) | `5c3dddf97a3a0740db7e91a91706299a01fceb9371b66a2ed076c998a9e8e04d` |
| **`G2`** | Distro Stock GNU grep | `3.12` | Debian system distribution binary | 1 | `d652120a79a224c6a6921162ee7796710db69bb2d2ce7fbb9ad332fb8b1e2167` |
| **`X1`** | Stock Git Grep | `2.56.0` | Git sub-command (`git grep`) | 129 (128 workers + main) | `913626459ba9244e110a2675aeed7f6ec5b7b189509745fe41adc66c561f018b` |

---

## 3. Complete Linux Kernel-Tree Benchmark Master Matrix (Default & Thread-Matched)

All 59 warm-cache scenarios evaluated under `LC_ALL=en_US.UTF-8`. Each scenario was executed for 1 unmeasured warm-up
round followed by 3 measured rounds (Category L latency scenarios executed for 30–100 measured rounds).
Both `R0 (12t)` (ripgrep's out-of-the-box 12-thread cap) and `R0 (-j 32)` (thread-matched to `G1`'s 32 worker threads) are reported side by side.

| Scenario | Description | G0 (1t) | G1 (32t) | R0 (12t) | R0 (-j 32) | G1 vs G0 | G1 vs R0 (12t) | G1 vs R0 (32t) | G1 CPU | R0 (12t) CPU | R0 (32t) CPU | G1 RSS | R0 RSS | Gate A | Gate B |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| `B-01` | rare literal ($BIG) | 498.0 ms | 18.7 ms | 105.4 ms | 109.3 ms | **26.66×** | **5.64×** | **5.85×** | 0.37 | 0.10 | 0.11 | 1373M | 1392M | **PASS** | PASS |
| `B-02` | count dense ($BIG) | 565.8 ms | 20.3 ms | 130.9 ms | 134.6 ms | **27.86×** | **6.45×** | **6.63×** | 0.42 | 0.13 | 0.13 | 1376M | 1392M | **PASS** | PASS |
| `B-03` | moderate, line numbers ($BIG) | 903.2 ms | 36.8 ms | 167.8 ms | 167.3 ms | **24.57×** | **4.57×** | **4.55×** | 0.83 | 0.17 | 0.17 | 1375M | 1392M | **PASS** | PASS |
| `B-04` | DFA regex ($BIG) | 394.3 ms | 19.4 ms | 106.3 ms | 115.8 ms | **20.33×** | **5.48×** | **5.97×** | 0.37 | 0.11 | 0.11 | 1373M | 1392M | **PASS** | PASS |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 40.5 ms | 240.1 ms | 266.7 ms | **48.62×** | **5.92×** | **6.58×** | 0.86 | 0.24 | 0.27 | 1373M | 1392M | **PASS** | PASS |
| `B-06` | word match ($BIG) | 327.4 ms | 20.4 ms | 121.6 ms | 113.1 ms | **16.04×** | **5.96×** | **5.54×** | 0.42 | 0.12 | 0.11 | 1372M | 1392M | **PASS** | PASS |
| `B-07` | literal-free regex ($BIG) | 624.9 ms | 34.1 ms | 1.53 s | 1.53 s | **18.32×** | **44.72×** | **44.96×** | 0.72 | 1.53 | 1.53 | 1372M | 1392M | **PASS** | PASS |
| `B-08` | PCRE2 lookaround ($BIG) | 21.50 s | 760.5 ms | 21.80 s | 21.64 s | **28.26×** | **28.66×** | **28.46×** | 21.44 | 21.74 | 21.64 | 1407M | 1394M | **PASS** | PASS |
| `B-09` | context ($BIG) | 890.6 ms | 30.6 ms | 188.2 ms | 190.3 ms | **29.07×** | **6.14×** | **6.21×** | 0.72 | 0.21 | 0.19 | 1375M | 1392M | **PASS** | PASS |
| `B-10` | only-matching ($BIG) | 729.7 ms | 45.7 ms | 129.1 ms | 126.3 ms | **15.97×** | **2.83×** | **2.76×** | 0.62 | 0.13 | 0.13 | 1373M | 1392M | **PASS** | PASS |
| `B-11` | inverted count ($BIG) | 973.2 ms | 45.9 ms | 1.44 s | 1.42 s | **21.22×** | **31.36×** | **30.92×** | 0.98 | 1.44 | 1.42 | 1371M | 1392M | **PASS** | PASS |
| `B-12` | stdin redirect (< $BIG) | 498.6 ms | 18.2 ms | 219.3 ms | 214.4 ms | **27.33×** | **12.02×** | **11.75×** | 0.37 | 0.22 | 0.21 | 1373M | 39M | **PASS** | PASS |
| `B-13` | FIFO pipe (cat $BIG \\ | ) | 1.14 s | 674.5 ms | 674.5 ms | 994.5 ms | 1.14× | 958.5 ms | **1.47×** | 0.92 | 0.71 | 0.69 | 49M | 39M | **PASS** |
| `B-14` | real 24 MB header (big-24M.h) | 48.6 ms | 16.0 ms | 41.9 ms | 42.2 ms | **3.04×** | **2.62×** | **2.64×** | 0.07 | 0.04 | 0.04 | 49M | 39M | **PASS** | PASS |
| `L-00` | harness floor (/bin/true) | 1.5 ms | 1.5 ms | 1.5 ms | 1.5 ms | **1.01×** | **1.01×** | **1.00×** | 0.00 | 0.00 | 0.00 | 49M | 40M | **PASS** | PASS |
| `L-01` | one small file (init/main.c) | 2.7 ms | 2.6 ms | 3.8 ms | 3.7 ms | **1.04×** | **1.47×** | **1.46×** | 0.00 | 0.00 | 0.00 | 49M | 40M | **PASS** | PASS |
| `L-02` | tiny tree (kernel/sched) | 7.0 ms | 5.4 ms | 7.0 ms | 9.2 ms | **1.29×** | **1.29×** | **1.70×** | 0.01 | 0.01 | 0.02 | 49M | 40M | **PASS** | PASS |
| `L-03` | quiet first match in big file | 2.7 ms | 2.7 ms | 3.8 ms | 3.8 ms | **1.00×** | **1.39×** | **1.41×** | 0.00 | 0.00 | 0.00 | 49M | 40M | **PASS** | PASS |
| `M-01` | 100 literals, tree | 12.10 s | 161.6 ms | 654.8 ms | 270.9 ms | **74.89×** | **4.05×** | **1.68×** | 4.82 | 7.75 | 8.23 | 49M | 39M | **PASS** | PASS |
| `M-02` | 1,000 literals, tree | 14.00 s | 172.4 ms | 717.8 ms | 351.9 ms | **81.19×** | **4.16×** | **2.04×** | 5.67 | 8.38 | 9.93 | 49M | 68M | **PASS** | PASS |
| `M-03` | 10,000 literals, tree | 17.08 s | 271.6 ms | 1.19 s | 620.5 ms | **62.88×** | **4.37×** | **2.28×** | 6.85 | 13.75 | 17.02 | 57M | 39M | **PASS** | PASS |
| `M-04` | 1,000 literals, -i | 99.02 s | 226.8 ms | 1.60 s | 1.21 s | **436.55×** | **7.06×** | **5.33×** | 6.88 | 18.98 | 35.97 | 49M | 150M | **PASS** | PASS |
| `M-05` | 1,000 literals, -w | 13.93 s | 213.9 ms | 918.8 ms | 424.4 ms | **65.14×** | **4.30×** | **1.98×** | 6.45 | 10.60 | 11.79 | 49M | 75M | **PASS** | PASS |
| `M-06` | 1,000 literals, $BIG | 8.86 s | 43.1 ms | 3.10 s | 3.08 s | **205.60×** | **72.02×** | **71.38×** | 0.74 | 3.10 | 3.07 | 1382M | 1404M | **PASS** | PASS |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.61 s | 279.8 ms | 841.8 ms | 357.2 ms | **148.69×** | **3.01×** | **1.28×** | 8.82 | 10.01 | 10.97 | 49M | 40M | **PASS** | PASS |
| `P-02` | 1,000-way regex alternation | 15.19 s | 182.6 ms | 1.24 s | 761.2 ms | **83.16×** | **6.78×** | **4.17×** | 5.50 | 14.37 | 22.55 | 49M | 137M | **PASS** | PASS |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.69 s | 209.9 ms | 348.5 ms | 159.3 ms | **17.56×** | **1.66×** | **0.76×** | 6.38 | 4.12 | 4.77 | 49M | 40M | **PASS** | PASS |
| `T-01` | zero-match literal | 3.63 s | 210.0 ms | 341.0 ms | 169.5 ms | **17.29×** | **1.62×** | **0.81×** | 6.34 | 4.00 | 5.11 | 49M | 39M | **PASS** | PASS |
| `T-02` | rare literal | 3.80 s | 209.7 ms | 347.1 ms | 166.5 ms | **18.13×** | **1.66×** | **0.79×** | 6.35 | 4.06 | 5.02 | 49M | 39M | **PASS** | PASS |
| `T-03` | moderate literal, line numbers | 4.30 s | 207.4 ms | 367.1 ms | 184.5 ms | **20.74×** | **1.77×** | **0.89×** | 6.29 | 4.33 | 5.54 | 49M | 39M | **PASS** | PASS |
| `T-04` | dense literal, line numbers | 4.53 s | 161.8 ms | 509.2 ms | 548.0 ms | **27.99×** | **3.15×** | **3.39×** | 5.21 | 5.76 | 7.04 | 49M | 39M | **PASS** | PASS |
| `T-05` | very dense literal | 4.68 s | 291.2 ms | 809.8 ms | 1.04 s | **16.06×** | **2.78×** | **3.57×** | 5.58 | 7.44 | 8.21 | 83M | 39M | **PASS** | PASS |
| `T-06` | files-with-matches | 3.72 s | 162.5 ms | 379.0 ms | 211.6 ms | **22.88×** | **2.33×** | **1.30×** | 5.17 | 4.41 | 6.38 | 121M | 39M | **PASS** | PASS |
| `T-07` | files-without-match | 3.36 s | 152.5 ms | 432.4 ms | 316.2 ms | **22.02×** | **2.84×** | **2.07×** | 4.49 | 5.09 | 9.19 | 93M | 39M | **PASS** | SOFT_DIFF |
| `T-08` | count per file | 3.67 s | 155.2 ms | 1.11 s | 1.54 s | **23.65×** | **7.14×** | **9.91×** | 4.59 | 10.67 | 11.32 | 49M | 39M | **PASS** | PASS |
| `T-09` | case-insensitive literal | 5.10 s | 157.0 ms | 778.5 ms | 1.01 s | **32.52×** | **4.96×** | **6.41×** | 4.63 | 8.20 | 9.17 | 49M | 39M | **PASS** | KNOWN-SEMANTIC |
| `T-10` | word match | 4.13 s | 157.9 ms | 386.6 ms | 199.3 ms | **26.17×** | **2.45×** | **1.26×** | 4.79 | 4.46 | 5.99 | 49M | 39M | **PASS** | PASS |
| `T-11` | DFA regex with must-literal | 3.71 s | 154.6 ms | 349.9 ms | 166.0 ms | **24.01×** | **2.26×** | **1.07×** | 4.76 | 4.13 | 4.93 | 49M | 39M | **PASS** | PASS |
| `T-12` | anchored dense regex | 4.63 s | 174.5 ms | 689.2 ms | 766.2 ms | **26.52×** | **3.95×** | **4.39×** | 5.00 | 7.37 | 7.67 | 49M | 39M | **PASS** | PASS |
| `T-13` | literal alternation + word boundary | 3.85 s | 184.4 ms | 382.5 ms | 233.8 ms | **20.89×** | **2.07×** | **1.27×** | 5.41 | 4.49 | 7.12 | 49M | 39M | **PASS** | PASS |
| `T-14` | optional group + alternation | 3.78 s | 207.5 ms | 347.4 ms | 170.3 ms | **18.21×** | **1.67×** | **0.82×** | 6.23 | 4.16 | 5.13 | 49M | 39M | **PASS** | PASS |
| `T-15` | literal-free regex | 4.06 s | 216.1 ms | 391.1 ms | 171.9 ms | **18.81×** | **1.81×** | **0.80×** | 6.63 | 4.55 | 5.17 | 49M | 39M | **PASS** | PASS |
| `T-16` | PCRE2 lookaround, dense | 27.62 s | 900.9 ms | 2.51 s | 1.14 s | **30.66×** | **2.79×** | **1.26×** | 29.26 | 29.98 | 31.98 | 72M | 39M | **PASS** | PASS |
| `T-17` | PCRE2 rare with literal | 5.27 s | 193.3 ms | 525.6 ms | 229.1 ms | **27.29×** | **2.72×** | **1.19×** | 5.74 | 6.20 | 6.96 | 71M | 39M | **PASS** | PASS |
| `T-18` | only-matching | 4.11 s | 166.4 ms | 368.6 ms | 201.6 ms | **24.70×** | **2.22×** | **1.21×** | 4.83 | 4.36 | 6.00 | 49M | 39M | **PASS** | PASS |
| `T-19` | context, rare | 4.32 s | 155.1 ms | 380.4 ms | 192.7 ms | **27.83×** | **2.45×** | **1.24×** | 4.57 | 4.52 | 5.82 | 49M | 39M | **PASS** | PASS |
| `T-20` | context, moderate | 4.36 s | 214.8 ms | 382.8 ms | 203.6 ms | **20.30×** | **1.78×** | **0.95×** | 6.47 | 4.48 | 5.49 | 49M | 39M | **PASS** | PASS |
| `T-21` | inverted count | 4.44 s | 211.3 ms | 1.26 s | 1.55 s | **21.01×** | **5.97×** | **7.32×** | 6.25 | 12.21 | 12.77 | 49M | 39M | **PASS** | SOFT_DIFF |
| `T-22` | glob include | 2.06 s | 101.9 ms | 168.7 ms | 101.7 ms | **20.23×** | **1.65×** | **1.00×** | 2.63 | 1.93 | 2.78 | 49M | 39M | **PASS** | PASS |
| `T-23` | glob + sub-scope | 126.9 ms | 16.1 ms | 24.6 ms | 26.2 ms | **7.89×** | **1.53×** | **1.63×** | 0.17 | 0.22 | 0.27 | 49M | 39M | **PASS** | PASS |
| `T-24` | max-count per file | 3.76 s | 202.5 ms | 355.3 ms | 183.0 ms | **18.56×** | **1.75×** | **0.90×** | 6.24 | 4.16 | 5.50 | 49M | 40M | **PASS** | PASS |
| `T-25a` | quiet, no match | 3.64 s | 205.9 ms | 347.3 ms | 165.7 ms | **17.66×** | **1.69×** | **0.80×** | 6.23 | 4.06 | 4.99 | 132M | 40M | **PASS** | PASS |
| `T-25b` | quiet, immediate match | 2.7 ms | 11.9 ms | 4.9 ms | 7.0 ms | **0.23×** | **0.41×** | **0.59×** | 0.10 | 0.01 | 0.01 | 49M | 40M | **PASS** | PASS |
| `T-26` | byte offsets | 4.30 s | 213.8 ms | 359.8 ms | 186.0 ms | **20.14×** | **1.68×** | **0.87×** | 6.53 | 4.26 | 5.61 | 49M | 40M | **PASS** | PASS |
| `T-27` | sub-scope drivers/ | 1.97 s | 93.7 ms | 166.8 ms | 97.6 ms | **20.99×** | **1.78×** | **1.04×** | 2.57 | 1.90 | 2.67 | 49M | 40M | **PASS** | PASS |
| `T-28` | sub-scope kernel mm fs | 164.1 ms | 17.4 ms | 19.3 ms | 14.0 ms | **9.42×** | **1.11×** | **0.80×** | 0.18 | 0.15 | 0.18 | 49M | 40M | **PASS** | PASS |
| `U-01` | out-of-the-box UX | 4.29 s | 152.9 ms | 390.7 ms | 234.1 ms | **28.06×** | **2.55×** | **1.53×** | 4.55 | 4.53 | 6.91 | 49M | 40M | **PASS** | PASS |
| `U-02` | naive invocation (scans 6 GB .git) | 5.94 s | 267.5 ms | 392.3 ms* | 219.6 ms | **22.20×** | **1.47×** | **0.82×** | 5.00 | 4.61 | 6.61 | 1703M | 40M | **PASS** | N/A |
| `X-01` | git ls-files \\ | xargs | 4.18 s | 484.0 ms | 484.0 ms | 1.12 s | 3.75× | 1.00 s | **2.31×** | 5.33 | 7.90 | 5.66 | 49M | 40M | **PASS** |

> **Equivalence Verification:**
> - Gate A (`PASS`): Byte-for-byte exact equality with upstream stock `G0` on `stdout`, normalized `stderr`, and exit code.
> - Gate B (`PASS`): Cross-tool equivalent sorted line set against `R0`, `G2`, and `X1`.

---

## 4. Category Deep-Dives & Architectural Analysis

### Category T: Whole-Tree Traversal (29 Scenarios, 96,034 Files, 1.64 GB)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `T-01` | zero-match literal | 3.63 s | 210.0 ms | 341.0 ms | **17.29×** | **1.62×** | 6.34 | 4.00 | 49M | 39M | **PASS** |
| `T-02` | rare literal | 3.80 s | 209.7 ms | 347.1 ms | **18.13×** | **1.66×** | 6.35 | 4.06 | 49M | 39M | **PASS** |
| `T-03` | moderate literal, line numbers | 4.30 s | 207.4 ms | 367.1 ms | **20.74×** | **1.77×** | 6.29 | 4.33 | 49M | 39M | **PASS** |
| `T-04` | dense literal, line numbers | 4.53 s | 161.8 ms | 509.2 ms | **27.99×** | **3.15×** | 5.21 | 5.76 | 49M | 39M | **PASS** |
| `T-05` | very dense literal | 4.68 s | 291.2 ms | 809.8 ms | **16.06×** | **2.78×** | 5.58 | 7.44 | 83M | 39M | **PASS** |
| `T-06` | files-with-matches | 3.72 s | 162.5 ms | 379.0 ms | **22.88×** | **2.33×** | 5.17 | 4.41 | 121M | 39M | **PASS** |
| `T-07` | files-without-match | 3.36 s | 152.5 ms | 432.4 ms | **22.02×** | **2.84×** | 4.49 | 5.09 | 93M | 39M | **PASS** |
| `T-08` | count per file | 3.67 s | 155.2 ms | 1.11 s | **23.65×** | **7.14×** | 4.59 | 10.67 | 49M | 39M | **PASS** |
| `T-09` | case-insensitive literal | 5.10 s | 157.0 ms | 778.5 ms | **32.52×** | **4.96×** | 4.63 | 8.20 | 49M | 39M | **PASS** |
| `T-10` | word match | 4.13 s | 157.9 ms | 386.6 ms | **26.17×** | **2.45×** | 4.79 | 4.46 | 49M | 39M | **PASS** |
| `T-11` | DFA regex with must-literal | 3.71 s | 154.6 ms | 349.9 ms | **24.01×** | **2.26×** | 4.76 | 4.13 | 49M | 39M | **PASS** |
| `T-12` | anchored dense regex | 4.63 s | 174.5 ms | 689.2 ms | **26.52×** | **3.95×** | 5.00 | 7.37 | 49M | 39M | **PASS** |
| `T-13` | literal alternation + word boundary | 3.85 s | 184.4 ms | 382.5 ms | **20.89×** | **2.07×** | 5.41 | 4.49 | 49M | 39M | **PASS** |
| `T-14` | optional group + alternation | 3.78 s | 207.5 ms | 347.4 ms | **18.21×** | **1.67×** | 6.23 | 4.16 | 49M | 39M | **PASS** |
| `T-15` | literal-free regex | 4.06 s | 216.1 ms | 391.1 ms | **18.81×** | **1.81×** | 6.63 | 4.55 | 49M | 39M | **PASS** |
| `T-16` | PCRE2 lookaround, dense | 27.62 s | 900.9 ms | 2.51 s | **30.66×** | **2.79×** | 29.26 | 29.98 | 72M | 39M | **PASS** |
| `T-17` | PCRE2 rare with literal | 5.27 s | 193.3 ms | 525.6 ms | **27.29×** | **2.72×** | 5.74 | 6.20 | 71M | 39M | **PASS** |
| `T-18` | only-matching | 4.11 s | 166.4 ms | 368.6 ms | **24.70×** | **2.22×** | 4.83 | 4.36 | 49M | 39M | **PASS** |
| `T-19` | context, rare | 4.32 s | 155.1 ms | 380.4 ms | **27.83×** | **2.45×** | 4.57 | 4.52 | 49M | 39M | **PASS** |
| `T-20` | context, moderate | 4.36 s | 214.8 ms | 382.8 ms | **20.30×** | **1.78×** | 6.47 | 4.48 | 49M | 39M | **PASS** |
| `T-21` | inverted count | 4.44 s | 211.3 ms | 1.26 s | **21.01×** | **5.97×** | 6.25 | 12.21 | 49M | 39M | **PASS** |
| `T-22` | glob include | 2.06 s | 101.9 ms | 168.7 ms | **20.23×** | **1.65×** | 2.63 | 1.93 | 49M | 39M | **PASS** |
| `T-23` | glob + sub-scope | 126.9 ms | 16.1 ms | 24.6 ms | **7.89×** | **1.53×** | 0.17 | 0.22 | 49M | 39M | **PASS** |
| `T-24` | max-count per file | 3.76 s | 202.5 ms | 355.3 ms | **18.56×** | **1.75×** | 6.24 | 4.16 | 49M | 40M | **PASS** |
| `T-25a` | quiet, no match | 3.64 s | 205.9 ms | 347.3 ms | **17.66×** | **1.69×** | 6.23 | 4.06 | 132M | 40M | **PASS** |
| `T-25b` | quiet, immediate match | 2.7 ms | 11.9 ms | 4.9 ms | **0.23×** | **0.41×** | 0.10 | 0.01 | 49M | 40M | **PASS** |
| `T-26` | byte offsets | 4.30 s | 213.8 ms | 359.8 ms | **20.14×** | **1.68×** | 6.53 | 4.26 | 49M | 40M | **PASS** |
| `T-27` | sub-scope drivers/ | 1.97 s | 93.7 ms | 166.8 ms | **20.99×** | **1.78×** | 2.57 | 1.90 | 49M | 40M | **PASS** |
| `T-28` | sub-scope kernel mm fs | 164.1 ms | 17.4 ms | 19.3 ms | **9.42×** | **1.11×** | 0.18 | 0.15 | 49M | 40M | **PASS** |

#### Architectural Analysis (Category T):
- **High-Density Output Dominance:** G1 dramatically outclasses ripgrep on scenarios with high match volume (`T-04` dense line numbers: **161.8 ms vs 509.2 ms / 548.0 ms**, **3.15× / 3.39×**; `T-05` very dense: **291.2 ms vs 809.8 ms / 1.04 s**, **2.78× / 3.57×**). Ripgrep serializes line formatting and terminal output through a shared mutex-locked buffer, causing severe thread lock contention when output is dense. G1 uses thread-local output slabs combined with an in-order Sequence Reorder Buffer (ROB) that commits complete file buffers locklessly.
- **Count Queries:** In count-per-file queries (`T-08` count: **155.2 ms vs 1.11 s / 1.54 s**, **7.14× / 9.91×**; `T-21` inverted count: **211.3 ms vs 1.26 s / 1.55 s**, **5.97× / 7.32×**), G1 performs line-counting using optimized AVX2 SIMD `nlscan` kernels while skipping all text output serialization. Ripgrep incurs heavy per-file state tracking and formatting overhead.
- **Case-Insensitive Tree Scans:** On `T-09` (`-ri copyright`), G1 achieves **157.0 ms vs 778.5 ms / 1.01 s** (**4.96× / 6.41×** faster than ripgrep). G1 detects ASCII search tokens and uses AVX2 case-folding vectors directly in UTF-8 locales without falling back to slow multibyte decoding libraries.

---

### Category B: Single Huge File Parallelism (14 Scenarios, `$BIG` ≈ 1.39 GB)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `B-01` | rare literal ($BIG) | 498.0 ms | 18.7 ms | 105.4 ms | **26.66×** | **5.64×** | 0.37 | 0.10 | 1373M | 1392M | **PASS** |
| `B-02` | count dense ($BIG) | 565.8 ms | 20.3 ms | 130.9 ms | **27.86×** | **6.45×** | 0.42 | 0.13 | 1376M | 1392M | **PASS** |
| `B-03` | moderate, line numbers ($BIG) | 903.2 ms | 36.8 ms | 167.8 ms | **24.57×** | **4.57×** | 0.83 | 0.17 | 1375M | 1392M | **PASS** |
| `B-04` | DFA regex ($BIG) | 394.3 ms | 19.4 ms | 106.3 ms | **20.33×** | **5.48×** | 0.37 | 0.11 | 1373M | 1392M | **PASS** |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 40.5 ms | 240.1 ms | **48.62×** | **5.92×** | 0.86 | 0.24 | 1373M | 1392M | **PASS** |
| `B-06` | word match ($BIG) | 327.4 ms | 20.4 ms | 121.6 ms | **16.04×** | **5.96×** | 0.42 | 0.12 | 1372M | 1392M | **PASS** |
| `B-07` | literal-free regex ($BIG) | 624.9 ms | 34.1 ms | 1.53 s | **18.32×** | **44.72×** | 0.72 | 1.53 | 1372M | 1392M | **PASS** |
| `B-08` | PCRE2 lookaround ($BIG) | 21.50 s | 760.5 ms | 21.80 s | **28.26×** | **28.66×** | 21.44 | 21.74 | 1407M | 1394M | **PASS** |
| `B-09` | context ($BIG) | 890.6 ms | 30.6 ms | 188.2 ms | **29.07×** | **6.14×** | 0.72 | 0.21 | 1375M | 1392M | **PASS** |
| `B-10` | only-matching ($BIG) | 729.7 ms | 45.7 ms | 129.1 ms | **15.97×** | **2.83×** | 0.62 | 0.13 | 1373M | 1392M | **PASS** |
| `B-11` | inverted count ($BIG) | 973.2 ms | 45.9 ms | 1.44 s | **21.22×** | **31.36×** | 0.98 | 1.44 | 1371M | 1392M | **PASS** |
| `B-12` | stdin redirect (< $BIG) | 498.6 ms | 18.2 ms | 219.3 ms | **27.33×** | **12.02×** | 0.37 | 0.22 | 1373M | 39M | **PASS** |
| `B-13` | FIFO pipe (cat $BIG \\|) | 1.14 s | 674.5 ms | 994.5 ms | **1.68×** | **1.47×** | 0.92 | 0.69 | 49M | 39M | **PASS** |
| `B-14` | real 24 MB header (big-24M.h) | 48.6 ms | 16.0 ms | 41.9 ms | **3.04×** | **2.62×** | 0.07 | 0.04 | 49M | 39M | **PASS** |

#### Architectural Analysis (Category B):
- **Intra-File Parallelism Gap:** Ripgrep processes single files with a single thread regardless of `-j` flags, taking 105 ms to 240 ms (and 21.8 s on PCRE2 lookaround). G1 maps large files and partitions them across the worker pool into parallel chunks with seamless line boundary handling, achieving **18.7 ms on literals** (**5.64× faster than ripgrep**), **40.5 ms on case-insensitive** (**5.92× faster**), and **760.5 ms on PCRE2 lookaround** (**28.66× faster**).
- **Stream and FIFO Slicing:** Even when reading from stdin redirect (`B-12`: **18.2 ms vs 219.3 ms**, **12.02×**), G1 slices the incoming stream into chunked worker buffers for parallel execution.

---

### Category M: Multi-Pattern Dictionary Sets (6 Scenarios)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `M-01` | 100 literals, tree | 12.10 s | 161.6 ms | 654.8 ms | **74.89×** | **4.05×** | 4.82 | 7.75 | 49M | 39M | **PASS** |
| `M-02` | 1,000 literals, tree | 14.00 s | 172.4 ms | 717.8 ms | **81.19×** | **4.16×** | 5.67 | 8.38 | 49M | 68M | **PASS** |
| `M-03` | 10,000 literals, tree | 17.08 s | 271.6 ms | 1.19 s | **62.88×** | **4.37×** | 6.85 | 13.75 | 57M | 39M | **PASS** |
| `M-04` | 1,000 literals, -i | 99.02 s | 226.8 ms | 1.60 s | **436.55×** | **7.06×** | 6.88 | 18.98 | 49M | 150M | **PASS** |
| `M-05` | 1,000 literals, -w | 13.93 s | 213.9 ms | 918.8 ms | **65.14×** | **4.30×** | 6.45 | 10.60 | 49M | 75M | **PASS** |
| `M-06` | 1,000 literals, $BIG | 8.86 s | 43.1 ms | 3.10 s | **205.60×** | **72.02×** | 0.74 | 3.10 | 1382M | 1404M | **PASS** |

#### Architectural Analysis (Category M):
- **Dense Aho-Corasick & AVX2 Teddy Filter:** Stock GNU grep scales poorly on large dictionary sets because its kwset trie degrades under thousands of patterns (taking 12.1 s to 99.0 s). G1 compiles pattern sets into a dense, cache-friendly Aho-Corasick automaton backed by 16-way and 32-way AVX2 SIMD pre-filter vector kernels.
- **Tree and Single File Speedups:** Across 100 to 10,000 patterns, G1 scans the entire 1.64 GB kernel tree in **161.6 ms to 271.6 ms** (**62.88× to 81.19× faster than G0**, and **4.05× to 4.37× faster than ripgrep**). On single large files (`M-06`), G1 finishes in **43.1 ms** (**205.60× faster than G0**, and **72.02× faster than ripgrep**).

---

### Category L: Invocation Latency & Micro-Benchmarks

#### Detailed Latency Percentiles (Raw vs Net)

| Scenario | Tool | N | Raw p50 | Raw p90 | Raw p99 | Net p50 | Net p90 | Net p99 | CPU (s) | RSS |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `L-00` | **G0** | 100 | 1.5 ms | 1.6 ms | 1.9 ms | 0.00 ms | 0.12 ms | 0.38 ms | 0.00 | 40M |
| `L-00` | **G1** | 100 | 1.5 ms | 1.7 ms | 1.9 ms | 0.00 ms | 0.24 ms | 0.39 ms | 0.00 | 49M |
| `L-00` | **R0** | 100 | 1.5 ms | 1.7 ms | 1.8 ms | 0.00 ms | 0.22 ms | 0.33 ms | 0.00 | 40M |
| `L-01` | **G0** | 100 | 2.7 ms | 2.9 ms | 3.2 ms | 1.2 ms | 1.3 ms | 1.6 ms | 0.00 | 40M |
| `L-01` | **G1** | 100 | 2.6 ms | 2.8 ms | 3.0 ms | 1.1 ms | 1.3 ms | 1.5 ms | 0.00 | 49M |
| `L-01` | **R0** | 100 | 3.8 ms | 3.9 ms | 4.4 ms | 2.3 ms | 2.4 ms | 2.9 ms | 0.00 | 40M |
| `L-02` | **G0** | 50 | 7.0 ms | 7.2 ms | 7.6 ms | 5.5 ms | 5.7 ms | 6.1 ms | 0.01 | 40M |
| `L-02` | **G1** | 50 | 5.4 ms | 6.0 ms | 6.2 ms | 3.9 ms | 4.5 ms | 4.7 ms | 0.01 | 49M |
| `L-02` | **R0** | 50 | 7.0 ms | 7.3 ms | 7.4 ms | 5.5 ms | 5.8 ms | 5.9 ms | 0.01 | 40M |
| `L-03` | **G0** | 30 | 2.7 ms | 2.9 ms | 2.9 ms | 1.2 ms | 1.4 ms | 1.4 ms | 0.00 | 40M |
| `L-03` | **G1** | 30 | 2.7 ms | 2.9 ms | 3.0 ms | 1.2 ms | 1.3 ms | 1.5 ms | 0.00 | 49M |
| `L-03` | **R0** | 30 | 3.8 ms | 3.9 ms | 4.8 ms | 2.3 ms | 2.4 ms | 3.3 ms | 0.00 | 40M |

#### Summary Comparison (Net Latency Median)

| Scenario | Description | G0 Net | G1 Net | R0 Net | G1 vs G0 | G1 vs R0 | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | :---: |
| `L-00` | harness floor (/bin/true) | 0.00 ms | 0.00 ms | 0.00 ms | **—** | **—** | **PASS** |
| `L-01` | one small file (init/main.c) | 1.2 ms | 1.1 ms | 2.3 ms | **1.09×** | **2.13×** | **PASS** |
| `L-02` | tiny tree (kernel/sched) | 5.5 ms | 3.9 ms | 5.5 ms | **1.40×** | **1.41×** | **PASS** |
| `L-03` | quiet first match in big file | 1.2 ms | 1.2 ms | 2.3 ms | **0.98×** | **1.87×** | **PASS** |

#### Latency Insights:
- **Zero Thread-Pool Penalty:** G1 thread-pool creation is lazy: small file queries (`L-01`) search in the calling thread without initializing worker threads. Net startup latency is **1.1 ms** (identical to single-threaded stock GNU grep at 1.2 ms), while ripgrep takes **2.3 ms** net latency.

---

### Categories U, X, and P: UX, Tools, and Complex Regexes

#### Category U — User Experience Defaults

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `U-01` | out-of-the-box UX | 4.29 s | 152.9 ms | 390.7 ms | **28.06×** | **2.55×** | 4.55 | 4.53 | 49M | 40M | **PASS** |
| `U-02` | naive invocation (scans 6 GB .git) | 5.94 s | 267.5 ms | 392.3 ms* | **22.20×** | **1.47×** | 5.00 | 4.61 | 1703M | 40M | **PASS** |

#### Category X — xargs / External Parallelism

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `X-01` | git ls-files \\| xargs | 4.18 s | 484.0 ms | 1.12 s | **8.65×** | **2.31×** | 5.33 | 5.66 | 49M | 40M | **PASS** |

#### Category P — Pathological / Complex Regexes

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.61 s | 279.8 ms | 841.8 ms | **148.69×** | **3.01×** | 8.82 | 10.01 | 49M | 40M | **PASS** |
| `P-02` | 1,000-way regex alternation | 15.19 s | 182.6 ms | 1.24 s | **83.16×** | **6.78×** | 5.50 | 14.37 | 49M | 137M | **PASS** |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.69 s | 209.9 ms | 348.5 ms | **17.56×** | **1.66×** | 6.38 | 4.12 | 49M | 40M | **PASS** |

---

## 5. Locale Sensitivity Analysis: en_US.UTF-8 vs LC_ALL=C

### Locale Sensitivity: en_US.UTF-8 vs LC_ALL=C (22 Scenarios)

> Ratio = UTF-8 median / C median. A ratio > 1.00× indicates that the C locale is faster (multibyte/case-folding overhead in UTF-8).

| Scenario | Description | G0 UTF-8 | G0 C | G0 (U/C) | G1 UTF-8 | G1 C | G1 (U/C) | R0 UTF-8 | R0 C | R0 (U/C) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | rare literal ($BIG) | 498.0 ms | 501.6 ms | **0.99×** | 18.7 ms | 18.1 ms | **1.03×** | 105.4 ms | 112.3 ms | **0.94×** |
| `B-02` | count dense ($BIG) | 565.8 ms | 571.4 ms | **0.99×** | 20.3 ms | 18.2 ms | **1.12×** | 130.9 ms | 133.7 ms | **0.98×** |
| `B-03` | moderate, line numbers ($BIG) | 903.2 ms | 900.0 ms | **1.00×** | 36.8 ms | 36.6 ms | **1.00×** | 167.8 ms | 175.8 ms | **0.95×** |
| `B-04` | DFA regex ($BIG) | 394.3 ms | 388.1 ms | **1.02×** | 19.4 ms | 18.1 ms | **1.07×** | 106.3 ms | 106.8 ms | **1.00×** |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 1.25 s | **1.57×** | 40.5 ms | 37.9 ms | **1.07×** | 240.1 ms | 242.9 ms | **0.99×** |
| `B-07` | literal-free regex ($BIG) | 624.9 ms | 626.3 ms | **1.00×** | 34.1 ms | 38.6 ms | **0.88×** | 1.53 s | 1.53 s | **1.00×** |
| `B-08` | PCRE2 lookaround ($BIG) | 21.50 s | 7.21 s | **2.98×** | 760.5 ms | 384.2 ms | **1.98×** | 21.80 s | 21.54 s | **1.01×** |
| `M-02` | 1,000 literals, tree | 14.00 s | 13.84 s | **1.01×** | 172.4 ms | 223.5 ms | **0.77×** | 717.8 ms | 713.3 ms | **1.01×** |
| `M-04` | 1,000 literals, -i | 99.02 s | 18.40 s | **5.38×** | 226.8 ms | 225.4 ms | **1.01×** | 1.60 s | 1.58 s | **1.02×** |
| `T-01` | zero-match literal | 3.63 s | 3.63 s | **1.00×** | 210.0 ms | 151.7 ms | **1.38×** | 341.0 ms | 343.6 ms | **0.99×** |
| `T-02` | rare literal | 3.80 s | 3.84 s | **0.99×** | 209.7 ms | 150.8 ms | **1.39×** | 347.1 ms | 360.2 ms | **0.96×** |
| `T-03` | moderate literal, line numbers | 4.30 s | 4.32 s | **1.00×** | 207.4 ms | 153.6 ms | **1.35×** | 367.1 ms | 388.0 ms | **0.95×** |
| `T-04` | dense literal, line numbers | 4.53 s | 4.51 s | **1.00×** | 161.8 ms | 163.2 ms | **0.99×** | 509.2 ms | 514.7 ms | **0.99×** |
| `T-05` | very dense literal | 4.68 s | 4.66 s | **1.00×** | 291.2 ms | 290.7 ms | **1.00×** | 809.8 ms | 774.7 ms | **1.05×** |
| `T-06` | files-with-matches | 3.72 s | 3.79 s | **0.98×** | 162.5 ms | 156.1 ms | **1.04×** | 379.0 ms | 368.0 ms | **1.03×** |
| `T-09` | case-insensitive literal | 5.10 s | 4.36 s | **1.17×** | 157.0 ms | 157.0 ms | **1.00×** | 778.5 ms | 865.2 ms | **0.90×** |
| `T-10` | word match | 4.13 s | 4.12 s | **1.00×** | 157.9 ms | 154.7 ms | **1.02×** | 386.6 ms | 376.4 ms | **1.03×** |
| `T-11` | DFA regex with must-literal | 3.71 s | 3.74 s | **0.99×** | 154.6 ms | 152.5 ms | **1.01×** | 349.9 ms | 354.7 ms | **0.99×** |
| `T-12` | anchored dense regex | 4.63 s | 4.40 s | **1.05×** | 174.5 ms | 167.6 ms | **1.04×** | 689.2 ms | 608.4 ms | **1.13×** |
| `T-15` | literal-free regex | 4.06 s | 4.06 s | **1.00×** | 216.1 ms | 170.0 ms | **1.27×** | 391.1 ms | 374.5 ms | **1.04×** |
| `T-16` | PCRE2 lookaround, dense | 27.62 s | 11.70 s | **2.36×** | 900.9 ms | 398.7 ms | **2.26×** | 2.51 s | 2.59 s | **0.97×** |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.9 ms | **0.94×** | 11.9 ms | 11.8 ms | **1.01×** | 4.9 ms | 5.3 ms | **0.92×** |

### Key Observations on Locale Sensitivity:
- **ASCII Case-Fold Acceleration:** Upstream GNU grep experiences a 5.38× slowdown on multi-pattern case-insensitive search (`M-04`) and 1.57× slowdown on single-file case-insensitive search (`B-05`) in UTF-8 locales. G1's AVX2 ASCII case-folding fast path reduces the UTF-8 penalty to near zero (**1.01× to 1.07×**), matching C locale throughput.

---

## 6. Cold Page Cache Performance (fadvise Evicted)

### Cold Page Cache Performance (fadvise Evicted)

> Eviction via posix_fadvise(POSIX_FADV_DONTNEED) with residency verification (< 2% resident) before every run.
> Cold/Warm Ratio = Cold median / Warm median. Cold Throughput = Corpus Size / Cold Time.

| Scenario | Description | Tool | Warm (ms) | Cold (ms) | Cold / Warm Ratio | Cold Throughput |
| :--- | :--- | :---: | ---: | ---: | ---: | ---: |
| `B-01` | rare literal ($BIG) | **G0** | 498.0 ms | 3.87 s | 7.76× | 360 MB/s |
| `B-01` | rare literal ($BIG) | **G1** | 18.7 ms | 1.22 s | 65.30× | 1.11 GB/s |
| `B-01` | rare literal ($BIG) | **R0** | 105.4 ms | 3.40 s | 32.23× | 409 MB/s |
| `T-01` | zero-match literal | **G0** | 3.63 s | 53.48 s | 14.73× | 31 MB/s |
| `T-01` | zero-match literal | **G1** | 210.0 ms | 1.97 s | 9.39× | 833 MB/s |
| `T-01` | zero-match literal | **R0** | 341.0 ms | 4.96 s | 14.55× | 331 MB/s |
| `T-03` | moderate literal, line numbers | **G0** | 4.30 s | 56.02 s | 13.03× | 29 MB/s |
| `T-03` | moderate literal, line numbers | **G1** | 207.4 ms | 1.96 s | 9.44× | 838 MB/s |
| `T-03` | moderate literal, line numbers | **R0** | 367.1 ms | 5.28 s | 14.38× | 311 MB/s |
| `T-06` | files-with-matches | **G0** | 3.72 s | 54.27 s | 14.59× | 30 MB/s |
| `T-06` | files-with-matches | **G1** | 162.5 ms | 1.96 s | 12.04× | 838 MB/s |
| `T-06` | files-with-matches | **R0** | 379.0 ms | 5.15 s | 13.58× | 319 MB/s |

### Cold Cache Performance Insights:
- Under cold cache conditions where all file data is evicted from RAM, G1 achieves **833 MB/s tree scan throughput** (**1.97 s** on `T-01`), outperforming ripgrep at **331 MB/s** (**4.96 s**, **2.52× faster**) and stock GNU grep at **31 MB/s** (**53.48 s**, **27.1× faster**).
- Concurrent worker prefetching keeps deep I/O request queues filled on underlying cloud storage, eliminating the latency stalls that plague single-threaded traversal.

---

## 7. Multi-Thread Scaling & Fairness Analysis (1 to 128 Threads)

### 7.1 Multi-Thread Scaling Sweep (1 to 128 Threads)

| Scenario | Tool | T=1 | T=2 | T=4 | T=8 | T=12 | T=16 | T=24 | T=32 | T=48 | T=64 | T=96 | T=128 | Peak Speedup vs G0 |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | **G1** | 281.3 ms (1.8×) | 183.5 ms (2.7×) | 130.2 ms (3.8×) | 34.2 ms (14.6×) | 29.0 ms (17.2×) | 22.3 ms (22.4×) | 21.2 ms (23.5×) | 19.6 ms (25.4×) | 19.3 ms (25.8×) | 20.1 ms (24.8×) | 20.3 ms (24.6×) | 21.3 ms (23.4×) | **25.80×** |
| `B-01` | **R0** | 118.0 ms (4.2×) | 114.0 ms (4.4×) | 113.4 ms (4.4×) | 119.3 ms (4.2×) | 114.6 ms (4.3×) | 117.1 ms (4.3×) | 117.8 ms (4.2×) | 109.3 ms (4.6×) | 114.3 ms (4.4×) | 115.9 ms (4.3×) | 116.7 ms (4.3×) | 131.6 ms (3.8×) | **4.56×** |
| `B-04` | **G1** | 297.3 ms (1.3×) | 191.3 ms (2.1×) | 133.8 ms (2.9×) | 42.1 ms (9.4×) | 32.2 ms (12.3×) | 27.4 ms (14.4×) | 24.9 ms (15.8×) | 21.0 ms (18.7×) | 21.6 ms (18.2×) | 21.7 ms (18.2×) | 22.5 ms (17.5×) | 23.4 ms (16.8×) | **18.74×** |
| `B-04` | **R0** | 125.5 ms (3.1×) | 113.1 ms (3.5×) | 114.2 ms (3.5×) | 116.3 ms (3.4×) | 113.9 ms (3.5×) | 115.3 ms (3.4×) | 114.4 ms (3.4×) | 115.8 ms (3.4×) | 120.7 ms (3.3×) | 117.5 ms (3.4×) | 119.0 ms (3.3×) | 116.6 ms (3.4×) | **3.49×** |
| `M-01` | **G1** | — | — | — | — | 305.0 ms (39.7×) | — | — | — | — | — | — | — | **39.68×** |
| `M-01` | **R0** | — | — | — | — | — | — | — | 270.9 ms (44.7×) | — | — | — | — | **44.68×** |
| `M-02` | **G1** | 4.02 s (3.5×) | 1.30 s (10.8×) | 824.6 ms (17.0×) | 460.1 ms (30.4×) | 334.7 ms (41.8×) | 264.4 ms (52.9×) | 195.6 ms (71.6×) | 181.1 ms (77.3×) | 168.2 ms (83.2×) | 225.3 ms (62.1×) | 248.2 ms (56.4×) | 254.8 ms (54.9×) | **83.20×** |
| `M-02` | **R0** | 7.66 s (1.8×) | 3.90 s (3.6×) | 2.02 s (6.9×) | 1.05 s (13.4×) | 714.3 ms (19.6×) | 556.3 ms (25.2×) | 412.7 ms (33.9×) | 351.9 ms (39.8×) | 354.8 ms (39.4×) | 353.4 ms (39.6×) | 375.7 ms (37.3×) | 429.3 ms (32.6×) | **39.77×** |
| `M-03` | **G1** | — | — | — | — | 424.8 ms (40.2×) | — | — | — | — | — | — | — | **40.21×** |
| `M-03` | **R0** | — | — | — | — | — | — | — | 620.5 ms (27.5×) | — | — | — | — | **27.53×** |
| `M-04` | **G1** | — | — | — | — | 374.4 ms (264.4×) | — | — | — | — | — | — | — | **264.45×** |
| `M-04` | **R0** | — | — | — | — | — | — | — | 1.21 s (81.9×) | — | — | — | — | **81.87×** |
| `M-05` | **G1** | — | — | — | — | 337.8 ms (41.3×) | — | — | — | — | — | — | — | **41.25×** |
| `M-05` | **R0** | — | — | — | — | — | — | — | 424.4 ms (32.8×) | — | — | — | — | **32.83×** |
| `M-06` | **G1** | — | — | — | — | 68.1 ms (130.1×) | — | — | — | — | — | — | — | **130.12×** |
| `M-06` | **R0** | — | — | — | — | — | — | — | 3.08 s (2.9×) | — | — | — | — | **2.88×** |
| `T-01` | **G1** | 3.38 s (1.1×) | 1.09 s (3.3×) | 670.4 ms (5.4×) | 388.0 ms (9.4×) | 281.7 ms (12.9×) | 225.9 ms (16.1×) | 169.2 ms (21.5×) | 155.8 ms (23.3×) | 164.7 ms (22.0×) | 221.1 ms (16.4×) | 238.1 ms (15.2×) | 244.5 ms (14.8×) | **23.30×** |
| `T-01` | **R0** | 3.69 s (1.0×) | 1.96 s (1.9×) | 982.1 ms (3.7×) | 518.7 ms (7.0×) | 364.7 ms (10.0×) | 281.1 ms (12.9×) | 215.5 ms (16.8×) | 169.5 ms (21.4×) | 196.3 ms (18.5×) | 232.8 ms (15.6×) | 239.1 ms (15.2×) | 211.2 ms (17.2×) | **21.42×** |
| `T-02` | **G1** | — | — | — | — | 291.5 ms (13.0×) | — | — | — | — | — | — | — | **13.04×** |
| `T-02` | **R0** | — | — | — | — | — | — | — | 166.5 ms (22.8×) | — | — | — | — | **22.83×** |
| `T-03` | **G1** | — | — | — | — | 288.2 ms (14.9×) | — | — | — | — | — | — | — | **14.92×** |
| `T-03` | **R0** | — | — | — | — | — | — | — | 184.5 ms (23.3×) | — | — | — | — | **23.31×** |
| `T-04` | **G1** | 3.59 s (1.3×) | 1.22 s (3.7×) | 709.8 ms (6.4×) | 405.9 ms (11.2×) | 294.2 ms (15.4×) | 231.6 ms (19.6×) | 179.3 ms (25.3×) | 157.6 ms (28.7×) | 152.0 ms (29.8×) | 220.7 ms (20.5×) | 242.3 ms (18.7×) | 249.3 ms (18.2×) | **29.80×** |
| `T-04` | **R0** | 4.69 s (1.0×) | 2.51 s (1.8×) | 1.37 s (3.3×) | 710.6 ms (6.4×) | 533.5 ms (8.5×) | 485.8 ms (9.3×) | 502.2 ms (9.0×) | 548.0 ms (8.3×) | 572.0 ms (7.9×) | 594.9 ms (7.6×) | 620.4 ms (7.3×) | 705.7 ms (6.4×) | **9.32×** |
| `T-05` | **G1** | — | — | — | — | 311.1 ms (15.0×) | — | — | — | — | — | — | — | **15.03×** |
| `T-05` | **R0** | — | — | — | — | — | — | — | 1.04 s (4.5×) | — | — | — | — | **4.50×** |
| `T-06` | **G1** | 3.37 s (1.1×) | 1.08 s (3.4×) | 671.8 ms (5.5×) | 385.5 ms (9.6×) | 277.5 ms (13.4×) | 218.8 ms (17.0×) | 173.0 ms (21.5×) | 155.1 ms (24.0×) | 151.3 ms (24.6×) | 217.2 ms (17.1×) | 240.5 ms (15.5×) | 248.1 ms (15.0×) | **24.59×** |
| `T-06` | **R0** | 3.79 s (1.0×) | 2.02 s (1.8×) | 1.02 s (3.6×) | 526.5 ms (7.1×) | 359.1 ms (10.4×) | 297.0 ms (12.5×) | 211.3 ms (17.6×) | 211.6 ms (17.6×) | 174.8 ms (21.3×) | 232.0 ms (16.0×) | 242.4 ms (15.3×) | 262.3 ms (14.2×) | **21.28×** |
| `T-07` | **G1** | — | — | — | — | 271.3 ms (12.4×) | — | — | — | — | — | — | — | **12.38×** |
| `T-07` | **R0** | — | — | — | — | — | — | — | 316.2 ms (10.6×) | — | — | — | — | **10.62×** |
| `T-08` | **G1** | — | — | — | — | 286.3 ms (12.8×) | — | — | — | — | — | — | — | **12.82×** |
| `T-08` | **R0** | — | — | — | — | — | — | — | 1.54 s (2.4×) | — | — | — | — | **2.39×** |
| `T-09` | **G1** | — | — | — | — | 286.2 ms (17.8×) | — | — | — | — | — | — | — | **17.84×** |
| `T-09` | **R0** | — | — | — | — | — | — | — | 1.01 s (5.1×) | — | — | — | — | **5.08×** |
| `T-10` | **G1** | 3.46 s (1.2×) | 1.12 s (3.7×) | 685.0 ms (6.0×) | 401.5 ms (10.3×) | 282.2 ms (14.6×) | 229.2 ms (18.0×) | 171.8 ms (24.0×) | 154.1 ms (26.8×) | 151.3 ms (27.3×) | 223.4 ms (18.5×) | 228.3 ms (18.1×) | 254.8 ms (16.2×) | **27.31×** |
| `T-10` | **R0** | 3.95 s (1.0×) | 2.09 s (2.0×) | 1.07 s (3.9×) | 558.2 ms (7.4×) | 377.4 ms (10.9×) | 293.1 ms (14.1×) | 248.6 ms (16.6×) | 199.3 ms (20.7×) | 189.4 ms (21.8×) | 271.3 ms (15.2×) | 277.4 ms (14.9×) | 270.6 ms (15.3×) | **21.81×** |
| `T-11` | **G1** | — | — | — | — | 277.2 ms (13.4×) | — | — | — | — | — | — | — | **13.39×** |
| `T-11` | **R0** | — | — | — | — | — | — | — | 166.0 ms (22.4×) | — | — | — | — | **22.35×** |
| `T-12` | **G1** | — | — | — | — | 309.4 ms (15.0×) | — | — | — | — | — | — | — | **14.96×** |
| `T-12` | **R0** | — | — | — | — | — | — | — | 766.2 ms (6.0×) | — | — | — | — | **6.04×** |
| `T-13` | **G1** | — | — | — | — | 295.4 ms (13.0×) | — | — | — | — | — | — | — | **13.04×** |
| `T-13` | **R0** | — | — | — | — | — | — | — | 233.8 ms (16.5×) | — | — | — | — | **16.48×** |
| `T-14` | **G1** | — | — | — | — | 287.1 ms (13.2×) | — | — | — | — | — | — | — | **13.16×** |
| `T-14` | **R0** | — | — | — | — | — | — | — | 170.3 ms (22.2×) | — | — | — | — | **22.20×** |
| `T-15` | **G1** | 4.02 s (1.0×) | 1.31 s (3.1×) | 813.1 ms (5.0×) | 460.6 ms (8.8×) | 330.6 ms (12.3×) | 258.4 ms (15.7×) | 194.4 ms (20.9×) | 173.7 ms (23.4×) | 152.9 ms (26.6×) | 215.8 ms (18.8×) | 242.4 ms (16.8×) | 251.8 ms (16.1×) | **26.59×** |
| `T-15` | **R0** | 4.10 s (1.0×) | 2.17 s (1.9×) | 1.09 s (3.7×) | 560.1 ms (7.3×) | 386.0 ms (10.5×) | 288.9 ms (14.1×) | 217.9 ms (18.7×) | 171.9 ms (23.7×) | 202.6 ms (20.1×) | 224.7 ms (18.1×) | 220.9 ms (18.4×) | 262.0 ms (15.5×) | **23.65×** |
| `T-16` | **G1** | 27.55 s (1.0×) | 9.04 s (3.1×) | 5.45 s (5.1×) | 3.05 s (9.1×) | 2.12 s (13.0×) | 1.63 s (17.0×) | 1.12 s (24.6×) | 954.1 ms (29.0×) | 896.3 ms (30.8×) | 912.1 ms (30.3×) | 763.1 ms (36.2×) | 744.1 ms (37.1×) | **37.12×** |
| `T-16` | **R0** | 29.18 s (0.9×) | 14.80 s (1.9×) | 7.40 s (3.7×) | 3.79 s (7.3×) | 2.51 s (11.0×) | 1.94 s (14.2×) | 1.31 s (21.1×) | 1.14 s (24.3×) | 1.09 s (25.3×) | 1.04 s (26.7×) | 963.1 ms (28.7×) | 990.1 ms (27.9×) | **28.68×** |
| `T-17` | **G1** | — | — | — | — | 388.3 ms (13.6×) | — | — | — | — | — | — | — | **13.58×** |
| `T-17` | **R0** | — | — | — | — | — | — | — | 229.1 ms (23.0×) | — | — | — | — | **23.02×** |
| `T-18` | **G1** | — | — | — | — | 301.4 ms (13.6×) | — | — | — | — | — | — | — | **13.64×** |
| `T-18` | **R0** | — | — | — | — | — | — | — | 201.6 ms (20.4×) | — | — | — | — | **20.38×** |
| `T-19` | **G1** | — | — | — | — | 276.3 ms (15.6×) | — | — | — | — | — | — | — | **15.62×** |
| `T-19` | **R0** | — | — | — | — | — | — | — | 192.7 ms (22.4×) | — | — | — | — | **22.40×** |
| `T-20` | **G1** | — | — | — | — | 289.7 ms (15.1×) | — | — | — | — | — | — | — | **15.05×** |
| `T-20` | **R0** | — | — | — | — | — | — | — | 203.6 ms (21.4×) | — | — | — | — | **21.41×** |
| `T-21` | **G1** | — | — | — | — | 353.4 ms (12.6×) | — | — | — | — | — | — | — | **12.56×** |
| `T-21` | **R0** | — | — | — | — | — | — | — | 1.55 s (2.9×) | — | — | — | — | **2.87×** |
| `T-22` | **G1** | — | — | — | — | 137.3 ms (15.0×) | — | — | — | — | — | — | — | **15.02×** |
| `T-22` | **R0** | — | — | — | — | — | — | — | 101.7 ms (20.3×) | — | — | — | — | **20.27×** |
| `T-23` | **G1** | — | — | — | — | 15.9 ms (8.0×) | — | — | — | — | — | — | — | **7.97×** |
| `T-23` | **R0** | — | — | — | — | — | — | — | 26.2 ms (4.8×) | — | — | — | — | **4.84×** |
| `T-24` | **G1** | — | — | — | — | 286.8 ms (13.1×) | — | — | — | — | — | — | — | **13.11×** |
| `T-24` | **R0** | — | — | — | — | — | — | — | 183.0 ms (20.5×) | — | — | — | — | **20.55×** |
| `T-25a` | **G1** | — | — | — | — | 287.0 ms (12.7×) | — | — | — | — | — | — | — | **12.67×** |
| `T-25a` | **R0** | — | — | — | — | — | — | — | 165.7 ms (21.9×) | — | — | — | — | **21.94×** |
| `T-25b` | **G1** | — | — | — | — | 7.4 ms (0.4×) | — | — | — | — | — | — | — | **0.36×** |
| `T-25b` | **R0** | — | — | — | — | — | — | — | 7.0 ms (0.4×) | — | — | — | — | **0.38×** |
| `T-26` | **G1** | — | — | — | — | 288.3 ms (14.9×) | — | — | — | — | — | — | — | **14.93×** |
| `T-26` | **R0** | — | — | — | — | — | — | — | 186.0 ms (23.1×) | — | — | — | — | **23.14×** |
| `T-27` | **G1** | — | — | — | — | 128.2 ms (15.3×) | — | — | — | — | — | — | — | **15.34×** |
| `T-27` | **R0** | — | — | — | — | — | — | — | 97.6 ms (20.1×) | — | — | — | — | **20.14×** |
| `T-28` | **G1** | — | — | — | — | 17.1 ms (9.6×) | — | — | — | — | — | — | — | **9.61×** |
| `T-28` | **R0** | — | — | — | — | — | — | — | 14.0 ms (11.7×) | — | — | — | — | **11.75×** |
| `P-01` | **G1** | — | — | — | — | 600.2 ms (69.3×) | — | — | — | — | — | — | — | **69.32×** |
| `P-01` | **R0** | — | — | — | — | — | — | — | 357.2 ms (116.5×) | — | — | — | — | **116.47×** |
| `P-02` | **G1** | — | — | — | — | 339.2 ms (44.8×) | — | — | — | — | — | — | — | **44.77×** |
| `P-02` | **R0** | — | — | — | — | — | — | — | 761.2 ms (20.0×) | — | — | — | — | **19.95×** |
| `P-03` | **G1** | — | — | — | — | 287.3 ms (12.8×) | — | — | — | — | — | — | — | **12.83×** |
| `P-03` | **R0** | — | — | — | — | — | — | — | 159.3 ms (23.1×) | — | — | — | — | **23.13×** |

### 7.2 Matched Thread Comparison Table: G1 (32t) vs G1 (12t) vs R0 (Default 12t) vs R0 (-j 32)

> Side-by-side comparison of G1 (default 32 threads and matched 12 threads) against both out-of-the-box R0 (default 12 threads) and thread-matched `R0 -j 32` (32 threads).

| Scenario | Cat | Description | G0 (1t) | G1 (12t) | R0 (12t) | G1 vs R0 (12t) | G1 (32t) | R0 (-j 32) | G1 vs R0 (32t) | G1 (32t) vs R0 (12t) |
| :--- | :-: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | B | rare literal ($BIG) | 498.0 ms | 29.0 ms | 105.4 ms | **3.63×** | 18.7 ms | 109.3 ms | **5.85×** | **5.64×** |
| `B-02` | B | count dense ($BIG) | 565.8 ms | — | 130.9 ms | **—** | 20.3 ms | 134.6 ms | **6.63×** | **6.45×** |
| `B-03` | B | moderate, line numbers ($BIG) | 903.2 ms | — | 167.8 ms | **—** | 36.8 ms | 167.3 ms | **4.55×** | **4.57×** |
| `B-04` | B | DFA regex ($BIG) | 394.3 ms | 32.2 ms | 106.3 ms | **3.30×** | 19.4 ms | 115.8 ms | **5.97×** | **5.48×** |
| `B-05` | B | case-insensitive dense ($BIG) | 1.97 s | — | 240.1 ms | **—** | 40.5 ms | 266.7 ms | **6.58×** | **5.92×** |
| `B-06` | B | word match ($BIG) | 327.4 ms | — | 121.6 ms | **—** | 20.4 ms | 113.1 ms | **5.54×** | **5.96×** |
| `B-07` | B | literal-free regex ($BIG) | 624.9 ms | — | 1.53 s | **—** | 34.1 ms | 1.53 s | **44.96×** | **44.72×** |
| `B-08` | B | PCRE2 lookaround ($BIG) | 21.50 s | — | 21.80 s | **—** | 760.5 ms | 21.64 s | **28.46×** | **28.66×** |
| `B-09` | B | context ($BIG) | 890.6 ms | — | 188.2 ms | **—** | 30.6 ms | 190.3 ms | **6.21×** | **6.14×** |
| `B-10` | B | only-matching ($BIG) | 729.7 ms | — | 129.1 ms | **—** | 45.7 ms | 126.3 ms | **2.76×** | **2.83×** |
| `B-11` | B | inverted count ($BIG) | 973.2 ms | — | 1.44 s | **—** | 45.9 ms | 1.42 s | **30.92×** | **31.36×** |
| `B-12` | B | stdin redirect (< $BIG) | 498.6 ms | — | 219.3 ms | **—** | 18.2 ms | 214.4 ms | **11.75×** | **12.02×** |
| `B-13` | B | FIFO pipe (cat $BIG \\|) | 1.14 s | — | 994.5 ms | **—** | 674.5 ms | 958.5 ms | **1.42×** | **1.47×** |
| `B-14` | B | real 24 MB header (big-24M.h) | 48.6 ms | — | 41.9 ms | **—** | 16.0 ms | 42.2 ms | **2.64×** | **2.62×** |
| `L-00` | L | harness floor (/bin/true) | 1.5 ms | — | 1.5 ms | **—** | 1.5 ms | 1.5 ms | **1.00×** | **1.01×** |
| `L-01` | L | one small file (init/main.c) | 2.7 ms | — | 3.8 ms | **—** | 2.6 ms | 3.7 ms | **1.46×** | **1.47×** |
| `L-02` | L | tiny tree (kernel/sched) | 7.0 ms | — | 7.0 ms | **—** | 5.4 ms | 9.2 ms | **1.70×** | **1.29×** |
| `L-03` | L | quiet first match in big file | 2.7 ms | — | 3.8 ms | **—** | 2.7 ms | 3.8 ms | **1.41×** | **1.39×** |
| `M-01` | M | 100 literals, tree | 12.10 s | 305.0 ms | 654.8 ms | **2.15×** | 161.6 ms | 270.9 ms | **1.68×** | **4.05×** |
| `M-02` | M | 1,000 literals, tree | 14.00 s | 334.7 ms | 717.8 ms | **2.14×** | 172.4 ms | 351.9 ms | **2.04×** | **4.16×** |
| `M-03` | M | 10,000 literals, tree | 17.08 s | 424.8 ms | 1.19 s | **2.79×** | 271.6 ms | 620.5 ms | **2.28×** | **4.37×** |
| `M-04` | M | 1,000 literals, -i | 99.02 s | 374.4 ms | 1.60 s | **4.28×** | 226.8 ms | 1.21 s | **5.33×** | **7.06×** |
| `M-05` | M | 1,000 literals, -w | 13.93 s | 337.8 ms | 918.8 ms | **2.72×** | 213.9 ms | 424.4 ms | **1.98×** | **4.30×** |
| `M-06` | M | 1,000 literals, $BIG | 8.86 s | 68.1 ms | 3.10 s | **45.58×** | 43.1 ms | 3.08 s | **71.38×** | **72.02×** |
| `T-01` | T | zero-match literal | 3.63 s | 281.7 ms | 341.0 ms | **1.21×** | 210.0 ms | 169.5 ms | **0.81×** | **1.62×** |
| `T-02` | T | rare literal | 3.80 s | 291.5 ms | 347.1 ms | **1.19×** | 209.7 ms | 166.5 ms | **0.79×** | **1.66×** |
| `T-03` | T | moderate literal, line numbers | 4.30 s | 288.2 ms | 367.1 ms | **1.27×** | 207.4 ms | 184.5 ms | **0.89×** | **1.77×** |
| `T-04` | T | dense literal, line numbers | 4.53 s | 294.2 ms | 509.2 ms | **1.73×** | 161.8 ms | 548.0 ms | **3.39×** | **3.15×** |
| `T-05` | T | very dense literal | 4.68 s | 311.1 ms | 809.8 ms | **2.60×** | 291.2 ms | 1.04 s | **3.57×** | **2.78×** |
| `T-06` | T | files-with-matches | 3.72 s | 277.5 ms | 379.0 ms | **1.37×** | 162.5 ms | 211.6 ms | **1.30×** | **2.33×** |
| `T-07` | T | files-without-match | 3.36 s | 271.3 ms | 432.4 ms | **1.59×** | 152.5 ms | 316.2 ms | **2.07×** | **2.84×** |
| `T-08` | T | count per file | 3.67 s | 286.3 ms | 1.11 s | **3.87×** | 155.2 ms | 1.54 s | **9.91×** | **7.14×** |
| `T-09` | T | case-insensitive literal | 5.10 s | 286.2 ms | 778.5 ms | **2.72×** | 157.0 ms | 1.01 s | **6.41×** | **4.96×** |
| `T-10` | T | word match | 4.13 s | 282.2 ms | 386.6 ms | **1.37×** | 157.9 ms | 199.3 ms | **1.26×** | **2.45×** |
| `T-11` | T | DFA regex with must-literal | 3.71 s | 277.2 ms | 349.9 ms | **1.26×** | 154.6 ms | 166.0 ms | **1.07×** | **2.26×** |
| `T-12` | T | anchored dense regex | 4.63 s | 309.4 ms | 689.2 ms | **2.23×** | 174.5 ms | 766.2 ms | **4.39×** | **3.95×** |
| `T-13` | T | literal alternation + word boundary | 3.85 s | 295.4 ms | 382.5 ms | **1.30×** | 184.4 ms | 233.8 ms | **1.27×** | **2.07×** |
| `T-14` | T | optional group + alternation | 3.78 s | 287.1 ms | 347.4 ms | **1.21×** | 207.5 ms | 170.3 ms | **0.82×** | **1.67×** |
| `T-15` | T | literal-free regex | 4.06 s | 330.6 ms | 391.1 ms | **1.18×** | 216.1 ms | 171.9 ms | **0.80×** | **1.81×** |
| `T-16` | T | PCRE2 lookaround, dense | 27.62 s | 2.12 s | 2.51 s | **1.18×** | 900.9 ms | 1.14 s | **1.26×** | **2.79×** |
| `T-17` | T | PCRE2 rare with literal | 5.27 s | 388.3 ms | 525.6 ms | **1.35×** | 193.3 ms | 229.1 ms | **1.19×** | **2.72×** |
| `T-18` | T | only-matching | 4.11 s | 301.4 ms | 368.6 ms | **1.22×** | 166.4 ms | 201.6 ms | **1.21×** | **2.22×** |
| `T-19` | T | context, rare | 4.32 s | 276.3 ms | 380.4 ms | **1.38×** | 155.1 ms | 192.7 ms | **1.24×** | **2.45×** |
| `T-20` | T | context, moderate | 4.36 s | 289.7 ms | 382.8 ms | **1.32×** | 214.8 ms | 203.6 ms | **0.95×** | **1.78×** |
| `T-21` | T | inverted count | 4.44 s | 353.4 ms | 1.26 s | **3.57×** | 211.3 ms | 1.55 s | **7.32×** | **5.97×** |
| `T-22` | T | glob include | 2.06 s | 137.3 ms | 168.7 ms | **1.23×** | 101.9 ms | 101.7 ms | **1.00×** | **1.65×** |
| `T-23` | T | glob + sub-scope | 126.9 ms | 15.9 ms | 24.6 ms | **1.55×** | 16.1 ms | 26.2 ms | **1.63×** | **1.53×** |
| `T-24` | T | max-count per file | 3.76 s | 286.8 ms | 355.3 ms | **1.24×** | 202.5 ms | 183.0 ms | **0.90×** | **1.75×** |
| `T-25a` | T | quiet, no match | 3.64 s | 287.0 ms | 347.3 ms | **1.21×** | 205.9 ms | 165.7 ms | **0.80×** | **1.69×** |
| `T-25b` | T | quiet, immediate match | 2.7 ms | 7.4 ms | 4.9 ms | **0.66×** | 11.9 ms | 7.0 ms | **0.59×** | **0.41×** |
| `T-26` | T | byte offsets | 4.30 s | 288.3 ms | 359.8 ms | **1.25×** | 213.8 ms | 186.0 ms | **0.87×** | **1.68×** |
| `T-27` | T | sub-scope drivers/ | 1.97 s | 128.2 ms | 166.8 ms | **1.30×** | 93.7 ms | 97.6 ms | **1.04×** | **1.78×** |
| `T-28` | T | sub-scope kernel mm fs | 164.1 ms | 17.1 ms | 19.3 ms | **1.13×** | 17.4 ms | 14.0 ms | **0.80×** | **1.11×** |
| `U-01` | U | out-of-the-box UX | 4.29 s | — | 390.7 ms | **—** | 152.9 ms | 234.1 ms | **1.53×** | **2.55×** |
| `U-02` | U | naive invocation (scans 6 GB .git) | 5.94 s | — | 392.3 ms | **—** | 267.5 ms | 219.6 ms | **0.82×** | **1.47×** |
| `X-01` | X | git ls-files \\| xargs | 4.18 s | — | 1.12 s | **—** | 484.0 ms | 1.00 s | **2.07×** | **2.31×** |
| `P-01` | P | backreference (\b([a-z]{4,})_\1\b) | 41.61 s | 600.2 ms | 841.8 ms | **1.40×** | 279.8 ms | 357.2 ms | **1.28×** | **3.01×** |
| `P-02` | P | 1,000-way regex alternation | 15.19 s | 339.2 ms | 1.24 s | **3.65×** | 182.6 ms | 761.2 ms | **4.17×** | **6.78×** |
| `P-03` | P | dot-star heavy (.*_lock.*_unlock) | 3.69 s | 287.3 ms | 348.5 ms | **1.21×** | 209.9 ms | 159.3 ms | **0.76×** | **1.66×** |

### 7.3 Why Ripgrep Defaults to 12 Threads vs Matched `-j 32` Behavior
1. **Default Heuristic Difference:**
   - Stock `ripgrep` caps its default directory-traversal thread count at `std::cmp::min(cores, 12)` (`12` worker threads on this 128-CPU server),
     whereas `G1` defaults to `MIN(count, 32)` (`32` worker threads).
2. **What Happens When Ripgrep Is Given `-j 32` Across All 59 Scenarios:**
   - **Single Large Files (`B-01..B-14`, `M-06`):** `-j 32` has **zero effect** on `ripgrep` (`105–267 ms`) because `ripgrep` does not parallelize
     within a single file. `G1` beats `ripgrep -j 32` on **14/14** `B-*` scenarios (**7.27× geomean**) and by **71.38×** on `M-06`.
   - **High-Output & Dense Match Tree Walks (`T-04`, `T-05`, `T-08`, `T-09`, `T-12`, `T-21`):** Increasing `ripgrep` from 12 to 32 threads
     **degrades** `ripgrep`'s runtime on 5 of these 6 scenarios (`T-05`: `809.8 ms` → `1.04 s`; `T-08`: `1.11 s` → `1.54 s`;
     `T-09`: `778.5 ms` → `1.01 s`; `T-12`: `689.2 ms` → `766.2 ms`; `T-21`: `1.26 s` → `1.55 s`) due to output lock contention.
     `G1` beats `ripgrep -j 32` by **1.26× to 9.91×**.
   - **Heavy Multi-Pattern & PCRE2 Tree Walks (`M-01..M-05`, `T-16`):** Even at matched 32 threads, `G1` beats `ripgrep -j 32`
     on all multi-pattern sets (`M-01`: **1.68×**, `M-02`: **2.04×**, `M-03`: **2.28×**, `M-04`: **5.33×**, `M-05`: **1.98×**),
     and dense PCRE2 lookaround (`900.9 ms` vs `1.14 s`, **1.26×**).
   - **Matched 12 Threads (`G1 @ 12t` vs `R0 @ 12t`):** When both tools run on 12 threads on the 40 matched scenarios, G1 wins **39 out of 40** scenarios (**1.83× geomean**); on the 29 Category T tree scenarios, G1 wins **28 out of 29** scenarios (**1.46× category geomean**),
     showing that our directory walker and search algorithms are fundamentally faster per-core.

---

## 8. Synthetic Benchmark Suite & Edge-Case Conformance

In addition to real-world kernel tree benchmarks, the project maintains an automated 13-workload synthetic benchmark
suite (`benchmarks/run_benchmarks.py`), a 200-test automated edge-case, boundary, and multithreaded conformance harness, and a continuous stress test harness (`benchmarks/stress_test.py`).

### 13-Workload Synthetic Benchmark (5 Iterations per Workload)

| Workload ID | Description | Corpus Size | Stock Baseline G0 (ms) | Baseline Throughput | G1 (t=1) (ms) | G1 (t=4) (ms) | G1 Default (32t) (ms) | Speedup (vs G0) | Equivalence |
| :--- | :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `BM-01` | `-F` rare literal | 1572 MB | 646.2 ms | 2.38 GB/s | 440.0 ms | 233.5 ms | **75.3 ms** | **8.58×** | **PASS** |
| `BM-02` | `-Fi` case-insensitive literal | 1572 MB | 679.9 ms | 2.26 GB/s | 434.1 ms | 235.1 ms | **77.9 ms** | **8.72×** | **PASS** |
| `BM-03` | `-E` DFA medium regex | 1572 MB | 555.5 ms | 2.76 GB/s | 452.0 ms | 235.8 ms | **80.3 ms** | **6.92×** | **PASS** |
| `BM-04` | `-E` regex backreference | 1572 MB | 1307.6 ms | 1.17 GB/s | 459.5 ms | 226.4 ms | **74.4 ms** | **17.57×** | **PASS** |
| `BM-05` | `-P` PCRE2 lookbehind | 1572 MB | 934.8 ms | 1.64 GB/s | 439.6 ms | 227.4 ms | **77.8 ms** | **12.02×** | **PASS** |
| `BM-06` | `-n -b -F` line numbers & byte offset | 1572 MB | 752.3 ms | 2.04 GB/s | 471.9 ms | 312.9 ms | **200.3 ms** | **3.76×** | **PASS** |
| `BM-07` | `-n -C 3` context printing | 1572 MB | 751.5 ms | 2.04 GB/s | 477.6 ms | 338.5 ms | **116.1 ms** | **6.47×** | **PASS** |
| `BM-08` | `-c -E` match counting | 1572 MB | 553.1 ms | 2.78 GB/s | 447.1 ms | 227.1 ms | **76.8 ms** | **7.20×** | **PASS** |
| `BM-09` | `-r -n` recursive tree line numbers | 566 MB | 404.4 ms | 1.37 GB/s | 297.1 ms | 67.3 ms | **67.0 ms** | **6.04×** | **PASS** |
| `BM-10` | `-r -l` recursive file listing | 566 MB | 223.3 ms | 2.48 GB/s | 221.3 ms | 51.1 ms | **88.1 ms** | **2.54×** | **PASS** |
| `BM-11` | `-r -F` multi-pattern tree | 566 MB | 285.8 ms | 1.94 GB/s | 301.5 ms | 67.1 ms | **95.5 ms** | **2.99×** | **PASS** |
| `BM-12` | Stdin file redirect (`< $FILE`) | 1572 MB | 648.5 ms | 2.37 GB/s | 443.4 ms | 226.5 ms | **76.3 ms** | **8.50×** | **PASS** |
| `BM-13` | True FIFO pipe (`cat \| grep`) | 1572 MB | 1188.6 ms | 1.29 GB/s | 1134.8 ms | 798.6 ms | **807.7 ms** | **1.47×** | **PASS** |

### Automated Edge-Case, Boundary & Multithreaded Conformance (200 / 200 PASS)
The 200 automated conformance checks verify boundary and concurrency conditions against stock baseline grep:
- **Thread Scaling Invariance (Checks 1–14, 140–149):** Thread sweeps across single-file and tree searches at power-of-two, prime, and oversubscribed thread counts ($t \in \{1, 2, 3, 4, 5, 7, 8, 16, 31, 32, 64, 128\}$) pass byte-for-byte.
- **Context & Group Separators (Checks 15–16):** Context overlaps with `-C 1`, `-C 2`, and `--group-separator` match GNU behavior.
- **Inverted Matching & Line Counting (Checks 17–19, 33):** Inverted searches (`-v`, `-c -v`, `-n -v`) across files up to 2 GB match byte-for-byte.
- **Color & Only-Matching (Checks 20–21):** `--color=always -o` formatting matches exactly.
- **Max-Count Bounds (Checks 22–25):** `-m 1`, `-m 5` with and without context, and recursive tree `-m 1` match.
- **NUL-Delimited Data & Files (Checks 26–27):** `-z` (null data) and `-Z -l` (null filename delimiter) match.
- **Binary File Handling (Checks 28–30):** Default binary detection, `-a` (text mode), and `-I` (ignore binary) match.
- **Virtual Memory Clamping & Slabs (Checks 31–32, 50–51):** `RLIMIT_AS 512MB` and `256MB` slab fallbacks function cleanly without crash or leak.
- **File Descriptor Constraints (Checks 52–53):** `RLIMIT_NOFILE 32` tree searches execute cleanly without EMFILE.
- **PCRE2 Advanced Syntax (Checks 34–37, 59–69):** `-Pi`, inline `(?i)`, bounded `{0,3}`, grouped quantifiers, lookaround assertions, named/numbered backreferences, and POSIX classes match.
- **Tab Alignment (Checks 38–39):** Initial tab `-T` alignment matches on single files and trees.
- **Dense Match Buffering (Checks 40–41):** 75,000 matches with `-C 1` format without buffer overflow or truncation.
- **DFA Boundary Heuristic (Checks 42–45):** Negative offset DFA edge cases (`EGexecute` boundary heuristic) resolve safely.
- **Post-File Batch Splitting (Checks 46–49):** High-density regex matches with batch splits match at $t=1$ and $t=32$.
- **Regex Pre-Filter Escapes & Multibyte Anchors (Checks 54–58, 70–79):** Escaped alternation pipes (`\|`), bracket character classes containing `|`, PCRE2 `\Q...\E` literals, BRE/ERE alternations, and non-UTF-8 multibyte `^`/`$` anchors match byte-for-byte.
- **CLI Option & Error Diagnostics (Checks 80–97):** `GREP_THREADS` parsing, `-f` pattern files, `-e` combinations, `-H`/`-h`, `--label`, `-s`, `-L`, `-q`, and syntax error codes match.
- **Buffer, Chunk & File Size Boundaries (Checks 98–139):** 0-byte, 1-byte, missing trailing newline, exact 64 KiB `SMALL_FILE_MAX`, exact 96 KiB pipe buffer, exact 2 MiB `CHUNK_SIZE` and $2\times$ `CHUNK_SIZE` (4 MiB) seam splits, lines > 64 KiB, and 1, 2, 32, 33, and 257 multi-pattern keyword thresholds match byte-for-byte.
- **Multithreaded Concurrency & Stress (Checks 150–159 + `stress_test.py`):** Concurrent multi-chunk files, Reorder Buffer (ROB) wrap-around (> 1,024 slots), head-of-line blocking, and a continuous 5-minute stress test (30,800+ randomized cycles across 1–64 threads) pass with zero discrepancies.
- **Round 2 & 3 Regression Edge Cases (Checks 160–200):** Zero-context `-C 0`, `-A 0`, `-B 0`, `-0` chunk boundary matches, fast-mmap binary/text separator and `-q` ordering, `kwsmusts_multi` quantifier handling (`\+`, `{m,n}`), stacked backreference quantifiers, PCRE `(*ACCEPT)` must-literals, DFA 17-branch alternations, disjoint bracket sets, multi-character backreference separators, and shorter `-w -o` fallback passes 100% byte-for-byte.

---

## 9. Correctness Audit & Hypothesis Scorecard

### Multi-Tier Verification Gates
- **Gate A (Hard Correctness)**: `G1` output is byte-exact identical to `G0` on `stdout`, normalized `stderr`, and exit code.
  **Result: 59 / 59 Scenarios PASS (100%)**.
- **Gate B (Cross-Tool Equivalence)**: `R0`, `G2`, and `X1` output sets match `G0` after sorting.
  **Result: PASS on all matching scenarios**. T-07 and T-21 soft diffs stem from binary file message differences; T-09 soft diff
  stems from ripgrep stripping the UTF-8 BOM in `sparse.rst`.
- **Gate C (Exit Code Consistency)**: `G1` and `G0` exit codes match identically across all tests (0 for match, 1 for no match, 123 for xargs).
- **Gate D (Thread Invariance Determinism)**: `G1` output across all thread counts {1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128}
  produces identical SHA-256 hashes.
  **Result: 9 / 9 Scenarios PASS (100%)**.

| **H1** | Tree literal scan speedup | Ours ≥ 5× G0; within ±30% of R0 | T-01: 17.29× vs G0, 1.62× vs R0; T-04: 27.99× vs G0, 3.15× vs R0 | ✅ PASS |
| **H2** | $BIG intra-file parallelism | Ours ≥ 4× R0 (R0 searches 1 file with 1 thread) | B-01: 5.64× vs R0; B-03: 4.57× vs R0 | ✅ PASS |
| **H3** | Tiny invocation startup latency | Ours ≤ 1.5× G0; R0 ≈ 2–3× G0 | G0: 2.7 ms, G1: 2.6 ms (0.96×), R0: 3.8 ms | ✅ PASS |
| **H4** | Output-bound queries (~1.3M lines) | Ours never slower than G0 | T-05: G1 is 16.06× vs G0 | ✅ PASS |
| **H5** | UTF-8 vs C locale sensitivity | Both greps slower in UTF-8 on -i; R0 unaffected | T-09 (-i): G0 1.17× slower in UTF-8, G1 1.00×; R0 0.90× (unaffected) | ⚠️ CHECK |
| **H6** | Cold page cache | All tools bounded by PD bandwidth (within 20–35%) | T-01: G1 1.97 s (833 MB/s), R0 4.96 s (331 MB/s), G0 53.48 s | ⚠️ DIVERGENT |
| **H7** | 1,000-literal multi-pattern (M-02) | Ours within 2× of R0 | M-02: 81.19× vs G0, 4.16× vs R0 | ✅ PASS |
| **H8** | PCRE2 lookaround (T-16, B-08) | Ours ≫ G0 (literal pre-filter + threads) and ≥ R0 | T-16: 30.66× vs G0, 2.79× vs R0; B-08: 28.26× vs G0 | ✅ PASS |
| **H9** | Scaling past 16 threads | Gains throughput past 16 threads | B-01: 1.11× (16→64 th); 1..128 thread sweep across 128 logical CPUs (2 NUMA nodes) | ✅ PASS |

---

*Report generated automatically from primary benchmark artifacts.*
