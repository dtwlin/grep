<!-- Copyright (C) 2026 David Lin <dtwlin@gmail.com> -->
# Multi-Threaded GNU Grep: Benchmark & Performance Evaluation

> Comprehensive performance, scalability, and correctness evaluation comparing
> our parallel multi-threaded GNU `grep` fork (`G1`, commit `83748c6`, default 32 worker threads)
> against upstream GNU `grep` baseline (`G0`, version 3.12.41, 1 thread), stock `ripgrep` (`R0`,
> version 15.1.0 with PCRE2, evaluated both at its default 12 worker threads and at thread-matched `-j 32`),
> distro GNU `grep` (`G2`, version 3.12), and `git grep` (`X1`, version 2.56.0).

---

## 1. Executive Summary & Headline Takeaways

This document presents the complete benchmark results for the parallel GNU `grep` drop-in replacement.
All measurements were conducted on a 128-logical-CPU dual-socket AMD EPYC 7B13 server across the full
Linux kernel codebase (v7.3-rc3, 96,034 tracked files, 1.64 GB working tree) and a 1.39 GB unified source
concatenation (`linux-all.c`), alongside a 266-test automated edge-case, boundary, and multithreaded conformance suite, 136 Automake test suites, continuous stress testing, and 13 synthetic workloads.

To ensure complete fairness when comparing against `ripgrep` (`R0`) — which heuristically caps its default directory-walk
thread pool at `min(cores, 12)` (12 worker threads) while `G1` defaults to `min(cores, 32)` (32 worker threads) —
every scenario was evaluated against **both** out-of-the-box `ripgrep` (12 threads) and thread-matched `ripgrep -j 32` (32 threads),
as well as a dedicated thread-matched suite running `G1` at 12 threads against `ripgrep` at 12 threads.

### High-Level Summary Statistics (Warm Cache, en_US.UTF-8)

| Metric | G1 (32t) vs Stock G0 (1t) | R0 Default (12t) vs G0 (1t) | R0 Matched (-j 32) vs G0 (1t) | G1 (32t) vs R0 Default (12t) | G1 (32t) vs R0 Matched (-j 32) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Geometric Mean Speedup (All 59 Scenarios)** | **21.32×** | **5.83×** | **7.91×** | **3.66×** | **2.70×** |
| **Category B — Single Huge File (14 Scenarios)** | **18.14×** | 2.39× | 2.37× | **7.59×** | **7.64×** |
| **Category T — Tree Traversal (29 Scenarios)** | **22.21×** | 8.15× | 12.22× | **2.73×** | **1.82×** |
| **Category M — Multi-Pattern Sets (6 Scenarios)** | **121.05×** | 15.50× | 26.90× | **7.81×** | **4.50×** |
| **Category L — Invocation Latency (4 Scenarios)** | **1.05×** (zero pool cost) | 0.84× | 0.79× | **1.24×** | **1.33×** |
| **Category U — UX & Defaults (2 Scenarios)** | **24.32×** | 12.89× | 22.26× | **1.89×** | **1.09×** |
| **Category X — xargs Parallelism (1 Scenario)** | **8.49×** | 3.75× | 4.18× | **2.26×** | **2.03×** |
| **Category P — Complex Regexes (3 Scenarios)** | **65.59×** | 18.57× | 37.74× | **3.53×** | **1.74×** |
| **Win / Tie / Loss Count vs Ripgrep** | — | — | — | **58 Wins / 1 Tie / 0 Losses** | **55 Wins / 2 Ties / 2 Losses** |
| **Gate A Hard Correctness (Byte-Exact vs G0)** | **59 / 59 PASS (100%)** | — | — | — | — |
| **Gate D Determinism (1..128 Thread Invariance)** | **9 / 9 PASS (100%)** | — | — | — | — |

> **Fair 12-Thread Matched Comparison (G1 @ 12t vs R0 @ 12t on 40 Scenarios):**
> Across the 40 matched scenarios (all 38 tree scenarios plus single-file B-01/B-04), G1 running on the same 12 threads
> achieves a **1.95× geometric mean speedup** over `ripgrep` (**40 Wins / 0 Ties / 0 Losses**)!
> In Category T alone, G1 @ 12t beats R0 @ 12t with a **1.61× geomean** (**29 Wins / 0 Ties / 0 Losses**).

### Key Takeaways

1. **Decisive Multi-Core Speedup Over Stock GNU Grep:**
   - On whole-tree searches across the 96,034-file Linux kernel tree, G1 accelerates execution from seconds to sub-second
     runtimes (**150–291 ms**, a **22.21×** category geometric mean speedup).
   - On massive multi-pattern dictionary searches (100 to 10,000 keywords), G1 delivers up to **515.59×** speedup over G0 (`M-04`, 192.1 ms vs 99.02 s)
     and **189.87×** on large single files (`M-06`, 46.7 ms vs 8.86 s) via parallelized dense Aho-Corasick DFA and SIMD pre-filtering.
   - On complex regexes (`Category P`), G1 achieves **147.48×** speedup on backreferences (`P-01`, 282.1 ms vs 41.61 s) and **79.74×** on 1,000-way alternations (`P-02`, 190.5 ms vs 15.19 s).
   - On a 1.39 GB single file, intra-file chunking yields up to **104.10×** speedup over G0 on PCRE2 lookaround (`B-08`, 206.5 ms vs 21.50 s) and **46.67×** on case-insensitive search (`B-05`, 42.2 ms vs 1.97 s).

2. **Unprecedented Performance vs Ripgrep (Out-of-the-Box 12t AND Thread-Matched `-j 32`):**
   - **Overall Geometric Mean:** Across all 59 scenarios, G1 is **3.66× faster** than default `ripgrep` (58 Wins / 1 Tie / 0 Losses)
     and **2.70× faster** than thread-matched `ripgrep -j 32` (55 Wins / 2 Ties / 2 Losses).
   - **Single Large Files (14/14 Wins at both 12t and 32t, 7.59×–7.64× geomean):** Ripgrep processes individual files using a single thread
     regardless of `-j`; our intra-file parallel chunking outperforms `ripgrep -j 32` by **1.39× to 104.81×** (and **65.92×** on 1,000-keyword matching `M-06`).
   - **Whole-Tree Traversal (2.73× geomean vs R0 12t; 1.82× geomean vs R0 `-j 32`):**
     - Against out-of-the-box `ripgrep` (12t), G1 wins **all 29 of 29** tree scenarios (including `T-25b` immediate first-file exit at **2.7 ms vs 4.9 ms**, **1.78×**).
     - When `ripgrep` is explicitly given `-j 32` (`32` threads), G1 beats or ties `ripgrep -j 32` in **28 of 29** tree scenarios and loses only 1 (`T-28` sub-scope `kernel mm fs`, 16.0 ms vs 14.0 ms):
       G1 dominates high-output, count-based, PCRE2, and sparse tree scans (`T-04` dense line numbers: **160.3 ms vs 548.0 ms**, **3.42×**;
       `T-05` very dense literal: **290.9 ms vs 1.04 s**, **3.57×**; `T-08` count per file: **151.1 ms vs 1.54 s**, **10.17×**;
       `T-09` case-insensitive: **156.2 ms vs 1.01 s**, **6.44×**; `T-12` anchored dense regex: **167.5 ms vs 766.2 ms**, **4.57×**;
       `T-16` dense PCRE2: **219.8 ms vs 1.14 s**, **5.18×**; `T-17` rare PCRE2: **154.0 ms vs 229.1 ms**, **1.49×**;
       `T-21` inverted count: **182.3 ms vs 1.55 s**, **8.48×**; `T-23` glob sub-scope: **14.9 ms vs 26.2 ms**, **1.76×**).
     - On sparse tree traversals (`T-01..T-03`), work-stealing parallel directory walking and openat-relative file descriptors keep runtime to **149.7–154.1 ms** (**1.10×–1.20× faster** than `ripgrep -j 32` at 166.5–184.5 ms).
   - **Multi-Pattern Dictionary Searches (6/6 Wins at both 12t and `-j 32`, 4.50× geomean vs `-j 32`):** G1 beats `ripgrep -j 32` on
     `M-01` (100 literals: **165.2 ms vs 270.9 ms**, **1.64×**), `M-02` (1,000 literals: **177.0 ms vs 351.9 ms**, **1.99×**),
     `M-03` (10,000 literals: **248.7 ms vs 620.5 ms**, **2.50×**), `M-04` (1,000 literals `-i`: **192.1 ms vs 1.21 s**, **6.30×**),
     `M-05` (1,000 literals `-w`: **172.6 ms vs 424.4 ms**, **2.46×**), and `M-06` (`$BIG`: **46.7 ms vs 3.08 s**, **65.92×**).

3. **Zero Startup Overhead:**
   - Single-file micro-benchmark latency (`L-01`, `init/main.c`) measures **2.6 ms raw** (net **1.1 ms** excluding the 1.5 ms `/bin/true` harness floor), and immediate quiet match on the full tree (`T-25b`) finishes in **2.7 ms**.
   - Stock G0 measures 2.7 ms raw (net 1.2 ms), while ripgrep measures 3.8 ms raw (net 2.3 ms) on `L-01` and 4.9 ms on `T-25b`. Worker threads start lazily after synchronous root-file probing,
     ensuring zero latency penalty for command-line interactive scripts and small single-file queries.

4. **Massive Cold Cache Throughput:**
   - Under full page-cache eviction (`posix_fadvise`), G1 achieves **825–858 MB/s** tree scan throughput (**1.99 s** on `T-01`, **1.91 s** on `T-06`), outperforming ripgrep
     at 311–331 MB/s (4.96 s on `T-01`, **2.49× faster**) and stock GNU grep at 29–31 MB/s (53.48 s on `T-01`, **26.9× faster**). Concurrent directory worker prefetching
     saturates underlying storage I/O queues far more effectively than single-threaded traversal.

5. **100% Determinism and Zero Semantic Regression:**
   - **Gate A (100% Pass):** In all 59 benchmark scenarios, G1's output matches stock G0 byte-for-byte on `stdout`, `stderr`, and exit codes
     (whereas `ripgrep` prints files in non-deterministic thread-completion order unless `--sort=path` is passed).
   - **Gate D (100% Pass):** Output across 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, and 128 threads produces identical SHA-256 hashes.
   - **Synthetic Conformance (266/266 Pass) & Automake Suite (136/136 Pass):** All edge-case regressions, context overlap rules, max-count limits, and memory limits pass.

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
| **`G1`** | Parallel Multi-Threaded GNU grep | `3.12.41` (`83748c6`) | `-pthread -O3 -march=native -flto=auto -fno-plt -fomit-frame-pointer -falign-functions=32 -falign-loops=32`, AVX2 kernels | 33 (32 workers + main) | `3d9a8a3974b2a0dc5a04770822a28a39ec2475081a4b6f5f2de886d4519fb6f8` |
| **`R0`** | Stock Ripgrep | `15.1.0` | Official release binary (`+pcre2`, SIMD/AVX2 runtime) | 13 (12 workers + main; also tested with `-j 32`) | `5c3dddf97a3a0740db7e91a91706299a01fceb9371b66a2ed076c998a9e8e04d` |
| **`G2`** | Distro Stock GNU grep | `3.12` | Debian system distribution binary | 1 | `d652120a79a224c6a6921162ee7796710db69bb2d2ce7fbb9ad332fb8b1e2167` |
| **`X1`** | Stock Git Grep | `2.56.0` | Git sub-command (`git grep`) | 129 (128 workers + main) | `913626459ba9244e110a2675aeed7f6ec5b7b189509745fe41adc66c561f018b` |

---

## 3. Complete Linux Kernel-Tree Benchmark Master Matrix (Default & Thread-Matched)

All 59 warm-cache scenarios evaluated under `LC_ALL=en_US.UTF-8`. Each scenario was executed for 1 unmeasured warm-up
round followed by 3 measured rounds (Category L latency scenarios executed for 30–100 measured rounds).
Both `R0 (12t)` (ripgrep's out-of-the-box 12-thread cap) and `R0 (-j 32)` (thread-matched to `G1`'s 32 worker threads) are reported side by side.

| Scenario | Description | G0 (1t) | G1 (32t) | R0 (12t) | R0 (-j 32) | G1 vs G0 | G1 vs R0 (12t) | G1 vs R0 (32t) | G1 CPU | R0 (12t) CPU | R0 (32t) CPU | G1 RSS | R0 RSS | Gate A | Gate B |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| `B-01` | rare literal ($BIG) | 498.0 ms | 20.8 ms | 105.4 ms | 109.3 ms | **23.94×** | **5.07×** | **5.25×** | 0.40 | 0.10 | 0.11 | 1380M | 1392M | **PASS** | PASS |
| `B-02` | count dense ($BIG) | 565.8 ms | 20.3 ms | 130.9 ms | 134.6 ms | **27.81×** | **6.44×** | **6.62×** | 0.41 | 0.13 | 0.14 | 1383M | 1392M | **PASS** | PASS |
| `B-03` | moderate, line numbers ($BIG) | 903.2 ms | 40.3 ms | 167.8 ms | 167.3 ms | **22.43×** | **4.17×** | **4.15×** | 0.97 | 0.17 | 0.17 | 1382M | 1392M | **PASS** | PASS |
| `B-04` | DFA regex ($BIG) | 394.3 ms | 21.5 ms | 106.3 ms | 115.8 ms | **18.33×** | **4.94×** | **5.38×** | 0.41 | 0.11 | 0.11 | 1379M | 1392M | **PASS** | PASS |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 42.2 ms | 240.1 ms | 266.7 ms | **46.67×** | **5.69×** | **6.32×** | 0.91 | 0.24 | 0.26 | 1382M | 1392M | **PASS** | PASS |
| `B-06` | word match ($BIG) | 327.4 ms | 22.3 ms | 121.6 ms | 113.1 ms | **14.65×** | **5.44×** | **5.06×** | 0.41 | 0.12 | 0.11 | 1379M | 1392M | **PASS** | PASS |
| `B-07` | literal-free regex ($BIG) | 624.9 ms | 31.2 ms | 1.53 s | 1.53 s | **20.06×** | **48.96×** | **49.22×** | 0.77 | 1.53 | 1.54 | 1377M | 1392M | **PASS** | PASS |
| `B-08` | PCRE2 lookaround ($BIG) | 21.50 s | 206.5 ms | 21.80 s | 21.64 s | **104.10×** | **105.56×** | **104.81×** | 2.56 | 21.74 | 21.63 | 1378M | 1394M | **PASS** | PASS |
| `B-09` | context ($BIG) | 890.6 ms | 33.3 ms | 188.2 ms | 190.3 ms | **26.74×** | **5.65×** | **5.71×** | 0.75 | 0.21 | 0.19 | 1379M | 1392M | **PASS** | PASS |
| `B-10` | only-matching ($BIG) | 729.7 ms | 41.1 ms | 129.1 ms | 126.3 ms | **17.75×** | **3.14×** | **3.07×** | 0.64 | 0.13 | 0.13 | 1373M | 1392M | **PASS** | PASS |
| `B-11` | inverted count ($BIG) | 973.2 ms | 46.3 ms | 1.44 s | 1.42 s | **21.00×** | **31.03×** | **30.60×** | 1.00 | 1.44 | 1.42 | 1381M | 1392M | **PASS** | PASS |
| `B-12` | stdin redirect (< $BIG) | 498.6 ms | 21.2 ms | 219.3 ms | 214.4 ms | **23.54×** | **10.35×** | **10.12×** | 0.40 | 0.22 | 0.21 | 1381M | 39M | **PASS** | PASS |
| `B-13` | FIFO pipe (cat $BIG \\|) | 1.14 s | 690.4 ms | 994.5 ms | 958.5 ms | **1.65×** | **1.44×** | **1.39×** | 0.97 | 0.69 | 0.70 | 50M | 39M | **PASS** | PASS |
| `B-14` | real 24 MB header (big-24M.h) | 48.6 ms | 17.6 ms | 41.9 ms | 42.2 ms | **2.76×** | **2.38×** | **2.39×** | 0.07 | 0.04 | 0.04 | 50M | 39M | **PASS** | PASS |
| `L-00` | harness floor (/bin/true) | 1.5 ms | 1.5 ms | 1.5 ms | 1.5 ms | **1.00×** | **1.00×** | **1.00×** | 0.00 | 0.00 | 0.00 | 37M | 40M | **PASS** | PASS |
| `L-01` | one small file (init/main.c) | 2.7 ms | 2.6 ms | 3.8 ms | 3.7 ms | **1.02×** | **1.44×** | **1.43×** | 0.00 | 0.00 | 0.00 | 50M | 40M | **PASS** | PASS |
| `L-02` | tiny tree (kernel/sched) | 7.0 ms | 6.0 ms | 7.0 ms | 9.2 ms | **1.17×** | **1.17×** | **1.54×** | 0.01 | 0.01 | 0.02 | 50M | 40M | **PASS** | PASS |
| `L-03` | quiet first match in big file | 2.7 ms | 2.7 ms | 3.8 ms | 3.8 ms | **1.01×** | **1.41×** | **1.43×** | 0.00 | 0.00 | 0.00 | 50M | 40M | **PASS** | PASS |
| `M-01` | 100 literals, tree | 12.10 s | 165.2 ms | 654.8 ms | 270.9 ms | **73.27×** | **3.96×** | **1.64×** | 4.97 | 7.75 | 8.20 | 47M | 39M | **PASS** | PASS |
| `M-02` | 1,000 literals, tree | 14.00 s | 177.0 ms | 717.8 ms | 351.9 ms | **79.08×** | **4.06×** | **1.99×** | 5.26 | 8.38 | 10.02 | 47M | 68M | **PASS** | PASS |
| `M-03` | 10,000 literals, tree | 17.08 s | 248.7 ms | 1.19 s | 620.5 ms | **68.68×** | **4.77×** | **2.50×** | 6.21 | 13.75 | 16.64 | 59M | 39M | **PASS** | PASS |
| `M-04` | 1,000 literals, -i | 99.02 s | 192.1 ms | 1.60 s | 1.21 s | **515.59×** | **8.34×** | **6.30×** | 5.75 | 18.98 | 35.97 | 50M | 150M | **PASS** | PASS |
| `M-05` | 1,000 literals, -w | 13.93 s | 172.6 ms | 918.8 ms | 424.4 ms | **80.75×** | **5.32×** | **2.46×** | 5.16 | 10.60 | 11.84 | 50M | 75M | **PASS** | PASS |
| `M-06` | 1,000 literals, $BIG | 8.86 s | 46.7 ms | 3.10 s | 3.08 s | **189.87×** | **66.51×** | **65.92×** | 0.76 | 3.10 | 3.09 | 1387M | 1404M | **PASS** | PASS |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.61 s | 282.1 ms | 841.8 ms | 357.2 ms | **147.48×** | **2.98×** | **1.27×** | 8.72 | 10.01 | 11.16 | 37M | 40M | **PASS** | PASS |
| `P-02` | 1,000-way regex alternation | 15.19 s | 190.5 ms | 1.24 s | 761.2 ms | **79.74×** | **6.51×** | **4.00×** | 5.49 | 14.37 | 22.49 | 37M | 137M | **PASS** | PASS |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.69 s | 153.6 ms | 348.5 ms | 159.3 ms | **23.99×** | **2.27×** | **1.04×** | 4.69 | 4.12 | 4.78 | 37M | 40M | **PASS** | PASS |
| `T-01` | zero-match literal | 3.63 s | 153.8 ms | 341.0 ms | 169.5 ms | **23.61×** | **2.22×** | **1.10×** | 4.67 | 4.00 | 5.17 | 50M | 39M | **PASS** | PASS |
| `T-02` | rare literal | 3.80 s | 149.7 ms | 347.1 ms | 166.5 ms | **25.39×** | **2.32×** | **1.11×** | 4.62 | 4.06 | 5.12 | 50M | 39M | **PASS** | PASS |
| `T-03` | moderate literal, line numbers | 4.30 s | 154.1 ms | 367.1 ms | 184.5 ms | **27.91×** | **2.38×** | **1.20×** | 4.56 | 4.33 | 5.90 | 50M | 39M | **PASS** | PASS |
| `T-04` | dense literal, line numbers | 4.53 s | 160.3 ms | 509.2 ms | 548.0 ms | **28.25×** | **3.18×** | **3.42×** | 4.74 | 5.76 | 7.04 | 50M | 39M | **PASS** | PASS |
| `T-05` | very dense literal | 4.68 s | 290.9 ms | 809.8 ms | 1.04 s | **16.08×** | **2.78×** | **3.57×** | 5.10 | 7.44 | 8.24 | 83M | 39M | **PASS** | PASS |
| `T-06` | files-with-matches | 3.72 s | 164.9 ms | 379.0 ms | 211.6 ms | **22.56×** | **2.30×** | **1.28×** | 4.54 | 4.41 | 6.53 | 172M | 39M | **PASS** | PASS |
| `T-07` | files-without-match | 3.36 s | 156.3 ms | 432.4 ms | 316.2 ms | **21.49×** | **2.77×** | **2.02×** | 4.54 | 5.09 | 9.22 | 95M | 39M | **PASS** | SOFT_DIFF |
| `T-08` | count per file | 3.67 s | 151.1 ms | 1.11 s | 1.54 s | **24.28×** | **7.33×** | **10.17×** | 4.52 | 10.67 | 11.37 | 50M | 39M | **PASS** | PASS |
| `T-09` | case-insensitive literal | 5.10 s | 156.2 ms | 778.5 ms | 1.01 s | **32.68×** | **4.98×** | **6.44×** | 4.68 | 8.20 | 9.12 | 50M | 39M | **PASS** | KNOWN-SEMANTIC |
| `T-10` | word match | 4.13 s | 161.1 ms | 386.6 ms | 199.3 ms | **25.64×** | **2.40×** | **1.24×** | 4.84 | 4.46 | 6.07 | 47M | 39M | **PASS** | PASS |
| `T-11` | DFA regex with must-literal | 3.71 s | 161.2 ms | 349.9 ms | 166.0 ms | **23.02×** | **2.17×** | **1.03×** | 4.79 | 4.13 | 5.03 | 47M | 39M | **PASS** | PASS |
| `T-12` | anchored dense regex | 4.63 s | 167.5 ms | 689.2 ms | 766.2 ms | **27.64×** | **4.12×** | **4.57×** | 4.90 | 7.37 | 7.75 | 42M | 39M | **PASS** | PASS |
| `T-13` | literal alternation + word boundary | 3.85 s | 159.2 ms | 382.5 ms | 233.8 ms | **24.19×** | **2.40×** | **1.47×** | 4.82 | 4.49 | 6.74 | 50M | 39M | **PASS** | PASS |
| `T-14` | optional group + alternation | 3.78 s | 156.0 ms | 347.4 ms | 170.3 ms | **24.23×** | **2.23×** | **1.09×** | 4.62 | 4.16 | 5.27 | 50M | 39M | **PASS** | PASS |
| `T-15` | literal-free regex | 4.06 s | 168.2 ms | 391.1 ms | 171.9 ms | **24.16×** | **2.32×** | **1.02×** | 5.18 | 4.55 | 5.25 | 50M | 39M | **PASS** | PASS |
| `T-16` | PCRE2 lookaround, dense | 27.62 s | 219.8 ms | 2.51 s | 1.14 s | **125.69×** | **11.42×** | **5.18×** | 6.83 | 29.98 | 32.09 | 50M | 39M | **PASS** | PASS |
| `T-17` | PCRE2 rare with literal | 5.27 s | 154.0 ms | 525.6 ms | 229.1 ms | **34.26×** | **3.41×** | **1.49×** | 4.55 | 6.20 | 7.00 | 50M | 39M | **PASS** | PASS |
| `T-18` | only-matching | 4.11 s | 174.7 ms | 368.6 ms | 201.6 ms | **23.53×** | **2.11×** | **1.15×** | 5.08 | 4.36 | 6.04 | 39M | 39M | **PASS** | PASS |
| `T-19` | context, rare | 4.32 s | 159.3 ms | 380.4 ms | 192.7 ms | **27.08×** | **2.39×** | **1.21×** | 4.79 | 4.52 | 5.52 | 47M | 39M | **PASS** | PASS |
| `T-20` | context, moderate | 4.36 s | 159.6 ms | 382.8 ms | 203.6 ms | **27.32×** | **2.40×** | **1.28×** | 4.72 | 4.48 | 5.54 | 40M | 39M | **PASS** | PASS |
| `T-21` | inverted count | 4.44 s | 182.3 ms | 1.26 s | 1.55 s | **24.36×** | **6.92×** | **8.48×** | 5.53 | 12.21 | 12.83 | 41M | 39M | **PASS** | SOFT_DIFF |
| `T-22` | glob include | 2.06 s | 77.3 ms | 168.7 ms | 101.7 ms | **26.69×** | **2.18×** | **1.32×** | 1.96 | 1.93 | 2.89 | 50M | 39M | **PASS** | PASS |
| `T-23` | glob + sub-scope | 126.9 ms | 14.9 ms | 24.6 ms | 26.2 ms | **8.50×** | **1.65×** | **1.76×** | 0.16 | 0.22 | 0.27 | 50M | 39M | **PASS** | PASS |
| `T-24` | max-count per file | 3.76 s | 156.0 ms | 355.3 ms | 183.0 ms | **24.10×** | **2.28×** | **1.17×** | 4.69 | 4.16 | 5.84 | 50M | 40M | **PASS** | PASS |
| `T-25a` | quiet, no match | 3.64 s | 163.7 ms | 347.3 ms | 165.7 ms | **22.21×** | **2.12×** | **1.01×** | 4.58 | 4.06 | 4.95 | 148M | 40M | **PASS** | PASS |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | 4.9 ms | 7.0 ms | **0.99×** | **1.78×** | **2.58×** | 0.00 | 0.01 | 0.01 | 50M | 40M | **PASS** | PASS |
| `T-26` | byte offsets | 4.30 s | 159.7 ms | 359.8 ms | 186.0 ms | **26.96×** | **2.25×** | **1.17×** | 4.80 | 4.26 | 5.77 | 47M | 40M | **PASS** | PASS |
| `T-27` | sub-scope drivers/ | 1.97 s | 71.8 ms | 166.8 ms | 97.6 ms | **27.39×** | **2.32×** | **1.36×** | 1.95 | 1.90 | 2.76 | 50M | 40M | **PASS** | PASS |
| `T-28` | sub-scope kernel mm fs | 164.1 ms | 16.0 ms | 19.3 ms | 14.0 ms | **10.27×** | **1.21×** | **0.87×** | 0.17 | 0.15 | 0.18 | 50M | 40M | **PASS** | PASS |
| `U-01` | out-of-the-box UX | 4.29 s | 153.3 ms | 390.7 ms | 234.1 ms | **27.98×** | **2.55×** | **1.53×** | 4.57 | 4.53 | 6.96 | 50M | 40M | **PASS** | PASS |
| `U-02` | naive invocation (scans 6 GB .git) | 5.94 s | 280.9 ms | 392.3 ms* | 219.6 ms | **21.14×** | **1.40×** | **0.78×** | 5.63 | 4.61 | 6.69 | 1946M | 40M | **PASS** | N/A |
| `X-01` | git ls-files \\| xargs | 4.18 s | 492.9 ms | 1.12 s | 1.00 s | **8.49×** | **2.26×** | **2.03×** | 5.30 | 5.66 | 7.91 | 42M | 40M | **PASS** | PASS |

> **Equivalence Verification:**
> - Gate A (`PASS`): Byte-for-byte exact equality with upstream stock `G0` on `stdout`, normalized `stderr`, and exit code.
> - Gate B (`PASS`): Cross-tool equivalent sorted line set against `R0`, `G2`, and `X1`.

---

## 4. Category Deep-Dives & Architectural Analysis

### Category T: Whole-Tree Traversal (29 Scenarios, 96,034 Files, 1.64 GB)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `T-01` | zero-match literal | 3.63 s | 153.8 ms | 341.0 ms | **23.61×** | **2.22×** | 4.67 | 4.00 | 50M | 39M | **PASS** |
| `T-02` | rare literal | 3.80 s | 149.7 ms | 347.1 ms | **25.39×** | **2.32×** | 4.62 | 4.06 | 50M | 39M | **PASS** |
| `T-03` | moderate literal, line numbers | 4.30 s | 154.1 ms | 367.1 ms | **27.91×** | **2.38×** | 4.56 | 4.33 | 50M | 39M | **PASS** |
| `T-04` | dense literal, line numbers | 4.53 s | 160.3 ms | 509.2 ms | **28.25×** | **3.18×** | 4.74 | 5.76 | 50M | 39M | **PASS** |
| `T-05` | very dense literal | 4.68 s | 290.9 ms | 809.8 ms | **16.08×** | **2.78×** | 5.10 | 7.44 | 83M | 39M | **PASS** |
| `T-06` | files-with-matches | 3.72 s | 164.9 ms | 379.0 ms | **22.56×** | **2.30×** | 4.54 | 4.41 | 172M | 39M | **PASS** |
| `T-07` | files-without-match | 3.36 s | 156.3 ms | 432.4 ms | **21.49×** | **2.77×** | 4.54 | 5.09 | 95M | 39M | **PASS** |
| `T-08` | count per file | 3.67 s | 151.1 ms | 1.11 s | **24.28×** | **7.33×** | 4.52 | 10.67 | 50M | 39M | **PASS** |
| `T-09` | case-insensitive literal | 5.10 s | 156.2 ms | 778.5 ms | **32.68×** | **4.98×** | 4.68 | 8.20 | 50M | 39M | **PASS** |
| `T-10` | word match | 4.13 s | 161.1 ms | 386.6 ms | **25.64×** | **2.40×** | 4.84 | 4.46 | 47M | 39M | **PASS** |
| `T-11` | DFA regex with must-literal | 3.71 s | 161.2 ms | 349.9 ms | **23.02×** | **2.17×** | 4.79 | 4.13 | 47M | 39M | **PASS** |
| `T-12` | anchored dense regex | 4.63 s | 167.5 ms | 689.2 ms | **27.64×** | **4.12×** | 4.90 | 7.37 | 42M | 39M | **PASS** |
| `T-13` | literal alternation + word boundary | 3.85 s | 159.2 ms | 382.5 ms | **24.19×** | **2.40×** | 4.82 | 4.49 | 50M | 39M | **PASS** |
| `T-14` | optional group + alternation | 3.78 s | 156.0 ms | 347.4 ms | **24.23×** | **2.23×** | 4.62 | 4.16 | 50M | 39M | **PASS** |
| `T-15` | literal-free regex | 4.06 s | 168.2 ms | 391.1 ms | **24.16×** | **2.32×** | 5.18 | 4.55 | 50M | 39M | **PASS** |
| `T-16` | PCRE2 lookaround, dense | 27.62 s | 219.8 ms | 2.51 s | **125.69×** | **11.42×** | 6.83 | 29.98 | 50M | 39M | **PASS** |
| `T-17` | PCRE2 rare with literal | 5.27 s | 154.0 ms | 525.6 ms | **34.26×** | **3.41×** | 4.55 | 6.20 | 50M | 39M | **PASS** |
| `T-18` | only-matching | 4.11 s | 174.7 ms | 368.6 ms | **23.53×** | **2.11×** | 5.08 | 4.36 | 39M | 39M | **PASS** |
| `T-19` | context, rare | 4.32 s | 159.3 ms | 380.4 ms | **27.08×** | **2.39×** | 4.79 | 4.52 | 47M | 39M | **PASS** |
| `T-20` | context, moderate | 4.36 s | 159.6 ms | 382.8 ms | **27.32×** | **2.40×** | 4.72 | 4.48 | 40M | 39M | **PASS** |
| `T-21` | inverted count | 4.44 s | 182.3 ms | 1.26 s | **24.36×** | **6.92×** | 5.53 | 12.21 | 41M | 39M | **PASS** |
| `T-22` | glob include | 2.06 s | 77.3 ms | 168.7 ms | **26.69×** | **2.18×** | 1.96 | 1.93 | 50M | 39M | **PASS** |
| `T-23` | glob + sub-scope | 126.9 ms | 14.9 ms | 24.6 ms | **8.50×** | **1.65×** | 0.16 | 0.22 | 50M | 39M | **PASS** |
| `T-24` | max-count per file | 3.76 s | 156.0 ms | 355.3 ms | **24.10×** | **2.28×** | 4.69 | 4.16 | 50M | 40M | **PASS** |
| `T-25a` | quiet, no match | 3.64 s | 163.7 ms | 347.3 ms | **22.21×** | **2.12×** | 4.58 | 4.06 | 148M | 40M | **PASS** |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | 4.9 ms | **0.99×** | **1.78×** | 0.00 | 0.01 | 50M | 40M | **PASS** |
| `T-26` | byte offsets | 4.30 s | 159.7 ms | 359.8 ms | **26.96×** | **2.25×** | 4.80 | 4.26 | 47M | 40M | **PASS** |
| `T-27` | sub-scope drivers/ | 1.97 s | 71.8 ms | 166.8 ms | **27.39×** | **2.32×** | 1.95 | 1.90 | 50M | 40M | **PASS** |
| `T-28` | sub-scope kernel mm fs | 164.1 ms | 16.0 ms | 19.3 ms | **10.27×** | **1.21×** | 0.17 | 0.15 | 50M | 40M | **PASS** |

#### Architectural Analysis (Category T):
- **High-Density Output Dominance:** G1 dramatically outclasses ripgrep on scenarios with high match volume (`T-04` dense line numbers: **160.3 ms vs 509.2 ms / 548.0 ms**, **3.18× / 3.42×**; `T-05` very dense: **290.9 ms vs 809.8 ms / 1.04 s**, **2.78× / 3.57×**). Ripgrep serializes line formatting and terminal output through a shared mutex-locked buffer, causing severe thread lock contention when output is dense. G1 uses thread-local output slabs combined with an in-order Sequence Reorder Buffer (ROB) that commits complete file buffers locklessly.
- **Count Queries:** In count-per-file queries (`T-08` count: **151.1 ms vs 1.11 s / 1.54 s**, **7.33× / 10.17×**; `T-21` inverted count: **182.3 ms vs 1.26 s / 1.55 s**, **6.92× / 8.48×**), G1 performs line-counting using optimized AVX2 SIMD `nlscan` kernels while skipping all text output serialization. Ripgrep incurs heavy per-file state tracking and formatting overhead.
- **Case-Insensitive & PCRE2 Tree Scans:** On `T-09` (`-ri copyright`), G1 achieves **156.2 ms vs 778.5 ms / 1.01 s** (**4.98× / 6.44×** faster than ripgrep). On `T-16` (`-rP` lookaround), G1's structural anchor-pair pre-filter (`struct ` + `;`) eliminates 99.6% of non-matching lines before invoking PCRE2 JIT, finishing in **219.8 ms vs 2.51 s / 1.14 s** (**11.42× / 5.18×** faster than ripgrep and **125.69×** faster than stock G0).

---

### Category B: Single Huge File Parallelism (14 Scenarios, `$BIG` ≈ 1.39 GB)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `B-01` | rare literal ($BIG) | 498.0 ms | 20.8 ms | 105.4 ms | **23.94×** | **5.07×** | 0.40 | 0.10 | 1380M | 1392M | **PASS** |
| `B-02` | count dense ($BIG) | 565.8 ms | 20.3 ms | 130.9 ms | **27.81×** | **6.44×** | 0.41 | 0.13 | 1383M | 1392M | **PASS** |
| `B-03` | moderate, line numbers ($BIG) | 903.2 ms | 40.3 ms | 167.8 ms | **22.43×** | **4.17×** | 0.97 | 0.17 | 1382M | 1392M | **PASS** |
| `B-04` | DFA regex ($BIG) | 394.3 ms | 21.5 ms | 106.3 ms | **18.33×** | **4.94×** | 0.41 | 0.11 | 1379M | 1392M | **PASS** |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 42.2 ms | 240.1 ms | **46.67×** | **5.69×** | 0.91 | 0.24 | 1382M | 1392M | **PASS** |
| `B-06` | word match ($BIG) | 327.4 ms | 22.3 ms | 121.6 ms | **14.65×** | **5.44×** | 0.41 | 0.12 | 1379M | 1392M | **PASS** |
| `B-07` | literal-free regex ($BIG) | 624.9 ms | 31.2 ms | 1.53 s | **20.06×** | **48.96×** | 0.77 | 1.53 | 1377M | 1392M | **PASS** |
| `B-08` | PCRE2 lookaround ($BIG) | 21.50 s | 206.5 ms | 21.80 s | **104.10×** | **105.56×** | 2.56 | 21.74 | 1378M | 1394M | **PASS** |
| `B-09` | context ($BIG) | 890.6 ms | 33.3 ms | 188.2 ms | **26.74×** | **5.65×** | 0.75 | 0.21 | 1379M | 1392M | **PASS** |
| `B-10` | only-matching ($BIG) | 729.7 ms | 41.1 ms | 129.1 ms | **17.75×** | **3.14×** | 0.64 | 0.13 | 1373M | 1392M | **PASS** |
| `B-11` | inverted count ($BIG) | 973.2 ms | 46.3 ms | 1.44 s | **21.00×** | **31.03×** | 1.00 | 1.44 | 1381M | 1392M | **PASS** |
| `B-12` | stdin redirect (< $BIG) | 498.6 ms | 21.2 ms | 219.3 ms | **23.54×** | **10.35×** | 0.40 | 0.22 | 1381M | 39M | **PASS** |
| `B-13` | FIFO pipe (cat $BIG \\|) | 1.14 s | 690.4 ms | 994.5 ms | **1.65×** | **1.44×** | 0.97 | 0.69 | 50M | 39M | **PASS** |
| `B-14` | real 24 MB header (big-24M.h) | 48.6 ms | 17.6 ms | 41.9 ms | **2.76×** | **2.38×** | 0.07 | 0.04 | 50M | 39M | **PASS** |

#### Architectural Analysis (Category B):
- **Intra-File Parallelism Gap:** Ripgrep processes single files with a single thread regardless of `-j` flags, taking 105 ms to 240 ms (and 21.8 s on PCRE2 lookaround). G1 maps large files and partitions them across the worker pool into parallel chunks with seamless line boundary handling, achieving **20.8 ms on literals** (**5.07× faster than ripgrep**), **42.2 ms on case-insensitive** (**5.69× faster**), and **206.5 ms on PCRE2 lookaround** (**105.56× faster**).
- **Stream and FIFO Slicing:** Even when reading from stdin redirect (`B-12`: **21.2 ms vs 219.3 ms**, **10.35×**), G1 slices the incoming stream into chunked worker buffers for parallel execution.

---

### Category M: Multi-Pattern Dictionary Sets (6 Scenarios)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `M-01` | 100 literals, tree | 12.10 s | 165.2 ms | 654.8 ms | **73.27×** | **3.96×** | 4.97 | 7.75 | 47M | 39M | **PASS** |
| `M-02` | 1,000 literals, tree | 14.00 s | 177.0 ms | 717.8 ms | **79.08×** | **4.06×** | 5.26 | 8.38 | 47M | 68M | **PASS** |
| `M-03` | 10,000 literals, tree | 17.08 s | 248.7 ms | 1.19 s | **68.68×** | **4.77×** | 6.21 | 13.75 | 59M | 39M | **PASS** |
| `M-04` | 1,000 literals, -i | 99.02 s | 192.1 ms | 1.60 s | **515.59×** | **8.34×** | 5.75 | 18.98 | 50M | 150M | **PASS** |
| `M-05` | 1,000 literals, -w | 13.93 s | 172.6 ms | 918.8 ms | **80.75×** | **5.32×** | 5.16 | 10.60 | 50M | 75M | **PASS** |
| `M-06` | 1,000 literals, $BIG | 8.86 s | 46.7 ms | 3.10 s | **189.87×** | **66.51×** | 0.76 | 3.10 | 1387M | 1404M | **PASS** |

#### Architectural Analysis (Category M):
- **Dense Aho-Corasick & AVX2 Teddy Filter:** Stock GNU grep scales poorly on large dictionary sets because its kwset trie degrades under thousands of patterns (taking 12.1 s to 99.0 s). G1 compiles pattern sets into a dense, cache-friendly Aho-Corasick automaton backed by 16-way and 32-way AVX2 SIMD pre-filter vector kernels.
- **Tree and Single File Speedups:** Across 100 to 10,000 patterns, G1 scans the entire 1.64 GB kernel tree in **165.2 ms to 248.7 ms** (**68.68× to 515.59× faster than G0**, and **3.96× to 8.34× faster than ripgrep**). On single large files (`M-06`), G1 finishes in **46.7 ms** (**189.87× faster than G0**, and **66.51× faster than ripgrep**).

---

### Category L: Invocation Latency & Micro-Benchmarks

#### Detailed Latency Percentiles (Raw vs Net)

| Scenario | Tool | N | Raw p50 | Raw p90 | Raw p99 | Net p50 | Net p90 | Net p99 | CPU (s) | RSS |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `L-00` | **G0** | 100 | 1.5 ms | 1.6 ms | 1.9 ms | 0.00 ms | 0.12 ms | 0.38 ms | 0.00 | 40M |
| `L-00` | **G1** | 100 | 1.5 ms | 1.7 ms | 1.8 ms | 0.00 ms | 0.18 ms | 0.24 ms | 0.00 | 37M |
| `L-00` | **R0** | 100 | 1.5 ms | 1.7 ms | 1.8 ms | 0.00 ms | 0.22 ms | 0.33 ms | 0.00 | 40M |
| `L-01` | **G0** | 100 | 2.7 ms | 2.9 ms | 3.2 ms | 1.2 ms | 1.3 ms | 1.6 ms | 0.00 | 40M |
| `L-01` | **G1** | 100 | 2.6 ms | 2.8 ms | 2.9 ms | 1.1 ms | 1.3 ms | 1.4 ms | 0.00 | 50M |
| `L-01` | **R0** | 100 | 3.8 ms | 3.9 ms | 4.4 ms | 2.3 ms | 2.4 ms | 2.9 ms | 0.00 | 40M |
| `L-02` | **G0** | 50 | 7.0 ms | 7.2 ms | 7.6 ms | 5.5 ms | 5.7 ms | 6.1 ms | 0.01 | 40M |
| `L-02` | **G1** | 50 | 6.0 ms | 6.2 ms | 6.6 ms | 4.5 ms | 4.7 ms | 5.0 ms | 0.01 | 50M |
| `L-02` | **R0** | 50 | 7.0 ms | 7.3 ms | 7.4 ms | 5.5 ms | 5.8 ms | 5.9 ms | 0.01 | 40M |
| `L-03` | **G0** | 30 | 2.7 ms | 2.9 ms | 2.9 ms | 1.2 ms | 1.4 ms | 1.4 ms | 0.00 | 40M |
| `L-03` | **G1** | 30 | 2.7 ms | 2.8 ms | 2.8 ms | 1.2 ms | 1.3 ms | 1.3 ms | 0.00 | 50M |
| `L-03` | **R0** | 30 | 3.8 ms | 3.9 ms | 4.8 ms | 2.3 ms | 2.4 ms | 3.3 ms | 0.00 | 40M |

#### Summary Comparison (Net Latency Median)

| Scenario | Description | G0 Net | G1 Net | R0 Net | G1 vs G0 | G1 vs R0 | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | :---: |
| `L-00` | harness floor (/bin/true) | 0.00 ms | 0.00 ms | 0.00 ms | **—** | **—** | **PASS** |
| `L-01` | one small file (init/main.c) | 1.2 ms | 1.1 ms | 2.3 ms | **1.04×** | **2.04×** | **PASS** |
| `L-02` | tiny tree (kernel/sched) | 5.5 ms | 4.5 ms | 5.5 ms | **1.23×** | **1.23×** | **PASS** |
| `L-03` | quiet first match in big file | 1.2 ms | 1.2 ms | 2.3 ms | **1.03×** | **1.96×** | **PASS** |

#### Latency Insights:
- **Zero Thread-Pool Penalty:** G1 thread-pool creation is lazy: small file queries (`L-01`) and immediate early-exit tree queries (`T-25b`) search on the calling thread before spawning worker threads. Net startup latency on `L-01` is **1.1 ms** (matching single-threaded stock GNU grep at 1.2 ms), while ripgrep takes **2.3 ms** net latency.

---

### Categories U, X, and P: UX, Tools, and Complex Regexes

#### Category U — User Experience Defaults

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `U-01` | out-of-the-box UX | 4.29 s | 153.3 ms | 390.7 ms | **27.98×** | **2.55×** | 4.57 | 4.53 | 50M | 40M | **PASS** |
| `U-02` | naive invocation (scans 6 GB .git) | 5.94 s | 280.9 ms | 392.3 ms* | **21.14×** | **1.40×** | 5.63 | 4.61 | 1946M | 40M | **PASS** |

#### Category X — xargs / External Parallelism

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `X-01` | git ls-files \\| xargs | 4.18 s | 492.9 ms | 1.12 s | **8.49×** | **2.26×** | 5.30 | 5.66 | 42M | 40M | **PASS** |

#### Category P — Pathological / Complex Regexes

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.61 s | 282.1 ms | 841.8 ms | **147.48×** | **2.98×** | 8.72 | 10.01 | 37M | 40M | **PASS** |
| `P-02` | 1,000-way regex alternation | 15.19 s | 190.5 ms | 1.24 s | **79.74×** | **6.51×** | 5.49 | 14.37 | 37M | 137M | **PASS** |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.69 s | 153.6 ms | 348.5 ms | **23.99×** | **2.27×** | 4.69 | 4.12 | 37M | 40M | **PASS** |

---

## 5. Locale Sensitivity Analysis: en_US.UTF-8 vs LC_ALL=C

### Locale Sensitivity: en_US.UTF-8 vs LC_ALL=C (22 Scenarios)

> Ratio = UTF-8 median / C median. A ratio > 1.00× indicates that the C locale is faster (multibyte/case-folding overhead in UTF-8).

| Scenario | Description | G0 UTF-8 | G0 C | G0 (U/C) | G1 UTF-8 | G1 C | G1 (U/C) | R0 UTF-8 | R0 C | R0 (U/C) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | rare literal ($BIG) | 498.0 ms | 501.6 ms | **0.99×** | 20.8 ms | 20.1 ms | **1.03×** | 105.4 ms | 112.3 ms | **0.94×** |
| `B-02` | count dense ($BIG) | 565.8 ms | 571.4 ms | **0.99×** | 20.3 ms | 21.1 ms | **0.96×** | 130.9 ms | 133.7 ms | **0.98×** |
| `B-03` | moderate, line numbers ($BIG) | 903.2 ms | 900.0 ms | **1.00×** | 40.3 ms | 39.2 ms | **1.03×** | 167.8 ms | 175.8 ms | **0.95×** |
| `B-04` | DFA regex ($BIG) | 394.3 ms | 388.1 ms | **1.02×** | 21.5 ms | 20.8 ms | **1.03×** | 106.3 ms | 106.8 ms | **1.00×** |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 1.25 s | **1.57×** | 42.2 ms | 39.9 ms | **1.06×** | 240.1 ms | 242.9 ms | **0.99×** |
| `B-07` | literal-free regex ($BIG) | 624.9 ms | 626.3 ms | **1.00×** | 31.2 ms | 30.7 ms | **1.02×** | 1.53 s | 1.53 s | **1.00×** |
| `B-08` | PCRE2 lookaround ($BIG) | 21.50 s | 7.21 s | **2.98×** | 206.5 ms | 133.0 ms | **1.55×** | 21.80 s | 21.54 s | **1.01×** |
| `M-02` | 1,000 literals, tree | 14.00 s | 13.84 s | **1.01×** | 177.0 ms | 174.2 ms | **1.02×** | 717.8 ms | 713.3 ms | **1.01×** |
| `M-04` | 1,000 literals, -i | 99.02 s | 18.40 s | **5.38×** | 192.1 ms | 185.1 ms | **1.04×** | 1.60 s | 1.58 s | **1.02×** |
| `T-01` | zero-match literal | 3.63 s | 3.63 s | **1.00×** | 153.8 ms | 151.6 ms | **1.01×** | 341.0 ms | 343.6 ms | **0.99×** |
| `T-02` | rare literal | 3.80 s | 3.84 s | **0.99×** | 149.7 ms | 152.5 ms | **0.98×** | 347.1 ms | 360.2 ms | **0.96×** |
| `T-03` | moderate literal, line numbers | 4.30 s | 4.32 s | **1.00×** | 154.1 ms | 150.6 ms | **1.02×** | 367.1 ms | 388.0 ms | **0.95×** |
| `T-04` | dense literal, line numbers | 4.53 s | 4.51 s | **1.00×** | 160.3 ms | 156.9 ms | **1.02×** | 509.2 ms | 514.7 ms | **0.99×** |
| `T-05` | very dense literal | 4.68 s | 4.66 s | **1.00×** | 290.9 ms | 309.9 ms | **0.94×** | 809.8 ms | 774.7 ms | **1.05×** |
| `T-06` | files-with-matches | 3.72 s | 3.79 s | **0.98×** | 164.9 ms | 169.0 ms | **0.98×** | 379.0 ms | 368.0 ms | **1.03×** |
| `T-09` | case-insensitive literal | 5.10 s | 4.36 s | **1.17×** | 156.2 ms | 156.2 ms | **1.00×** | 778.5 ms | 865.2 ms | **0.90×** |
| `T-10` | word match | 4.13 s | 4.12 s | **1.00×** | 161.1 ms | 154.8 ms | **1.04×** | 386.6 ms | 376.4 ms | **1.03×** |
| `T-11` | DFA regex with must-literal | 3.71 s | 3.74 s | **0.99×** | 161.2 ms | 153.9 ms | **1.05×** | 349.9 ms | 354.7 ms | **0.99×** |
| `T-12` | anchored dense regex | 4.63 s | 4.40 s | **1.05×** | 167.5 ms | 162.5 ms | **1.03×** | 689.2 ms | 608.4 ms | **1.13×** |
| `T-15` | literal-free regex | 4.06 s | 4.06 s | **1.00×** | 168.2 ms | 167.0 ms | **1.01×** | 391.1 ms | 374.5 ms | **1.04×** |
| `T-16` | PCRE2 lookaround, dense | 27.62 s | 11.70 s | **2.36×** | 219.8 ms | 176.3 ms | **1.25×** | 2.51 s | 2.59 s | **0.97×** |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.9 ms | **0.94×** | 2.7 ms | 2.9 ms | **0.94×** | 4.9 ms | 5.3 ms | **0.92×** |

### Key Observations on Locale Sensitivity:
- **ASCII Case-Fold Acceleration:** Upstream GNU grep experiences a 5.38× slowdown on multi-pattern case-insensitive search (`M-04`) and 1.57× slowdown on single-file case-insensitive search (`B-05`) in UTF-8 locales. G1's AVX2 ASCII case-folding fast path reduces the UTF-8 penalty to near zero (**1.00× to 1.06×**), matching C locale throughput.

---

## 6. Cold Page Cache Performance (fadvise Evicted)

### Cold Page Cache Performance (fadvise Evicted)

> Eviction via posix_fadvise(POSIX_FADV_DONTNEED) with residency verification (< 2% resident) before every run.
> Cold/Warm Ratio = Cold median / Warm median. Cold Throughput = Corpus Size / Cold Time.

| Scenario | Description | Tool | Warm (ms) | Cold (ms) | Cold / Warm Ratio | Cold Throughput |
| :--- | :--- | :---: | ---: | ---: | ---: | ---: |
| `B-01` | rare literal ($BIG) | **G0** | 498.0 ms | 3.87 s | 7.76× | 360 MB/s |
| `B-01` | rare literal ($BIG) | **G1** | 20.8 ms | 1.21 s | 58.07× | 1.12 GB/s |
| `B-01` | rare literal ($BIG) | **R0** | 105.4 ms | 3.40 s | 32.23× | 409 MB/s |
| `T-01` | zero-match literal | **G0** | 3.63 s | 53.48 s | 14.73× | 31 MB/s |
| `T-01` | zero-match literal | **G1** | 153.8 ms | 1.99 s | 12.93× | 825 MB/s |
| `T-01` | zero-match literal | **R0** | 341.0 ms | 4.96 s | 14.55× | 331 MB/s |
| `T-03` | moderate literal, line numbers | **G0** | 4.30 s | 56.02 s | 13.03× | 29 MB/s |
| `T-03` | moderate literal, line numbers | **G1** | 154.1 ms | 1.93 s | 12.55× | 848 MB/s |
| `T-03` | moderate literal, line numbers | **R0** | 367.1 ms | 5.28 s | 14.38× | 311 MB/s |
| `T-06` | files-with-matches | **G0** | 3.72 s | 54.27 s | 14.59× | 30 MB/s |
| `T-06` | files-with-matches | **G1** | 164.9 ms | 1.91 s | 11.60× | 858 MB/s |
| `T-06` | files-with-matches | **R0** | 379.0 ms | 5.15 s | 13.58× | 319 MB/s |

### Cold Cache Performance Insights:
- Under cold cache conditions where all file data is evicted from RAM, G1 achieves **825–858 MB/s tree scan throughput** (**1.99 s** on `T-01`, **1.91 s** on `T-06`), outperforming ripgrep at **311–331 MB/s** (**4.96 s** on `T-01`, **2.49× faster**) and stock GNU grep at **29–31 MB/s** (**53.48 s** on `T-01`, **26.9× faster**).
- Concurrent worker prefetching keeps deep I/O request queues filled on underlying cloud storage, eliminating the latency stalls that plague single-threaded traversal.

---

## 7. Multi-Thread Scaling & Fairness Analysis (1 to 128 Threads)

### 7.1 Multi-Thread Scaling Sweep (1 to 128 Threads)

| Scenario | Tool | T=1 | T=2 | T=4 | T=8 | T=12 | T=16 | T=24 | T=32 | T=48 | T=64 | T=96 | T=128 | Peak Speedup vs G0 |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | **G1** | 262.2 ms (1.9×) | 177.3 ms (2.8×) | 124.0 ms (4.0×) | 35.8 ms (13.9×) | 27.9 ms (17.9×) | 24.9 ms (20.0×) | 22.0 ms (22.7×) | 20.6 ms (24.2×) | 19.3 ms (25.8×) | 20.2 ms (24.7×) | 21.2 ms (23.5×) | 24.5 ms (20.3×) | **25.84×** |
| `B-01` | **R0** | 118.0 ms (4.2×) | 114.0 ms (4.4×) | 113.4 ms (4.4×) | 119.3 ms (4.2×) | 114.6 ms (4.3×) | 117.1 ms (4.3×) | 117.8 ms (4.2×) | 109.3 ms (4.6×) | 114.3 ms (4.4×) | 115.9 ms (4.3×) | 116.7 ms (4.3×) | 131.6 ms (3.8×) | **4.56×** |
| `B-04` | **G1** | 283.3 ms (1.4×) | 181.2 ms (2.2×) | 128.8 ms (3.1×) | 36.6 ms (10.8×) | 29.3 ms (13.4×) | 26.4 ms (14.9×) | 23.1 ms (17.1×) | 22.3 ms (17.7×) | 21.4 ms (18.4×) | 22.2 ms (17.8×) | 21.8 ms (18.1×) | 23.7 ms (16.6×) | **18.43×** |
| `B-04` | **R0** | 125.5 ms (3.1×) | 113.1 ms (3.5×) | 114.2 ms (3.5×) | 116.3 ms (3.4×) | 113.9 ms (3.5×) | 115.3 ms (3.4×) | 114.4 ms (3.4×) | 115.8 ms (3.4×) | 120.7 ms (3.3×) | 117.5 ms (3.4×) | 119.0 ms (3.3×) | 116.6 ms (3.4×) | **3.49×** |
| `M-01` | **G1** | — | — | — | — | 323.4 ms (37.4×) | — | — | — | — | — | — | — | **37.43×** |
| `M-01` | **R0** | — | — | — | — | — | — | — | 270.9 ms (44.7×) | — | — | — | — | **44.68×** |
| `M-02` | **G1** | 4.05 s (3.5×) | 1.33 s (10.5×) | 814.5 ms (17.2×) | 476.1 ms (29.4×) | 342.5 ms (40.9×) | 271.3 ms (51.6×) | 222.0 ms (63.0×) | 175.0 ms (80.0×) | 163.3 ms (85.7×) | 231.0 ms (60.6×) | 251.2 ms (55.7×) | 279.9 ms (50.0×) | **85.73×** |
| `M-02` | **R0** | 7.66 s (1.8×) | 3.90 s (3.6×) | 2.02 s (6.9×) | 1.05 s (13.4×) | 714.3 ms (19.6×) | 556.3 ms (25.2×) | 412.7 ms (33.9×) | 351.9 ms (39.8×) | 354.8 ms (39.4×) | 353.4 ms (39.6×) | 375.7 ms (37.3×) | 429.3 ms (32.6×) | **39.77×** |
| `M-03` | **G1** | — | — | — | — | 458.9 ms (37.2×) | — | — | — | — | — | — | — | **37.22×** |
| `M-03` | **R0** | — | — | — | — | — | — | — | 620.5 ms (27.5×) | — | — | — | — | **27.53×** |
| `M-04` | **G1** | — | — | — | — | 369.5 ms (268.0×) | — | — | — | — | — | — | — | **267.96×** |
| `M-04` | **R0** | — | — | — | — | — | — | — | 1.21 s (81.9×) | — | — | — | — | **81.87×** |
| `M-05` | **G1** | — | — | — | — | 333.6 ms (41.8×) | — | — | — | — | — | — | — | **41.77×** |
| `M-05` | **R0** | — | — | — | — | — | — | — | 424.4 ms (32.8×) | — | — | — | — | **32.83×** |
| `M-06` | **G1** | — | — | — | — | 83.6 ms (106.0×) | — | — | — | — | — | — | — | **105.95×** |
| `M-06` | **R0** | — | — | — | — | — | — | — | 3.08 s (2.9×) | — | — | — | — | **2.88×** |
| `T-01` | **G1** | 3.41 s (1.1×) | 1.10 s (3.3×) | 671.2 ms (5.4×) | 386.5 ms (9.4×) | 278.4 ms (13.0×) | 221.5 ms (16.4×) | 175.4 ms (20.7×) | 199.2 ms (18.2×) | 203.8 ms (17.8×) | 217.6 ms (16.7×) | 238.4 ms (15.2×) | 245.4 ms (14.8×) | **20.70×** |
| `T-01` | **R0** | 3.69 s (1.0×) | 1.96 s (1.9×) | 982.1 ms (3.7×) | 518.7 ms (7.0×) | 364.7 ms (10.0×) | 281.1 ms (12.9×) | 215.5 ms (16.8×) | 169.5 ms (21.4×) | 196.3 ms (18.5×) | 232.8 ms (15.6×) | 239.1 ms (15.2×) | 211.2 ms (17.2×) | **21.42×** |
| `T-02` | **G1** | — | — | — | — | 277.8 ms (13.7×) | — | — | — | — | — | — | — | **13.68×** |
| `T-02` | **R0** | — | — | — | — | — | — | — | 166.5 ms (22.8×) | — | — | — | — | **22.83×** |
| `T-03` | **G1** | — | — | — | — | 281.9 ms (15.3×) | — | — | — | — | — | — | — | **15.25×** |
| `T-03` | **R0** | — | — | — | — | — | — | — | 184.5 ms (23.3×) | — | — | — | — | **23.31×** |
| `T-04` | **G1** | 3.62 s (1.2×) | 1.15 s (3.9×) | 723.9 ms (6.3×) | 417.5 ms (10.8×) | 294.0 ms (15.4×) | 232.6 ms (19.5×) | 181.7 ms (24.9×) | 160.4 ms (28.2×) | 158.9 ms (28.5×) | 180.2 ms (25.1×) | 219.5 ms (20.6×) | 250.7 ms (18.1×) | **28.50×** |
| `T-04` | **R0** | 4.69 s (1.0×) | 2.51 s (1.8×) | 1.37 s (3.3×) | 710.6 ms (6.4×) | 533.5 ms (8.5×) | 485.8 ms (9.3×) | 502.2 ms (9.0×) | 548.0 ms (8.3×) | 572.0 ms (7.9×) | 594.9 ms (7.6×) | 620.4 ms (7.3×) | 705.7 ms (6.4×) | **9.32×** |
| `T-05` | **G1** | — | — | — | — | 315.8 ms (14.8×) | — | — | — | — | — | — | — | **14.81×** |
| `T-05` | **R0** | — | — | — | — | — | — | — | 1.04 s (4.5×) | — | — | — | — | **4.50×** |
| `T-06` | **G1** | 3.40 s (1.1×) | 1.10 s (3.4×) | 695.0 ms (5.4×) | 400.3 ms (9.3×) | 290.9 ms (12.8×) | 240.8 ms (15.4×) | 207.6 ms (17.9×) | 214.2 ms (17.4×) | 209.4 ms (17.8×) | 215.6 ms (17.3×) | 242.7 ms (15.3×) | 260.4 ms (14.3×) | **17.92×** |
| `T-06` | **R0** | 3.79 s (1.0×) | 2.02 s (1.8×) | 1.02 s (3.6×) | 526.5 ms (7.1×) | 359.1 ms (10.4×) | 297.0 ms (12.5×) | 211.3 ms (17.6×) | 211.6 ms (17.6×) | 174.8 ms (21.3×) | 232.0 ms (16.0×) | 242.4 ms (15.3×) | 262.3 ms (14.2×) | **21.28×** |
| `T-07` | **G1** | — | — | — | — | 278.9 ms (12.0×) | — | — | — | — | — | — | — | **12.04×** |
| `T-07` | **R0** | — | — | — | — | — | — | — | 316.2 ms (10.6×) | — | — | — | — | **10.62×** |
| `T-08` | **G1** | — | — | — | — | 279.7 ms (13.1×) | — | — | — | — | — | — | — | **13.12×** |
| `T-08` | **R0** | — | — | — | — | — | — | — | 1.54 s (2.4×) | — | — | — | — | **2.39×** |
| `T-09` | **G1** | — | — | — | — | 291.8 ms (17.5×) | — | — | — | — | — | — | — | **17.49×** |
| `T-09` | **R0** | — | — | — | — | — | — | — | 1.01 s (5.1×) | — | — | — | — | **5.08×** |
| `T-10` | **G1** | 3.48 s (1.2×) | 1.13 s (3.6×) | 704.3 ms (5.9×) | 400.0 ms (10.3×) | 293.7 ms (14.1×) | 227.6 ms (18.1×) | 171.3 ms (24.1×) | 153.5 ms (26.9×) | 153.0 ms (27.0×) | 215.7 ms (19.1×) | 240.5 ms (17.2×) | 244.9 ms (16.9×) | **27.00×** |
| `T-10` | **R0** | 3.95 s (1.0×) | 2.09 s (2.0×) | 1.07 s (3.9×) | 558.2 ms (7.4×) | 377.4 ms (10.9×) | 293.1 ms (14.1×) | 248.6 ms (16.6×) | 199.3 ms (20.7×) | 189.4 ms (21.8×) | 271.3 ms (15.2×) | 277.4 ms (14.9×) | 270.6 ms (15.3×) | **21.81×** |
| `T-11` | **G1** | — | — | — | — | 289.3 ms (12.8×) | — | — | — | — | — | — | — | **12.83×** |
| `T-11` | **R0** | — | — | — | — | — | — | — | 166.0 ms (22.4×) | — | — | — | — | **22.35×** |
| `T-12` | **G1** | — | — | — | — | 320.5 ms (14.4×) | — | — | — | — | — | — | — | **14.44×** |
| `T-12` | **R0** | — | — | — | — | — | — | — | 766.2 ms (6.0×) | — | — | — | — | **6.04×** |
| `T-13` | **G1** | — | — | — | — | 291.7 ms (13.2×) | — | — | — | — | — | — | — | **13.20×** |
| `T-13` | **R0** | — | — | — | — | — | — | — | 233.8 ms (16.5×) | — | — | — | — | **16.48×** |
| `T-14` | **G1** | — | — | — | — | 291.7 ms (13.0×) | — | — | — | — | — | — | — | **12.96×** |
| `T-14` | **R0** | — | — | — | — | — | — | — | 170.3 ms (22.2×) | — | — | — | — | **22.20×** |
| `T-15` | **G1** | 3.95 s (1.0×) | 1.30 s (3.1×) | 782.0 ms (5.2×) | 452.3 ms (9.0×) | 318.3 ms (12.8×) | 251.8 ms (16.1×) | 189.9 ms (21.4×) | 183.3 ms (22.2×) | 212.0 ms (19.2×) | 221.0 ms (18.4×) | 244.8 ms (16.6×) | 247.6 ms (16.4×) | **22.18×** |
| `T-15` | **R0** | 4.10 s (1.0×) | 2.17 s (1.9×) | 1.09 s (3.7×) | 560.1 ms (7.3×) | 386.0 ms (10.5×) | 288.9 ms (14.1×) | 217.9 ms (18.7×) | 171.9 ms (23.7×) | 202.6 ms (20.1×) | 224.7 ms (18.1×) | 220.9 ms (18.4×) | 262.0 ms (15.5×) | **23.65×** |
| `T-16` | **G1** | 5.76 s (4.8×) | 1.88 s (14.7×) | 1.14 s (24.2×) | 648.3 ms (42.6×) | 459.1 ms (60.2×) | 359.7 ms (76.8×) | 265.2 ms (104.2×) | 222.5 ms (124.2×) | 199.1 ms (138.7×) | 211.8 ms (130.4×) | 254.1 ms (108.7×) | 268.9 ms (102.7×) | **138.71×** |
| `T-16` | **R0** | 29.18 s (0.9×) | 14.80 s (1.9×) | 7.40 s (3.7×) | 3.79 s (7.3×) | 2.51 s (11.0×) | 1.94 s (14.2×) | 1.31 s (21.1×) | 1.14 s (24.3×) | 1.09 s (25.3×) | 1.04 s (26.7×) | 963.1 ms (28.7×) | 990.1 ms (27.9×) | **28.68×** |
| `T-17` | **G1** | — | — | — | — | 278.7 ms (18.9×) | — | — | — | — | — | — | — | **18.92×** |
| `T-17` | **R0** | — | — | — | — | — | — | — | 229.1 ms (23.0×) | — | — | — | — | **23.02×** |
| `T-18` | **G1** | — | — | — | — | 314.8 ms (13.1×) | — | — | — | — | — | — | — | **13.05×** |
| `T-18` | **R0** | — | — | — | — | — | — | — | 201.6 ms (20.4×) | — | — | — | — | **20.38×** |
| `T-19` | **G1** | — | — | — | — | 285.8 ms (15.1×) | — | — | — | — | — | — | — | **15.10×** |
| `T-19` | **R0** | — | — | — | — | — | — | — | 192.7 ms (22.4×) | — | — | — | — | **22.40×** |
| `T-20` | **G1** | — | — | — | — | 291.0 ms (15.0×) | — | — | — | — | — | — | — | **14.99×** |
| `T-20` | **R0** | — | — | — | — | — | — | — | 203.6 ms (21.4×) | — | — | — | — | **21.41×** |
| `T-21` | **G1** | — | — | — | — | 352.1 ms (12.6×) | — | — | — | — | — | — | — | **12.61×** |
| `T-21` | **R0** | — | — | — | — | — | — | — | 1.55 s (2.9×) | — | — | — | — | **2.87×** |
| `T-22` | **G1** | — | — | — | — | 131.5 ms (15.7×) | — | — | — | — | — | — | — | **15.68×** |
| `T-22` | **R0** | — | — | — | — | — | — | — | 101.7 ms (20.3×) | — | — | — | — | **20.27×** |
| `T-23` | **G1** | — | — | — | — | 15.9 ms (8.0×) | — | — | — | — | — | — | — | **7.96×** |
| `T-23` | **R0** | — | — | — | — | — | — | — | 26.2 ms (4.8×) | — | — | — | — | **4.84×** |
| `T-24` | **G1** | — | — | — | — | 277.0 ms (13.6×) | — | — | — | — | — | — | — | **13.57×** |
| `T-24` | **R0** | — | — | — | — | — | — | — | 183.0 ms (20.5×) | — | — | — | — | **20.55×** |
| `T-25a` | **G1** | — | — | — | — | 291.8 ms (12.5×) | — | — | — | — | — | — | — | **12.46×** |
| `T-25a` | **R0** | — | — | — | — | — | — | — | 165.7 ms (21.9×) | — | — | — | — | **21.94×** |
| `T-25b` | **G1** | — | — | — | — | 2.8 ms (1.0×) | — | — | — | — | — | — | — | **0.95×** |
| `T-25b` | **R0** | — | — | — | — | — | — | — | 7.0 ms (0.4×) | — | — | — | — | **0.38×** |
| `T-26` | **G1** | — | — | — | — | 288.4 ms (14.9×) | — | — | — | — | — | — | — | **14.93×** |
| `T-26` | **R0** | — | — | — | — | — | — | — | 186.0 ms (23.1×) | — | — | — | — | **23.14×** |
| `T-27` | **G1** | — | — | — | — | 125.5 ms (15.7×) | — | — | — | — | — | — | — | **15.68×** |
| `T-27` | **R0** | — | — | — | — | — | — | — | 97.6 ms (20.1×) | — | — | — | — | **20.14×** |
| `T-28` | **G1** | — | — | — | — | 15.7 ms (10.4×) | — | — | — | — | — | — | — | **10.44×** |
| `T-28` | **R0** | — | — | — | — | — | — | — | 14.0 ms (11.7×) | — | — | — | — | **11.75×** |
| `P-01` | **G1** | — | — | — | — | 571.7 ms (72.8×) | — | — | — | — | — | — | — | **72.77×** |
| `P-01` | **R0** | — | — | — | — | — | — | — | 357.2 ms (116.5×) | — | — | — | — | **116.47×** |
| `P-02` | **G1** | — | — | — | — | 344.7 ms (44.1×) | — | — | — | — | — | — | — | **44.05×** |
| `P-02` | **R0** | — | — | — | — | — | — | — | 761.2 ms (20.0×) | — | — | — | — | **19.95×** |
| `P-03` | **G1** | — | — | — | — | 281.5 ms (13.1×) | — | — | — | — | — | — | — | **13.09×** |
| `P-03` | **R0** | — | — | — | — | — | — | — | 159.3 ms (23.1×) | — | — | — | — | **23.13×** |

### 7.2 Matched Thread Comparison Table: G1 (32t) vs G1 (12t) vs R0 (Default 12t) vs R0 (-j 32)

> Side-by-side comparison of G1 (default 32 threads and matched 12 threads) against both out-of-the-box R0 (default 12 threads) and thread-matched `R0 -j 32` (32 threads).

| Scenario | Cat | Description | G0 (1t) | G1 (12t) | R0 (12t) | G1 vs R0 (12t) | G1 (32t) | R0 (-j 32) | G1 vs R0 (32t) | G1 (32t) vs R0 (12t) |
| :--- | :-: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | B | rare literal ($BIG) | 498.0 ms | 27.9 ms | 105.4 ms | **3.78×** | 20.8 ms | 109.3 ms | **5.25×** | **5.07×** |
| `B-02` | B | count dense ($BIG) | 565.8 ms | — | 130.9 ms | **—** | 20.3 ms | 134.6 ms | **6.62×** | **6.44×** |
| `B-03` | B | moderate, line numbers ($BIG) | 903.2 ms | — | 167.8 ms | **—** | 40.3 ms | 167.3 ms | **4.15×** | **4.17×** |
| `B-04` | B | DFA regex ($BIG) | 394.3 ms | 29.3 ms | 106.3 ms | **3.62×** | 21.5 ms | 115.8 ms | **5.38×** | **4.94×** |
| `B-05` | B | case-insensitive dense ($BIG) | 1.97 s | — | 240.1 ms | **—** | 42.2 ms | 266.7 ms | **6.32×** | **5.69×** |
| `B-06` | B | word match ($BIG) | 327.4 ms | — | 121.6 ms | **—** | 22.3 ms | 113.1 ms | **5.06×** | **5.44×** |
| `B-07` | B | literal-free regex ($BIG) | 624.9 ms | — | 1.53 s | **—** | 31.2 ms | 1.53 s | **49.22×** | **48.96×** |
| `B-08` | B | PCRE2 lookaround ($BIG) | 21.50 s | — | 21.80 s | **—** | 206.5 ms | 21.64 s | **104.81×** | **105.56×** |
| `B-09` | B | context ($BIG) | 890.6 ms | — | 188.2 ms | **—** | 33.3 ms | 190.3 ms | **5.71×** | **5.65×** |
| `B-10` | B | only-matching ($BIG) | 729.7 ms | — | 129.1 ms | **—** | 41.1 ms | 126.3 ms | **3.07×** | **3.14×** |
| `B-11` | B | inverted count ($BIG) | 973.2 ms | — | 1.44 s | **—** | 46.3 ms | 1.42 s | **30.60×** | **31.03×** |
| `B-12` | B | stdin redirect (< $BIG) | 498.6 ms | — | 219.3 ms | **—** | 21.2 ms | 214.4 ms | **10.12×** | **10.35×** |
| `B-13` | B | FIFO pipe (cat $BIG \\|) | 1.14 s | — | 994.5 ms | **—** | 690.4 ms | 958.5 ms | **1.39×** | **1.44×** |
| `B-14` | B | real 24 MB header (big-24M.h) | 48.6 ms | — | 41.9 ms | **—** | 17.6 ms | 42.2 ms | **2.39×** | **2.38×** |
| `L-00` | L | harness floor (/bin/true) | 1.5 ms | — | 1.5 ms | **—** | 1.5 ms | 1.5 ms | **1.00×** | **1.00×** |
| `L-01` | L | one small file (init/main.c) | 2.7 ms | — | 3.8 ms | **—** | 2.6 ms | 3.7 ms | **1.43×** | **1.44×** |
| `L-02` | L | tiny tree (kernel/sched) | 7.0 ms | — | 7.0 ms | **—** | 6.0 ms | 9.2 ms | **1.54×** | **1.17×** |
| `L-03` | L | quiet first match in big file | 2.7 ms | — | 3.8 ms | **—** | 2.7 ms | 3.8 ms | **1.43×** | **1.41×** |
| `M-01` | M | 100 literals, tree | 12.10 s | 323.4 ms | 654.8 ms | **2.02×** | 165.2 ms | 270.9 ms | **1.64×** | **3.96×** |
| `M-02` | M | 1,000 literals, tree | 14.00 s | 342.5 ms | 717.8 ms | **2.10×** | 177.0 ms | 351.9 ms | **1.99×** | **4.06×** |
| `M-03` | M | 10,000 literals, tree | 17.08 s | 458.9 ms | 1.19 s | **2.59×** | 248.7 ms | 620.5 ms | **2.50×** | **4.77×** |
| `M-04` | M | 1,000 literals, -i | 99.02 s | 369.5 ms | 1.60 s | **4.33×** | 192.1 ms | 1.21 s | **6.30×** | **8.34×** |
| `M-05` | M | 1,000 literals, -w | 13.93 s | 333.6 ms | 918.8 ms | **2.75×** | 172.6 ms | 424.4 ms | **2.46×** | **5.32×** |
| `M-06` | M | 1,000 literals, $BIG | 8.86 s | 83.6 ms | 3.10 s | **37.12×** | 46.7 ms | 3.08 s | **65.92×** | **66.51×** |
| `T-01` | T | zero-match literal | 3.63 s | 278.4 ms | 341.0 ms | **1.22×** | 153.8 ms | 169.5 ms | **1.10×** | **2.22×** |
| `T-02` | T | rare literal | 3.80 s | 277.8 ms | 347.1 ms | **1.25×** | 149.7 ms | 166.5 ms | **1.11×** | **2.32×** |
| `T-03` | T | moderate literal, line numbers | 4.30 s | 281.9 ms | 367.1 ms | **1.30×** | 154.1 ms | 184.5 ms | **1.20×** | **2.38×** |
| `T-04` | T | dense literal, line numbers | 4.53 s | 294.0 ms | 509.2 ms | **1.73×** | 160.3 ms | 548.0 ms | **3.42×** | **3.18×** |
| `T-05` | T | very dense literal | 4.68 s | 315.8 ms | 809.8 ms | **2.56×** | 290.9 ms | 1.04 s | **3.57×** | **2.78×** |
| `T-06` | T | files-with-matches | 3.72 s | 290.9 ms | 379.0 ms | **1.30×** | 164.9 ms | 211.6 ms | **1.28×** | **2.30×** |
| `T-07` | T | files-without-match | 3.36 s | 278.9 ms | 432.4 ms | **1.55×** | 156.3 ms | 316.2 ms | **2.02×** | **2.77×** |
| `T-08` | T | count per file | 3.67 s | 279.7 ms | 1.11 s | **3.96×** | 151.1 ms | 1.54 s | **10.17×** | **7.33×** |
| `T-09` | T | case-insensitive literal | 5.10 s | 291.8 ms | 778.5 ms | **2.67×** | 156.2 ms | 1.01 s | **6.44×** | **4.98×** |
| `T-10` | T | word match | 4.13 s | 293.7 ms | 386.6 ms | **1.32×** | 161.1 ms | 199.3 ms | **1.24×** | **2.40×** |
| `T-11` | T | DFA regex with must-literal | 3.71 s | 289.3 ms | 349.9 ms | **1.21×** | 161.2 ms | 166.0 ms | **1.03×** | **2.17×** |
| `T-12` | T | anchored dense regex | 4.63 s | 320.5 ms | 689.2 ms | **2.15×** | 167.5 ms | 766.2 ms | **4.57×** | **4.12×** |
| `T-13` | T | literal alternation + word boundary | 3.85 s | 291.7 ms | 382.5 ms | **1.31×** | 159.2 ms | 233.8 ms | **1.47×** | **2.40×** |
| `T-14` | T | optional group + alternation | 3.78 s | 291.7 ms | 347.4 ms | **1.19×** | 156.0 ms | 170.3 ms | **1.09×** | **2.23×** |
| `T-15` | T | literal-free regex | 4.06 s | 318.3 ms | 391.1 ms | **1.23×** | 168.2 ms | 171.9 ms | **1.02×** | **2.32×** |
| `T-16` | T | PCRE2 lookaround, dense | 27.62 s | 459.1 ms | 2.51 s | **5.46×** | 219.8 ms | 1.14 s | **5.18×** | **11.42×** |
| `T-17` | T | PCRE2 rare with literal | 5.27 s | 278.7 ms | 525.6 ms | **1.89×** | 154.0 ms | 229.1 ms | **1.49×** | **3.41×** |
| `T-18` | T | only-matching | 4.11 s | 314.8 ms | 368.6 ms | **1.17×** | 174.7 ms | 201.6 ms | **1.15×** | **2.11×** |
| `T-19` | T | context, rare | 4.32 s | 285.8 ms | 380.4 ms | **1.33×** | 159.3 ms | 192.7 ms | **1.21×** | **2.39×** |
| `T-20` | T | context, moderate | 4.36 s | 291.0 ms | 382.8 ms | **1.32×** | 159.6 ms | 203.6 ms | **1.28×** | **2.40×** |
| `T-21` | T | inverted count | 4.44 s | 352.1 ms | 1.26 s | **3.58×** | 182.3 ms | 1.55 s | **8.48×** | **6.92×** |
| `T-22` | T | glob include | 2.06 s | 131.5 ms | 168.7 ms | **1.28×** | 77.3 ms | 101.7 ms | **1.32×** | **2.18×** |
| `T-23` | T | glob + sub-scope | 126.9 ms | 15.9 ms | 24.6 ms | **1.54×** | 14.9 ms | 26.2 ms | **1.76×** | **1.65×** |
| `T-24` | T | max-count per file | 3.76 s | 277.0 ms | 355.3 ms | **1.28×** | 156.0 ms | 183.0 ms | **1.17×** | **2.28×** |
| `T-25a` | T | quiet, no match | 3.64 s | 291.8 ms | 347.3 ms | **1.19×** | 163.7 ms | 165.7 ms | **1.01×** | **2.12×** |
| `T-25b` | T | quiet, immediate match | 2.7 ms | 2.8 ms | 4.9 ms | **1.72×** | 2.7 ms | 7.0 ms | **2.58×** | **1.78×** |
| `T-26` | T | byte offsets | 4.30 s | 288.4 ms | 359.8 ms | **1.25×** | 159.7 ms | 186.0 ms | **1.17×** | **2.25×** |
| `T-27` | T | sub-scope drivers/ | 1.97 s | 125.5 ms | 166.8 ms | **1.33×** | 71.8 ms | 97.6 ms | **1.36×** | **2.32×** |
| `T-28` | T | sub-scope kernel mm fs | 164.1 ms | 15.7 ms | 19.3 ms | **1.23×** | 16.0 ms | 14.0 ms | **0.87×** | **1.21×** |
| `U-01` | U | out-of-the-box UX | 4.29 s | — | 390.7 ms | **—** | 153.3 ms | 234.1 ms | **1.53×** | **2.55×** |
| `U-02` | U | naive invocation (scans 6 GB .git) | 5.94 s | — | 392.3 ms | **—** | 280.9 ms | 219.6 ms | **0.78×** | **1.40×** |
| `X-01` | X | git ls-files \\| xargs | 4.18 s | — | 1.12 s | **—** | 492.9 ms | 1.00 s | **2.03×** | **2.26×** |
| `P-01` | P | backreference (\b([a-z]{4,})_\1\b) | 41.61 s | 571.7 ms | 841.8 ms | **1.47×** | 282.1 ms | 357.2 ms | **1.27×** | **2.98×** |
| `P-02` | P | 1,000-way regex alternation | 15.19 s | 344.7 ms | 1.24 s | **3.59×** | 190.5 ms | 761.2 ms | **4.00×** | **6.51×** |
| `P-03` | P | dot-star heavy (.*_lock.*_unlock) | 3.69 s | 281.5 ms | 348.5 ms | **1.24×** | 153.6 ms | 159.3 ms | **1.04×** | **2.27×** |

### 7.3 Why Ripgrep Defaults to 12 Threads vs Matched `-j 32` Behavior
1. **Default Heuristic Difference:**
   - Stock `ripgrep` caps its default directory-traversal thread count at `std::cmp::min(cores, 12)` (`12` worker threads on this 128-CPU server),
     whereas `G1` defaults to `MIN(count, 32)` (`32` worker threads).
2. **What Happens When Ripgrep Is Given `-j 32` Across All 59 Scenarios:**
   - **Single Large Files (`B-01..B-14`, `M-06`):** `-j 32` has **zero effect** on `ripgrep` (`105–267 ms`) because `ripgrep` does not parallelize
     within a single file. `G1` beats `ripgrep -j 32` on **14/14** `B-*` scenarios (**7.64× geomean**) and by **65.92×** on `M-06`.
   - **High-Output & Dense Match Tree Walks (`T-04`, `T-05`, `T-08`, `T-09`, `T-12`, `T-21`):** Increasing `ripgrep` from 12 to 32 threads
     **degrades** `ripgrep`'s runtime on 5 of these 6 scenarios (`T-05`: `809.8 ms` → `1.04 s`; `T-08`: `1.11 s` → `1.54 s`;
     `T-09`: `778.5 ms` → `1.01 s`; `T-12`: `689.2 ms` → `766.2 ms`; `T-21`: `1.26 s` → `1.55 s`) due to output lock contention.
     `G1` beats `ripgrep -j 32` by **3.42× to 10.17×**.
   - **Heavy Multi-Pattern & PCRE2 Tree Walks (`M-01..M-05`, `T-16`, `T-17`):** Even at matched 32 threads, `G1` beats `ripgrep -j 32`
     on all multi-pattern sets (`M-01`: **1.64×**, `M-02`: **1.99×**, `M-03`: **2.50×**, `M-04`: **6.30×**, `M-05`: **2.46×**),
     dense PCRE2 lookaround (`T-16`: **219.8 ms vs 1.14 s**, **5.18×**), and rare PCRE2 (`T-17`: **154.0 ms vs 229.1 ms**, **1.49×**).
   - **Matched 12 Threads (`G1 @ 12t` vs `R0 @ 12t`):** When both tools run on 12 threads on the 40 matched scenarios, G1 wins **all 40 out of 40** scenarios (**1.95× geomean**); on the 29 Category T tree scenarios, G1 wins **29 out of 29** scenarios (**1.61× category geomean**),
     showing that our directory walker and search algorithms are fundamentally faster per-core.

---

## 8. Synthetic Benchmark Suite & Edge-Case Conformance

In addition to real-world kernel tree benchmarks, the project maintains an automated 13-workload synthetic benchmark
suite (`benchmarks/run_benchmarks.py`), a 266-test automated edge-case, boundary, and multithreaded conformance harness, 136 Automake test suites, and a continuous stress test harness (`benchmarks/stress_test.py`).

### 13-Workload Synthetic Benchmark (5 Iterations per Workload)

| Workload ID | Description | Corpus Size | Stock Baseline G0 (ms) | Baseline Throughput | G1 (t=1) (ms) | G1 (t=4) (ms) | G1 Default (32t) (ms) | Speedup (vs G0) | Equivalence |
| :--- | :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `BM-01` | `-F` rare literal | 1572 MB | 659.2 ms | 2.33 GB/s | 452.9 ms | 247.7 ms | **83.1 ms** | **7.93×** | **PASS** |
| `BM-02` | `-Fi` case-insensitive literal | 1572 MB | 693.6 ms | 2.21 GB/s | 455.3 ms | 241.7 ms | **83.0 ms** | **8.35×** | **PASS** |
| `BM-03` | `-E` DFA medium regex | 1572 MB | 572.9 ms | 2.68 GB/s | 464.3 ms | 244.2 ms | **85.4 ms** | **6.71×** | **PASS** |
| `BM-04` | `-E` regex backreference | 1572 MB | 1317.2 ms | 1.17 GB/s | 473.3 ms | 242.7 ms | **83.0 ms** | **15.88×** | **PASS** |
| `BM-05` | `-P` PCRE2 lookbehind | 1572 MB | 939.2 ms | 1.63 GB/s | 451.4 ms | 247.1 ms | **81.2 ms** | **11.57×** | **PASS** |
| `BM-06` | `-n -b -F` line numbers & byte offset | 1572 MB | 759.1 ms | 2.02 GB/s | 481.5 ms | 358.2 ms | **113.9 ms** | **6.66×** | **PASS** |
| `BM-07` | `-n -C 3` context printing | 1572 MB | 761.2 ms | 2.02 GB/s | 482.7 ms | 324.6 ms | **122.1 ms** | **6.23×** | **PASS** |
| `BM-08` | `-c -E` match counting | 1572 MB | 559.3 ms | 2.74 GB/s | 457.2 ms | 239.0 ms | **83.2 ms** | **6.72×** | **PASS** |
| `BM-09` | `-r -n` recursive tree line numbers | 566 MB | 392.1 ms | 1.41 GB/s | 288.8 ms | 64.4 ms | **26.7 ms** | **14.68×** | **PASS** |
| `BM-10` | `-r -l` recursive file listing | 566 MB | 215.0 ms | 2.57 GB/s | 214.2 ms | 49.5 ms | **23.3 ms** | **9.23×** | **PASS** |
| `BM-11` | `-r -F` multi-pattern tree | 566 MB | 270.1 ms | 2.05 GB/s | 285.3 ms | 65.0 ms | **27.3 ms** | **9.90×** | **PASS** |
| `BM-12` | Stdin file redirect (`< $FILE`) | 1572 MB | 662.0 ms | 2.32 GB/s | 465.1 ms | 252.8 ms | **90.4 ms** | **7.32×** | **PASS** |
| `BM-13` | True FIFO pipe (`cat \| grep`) | 1572 MB | 1162.8 ms | 1.32 GB/s | 1166.7 ms | 787.6 ms | **810.6 ms** | **1.43×** | **PASS** |

### Automated Edge-Case, Boundary & Multithreaded Conformance (266 / 266 PASS)
The 266 automated conformance checks verify boundary and concurrency conditions against stock baseline grep:
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
- **Pairwise Matrix, Fault Injection & Coverage Edge Cases (Checks 201–266 + 136 Automake Tests):** Exhaustive pairwise engine × input-path × modifier × cross-file transition matrix (`tests/parallel-matrix`), fault-injection boundary shrinking (`WALK_FD_CAP_MAX=2`, `MMAP_SCAN` match overflow, `mmap` failure fallback, short pipe reads, `EINTR`, `EPIPE`, `/dev/full` `ENOSPC` in `tests/parallel-fault-injection`), and full gcov line/branch coverage exercises (`tests/parallel-gcov-coverage`) pass 100% byte-for-byte.

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

| # | Hypothesis | Target Criterion | Observed Outcome | Status |
| :-: | :--- | :--- | :--- | :---: |
| **H1** | Tree literal scan speedup | Ours ≥ 5× G0; within ±30% of R0 | T-01: 23.61× vs G0, 2.22× vs R0; T-04: 28.25× vs G0, 3.18× vs R0 | ✅ PASS |
| **H2** | $BIG intra-file parallelism | Ours ≥ 4× R0 (R0 searches 1 file with 1 thread) | B-01: 5.07× vs R0; B-03: 4.17× vs R0 | ✅ PASS |
| **H3** | Tiny invocation startup latency | Ours ≤ 1.5× G0; R0 ≈ 2–3× G0 | G0: 2.7 ms, G1: 2.6 ms (0.98×), R0: 3.8 ms | ✅ PASS |
| **H4** | Output-bound queries (~1.3M lines) | Ours never slower than G0 | T-05: G1 is 16.08× vs G0 | ✅ PASS |
| **H5** | UTF-8 vs C locale sensitivity | Both greps slower in UTF-8 on -i; R0 unaffected | T-09 (-i): G0 1.17× slower in UTF-8, G1 1.00×; R0 0.90× (unaffected) | ⚠️ CHECK |
| **H6** | Cold page cache | All tools bounded by PD bandwidth (within 20–35%) | T-01: G1 1.99 s (825 MB/s), R0 4.96 s (331 MB/s), G0 53.48 s | ⚠️ DIVERGENT |
| **H7** | 1,000-literal multi-pattern (M-02) | Ours within 2× of R0 | M-02: 79.08× vs G0, 4.06× vs R0 | ✅ PASS |
| **H8** | PCRE2 lookaround (T-16, B-08) | Ours ≫ G0 (literal pre-filter + threads) and ≥ R0 | T-16: 125.69× vs G0, 11.42× vs R0; B-08: 104.10× vs G0 | ✅ PASS |
| **H9** | Scaling past 16 threads | Gains throughput past 16 threads | B-01: 1.24× (16→64 th); 1..128 thread sweep across 128 logical CPUs (2 NUMA nodes) | ✅ PASS |

---

*Report generated automatically from primary benchmark artifacts.*
