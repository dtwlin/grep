<!-- Copyright (C) 2026 David Lin <dtwlin@gmail.com> -->
# Multi-Threaded GNU Grep: Benchmark & Performance Evaluation

> Comprehensive performance, scalability, and correctness evaluation comparing
> our parallel multi-threaded GNU `grep` fork (`G1`, commit `76d3503`, default 32 worker threads)
> against upstream GNU `grep` baseline (`G0`, version 3.12.41, 1 thread), stock `ripgrep` (`R0`,
> version 15.1.0 with PCRE2, evaluated both at its default 12 worker threads and at thread-matched `-j 32`),
> and stock `ugrep` (`U0`, version 7.8.5 with PCRE2, evaluated both at its default 12 worker threads and at thread-matched `-J 32`).

---

## 1. Executive Summary & Headline Takeaways

This document presents the complete benchmark results for the parallel GNU `grep` drop-in replacement.
All measurements were conducted on a 128-logical-CPU dual-socket AMD EPYC 7B13 server across the full
Linux kernel codebase (v7.3-rc3, 96,034 tracked files, 1.64 GB working tree) and a 1.45 GB unified source
concatenation (`linux-all.c`), alongside a 266-test automated edge-case, boundary, and multithreaded conformance suite, 151 Automake tests, continuous stress testing, and 13 synthetic workloads.

To ensure complete fairness when comparing against `ripgrep` (`R0`) — which heuristically caps its default directory-walk
thread pool at `min(cores, 12)` (12 worker threads) while `G1` defaults to `min(cores, 32)` (32 worker threads) —
every scenario was evaluated against **both** out-of-the-box `ripgrep` (12 threads) and thread-matched `ripgrep -j 32` (32 threads),
as well as a dedicated thread-matched suite running `G1` at 12 threads against `ripgrep` at 12 threads.
`ugrep` (`U0`) caps its default at `min(cores − 1, 12)` (12 worker threads on this server) and was evaluated the same way:
at its default, at thread-matched `ugrep -J 32`, and against `G1` at 12 threads.

### High-Level Summary Statistics (Warm Cache, en_US.UTF-8)

| Metric | G1 (32t) vs Stock G0 (1t) | R0 Default (12t) vs G0 (1t) | R0 Matched (-j 32) vs G0 (1t) | G1 (32t) vs R0 Default (12t) | G1 (32t) vs R0 Matched (-j 32) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Geometric Mean Speedup (All 59 Scenarios)** | **22.07×** | **5.70×** | **7.96×** | **3.87×** | **2.77×** |
| **Category B — Single Huge File (14 Scenarios)** | **19.51×** | 1.99× | 1.96× | **9.81×** | **9.94×** |
| **Category T — Tree Traversal (29 Scenarios)** | **22.54×** | 8.45× | 13.29× | **2.67×** | **1.70×** |
| **Category M — Multi-Pattern Sets (6 Scenarios)** | **125.89×** | 15.73× | 28.16× | **8.01×** | **4.47×** |
| **Category L — Invocation Latency (4 Scenarios)** | **1.04×** (zero pool cost) | 0.84× | 0.77× | **1.24×** | **1.36×** |
| **Category U — UX & Defaults (2 Scenarios)** | **27.37×** | 13.26× | 24.87× | **2.06×** | **1.10×** |
| **Category X — xargs Parallelism (1 Scenario)** | **8.81×** | 3.88× | 4.40× | **2.27×** | **2.00×** |
| **Category P — Complex Regexes (3 Scenarios)** | **68.33×** | 18.94× | 39.84× | **3.61×** | **1.72×** |
| **Win / Tie / Loss Count vs Ripgrep** | — | — | — | **58 Wins / 1 Tie / 0 Losses** | **52 Wins / 3 Ties / 4 Losses** |
| **Gate A Hard Correctness (Byte-Exact vs G0)** | **59 / 59 PASS (100%)** | — | — | — | — |
| **Gate D Determinism (1..128 Thread Invariance)** | **9 / 9 PASS (100%)** | — | — | — | — |

> **Fair 12-Thread Matched Comparison (G1 @ 12t vs R0 @ 12t on 40 Scenarios):**
> Across the 40 matched scenarios (37 tree scenarios plus single-file B-01/B-04/M-06), G1 running on the same 12 threads
> achieves a **2.01× geometric mean speedup** over `ripgrep` (**40 Wins / 0 Ties / 0 Losses**)!
> In Category T alone, G1 @ 12t beats R0 @ 12t with a **1.62× geomean** (**29 Wins / 0 Ties / 0 Losses**).

### Summary Statistics vs ugrep (Warm Cache, en_US.UTF-8)

| Metric | G1 (32t) vs Stock G0 (1t) | U0 Default (12t) vs G0 (1t) | U0 Matched (-J 32) vs G0 (1t) | G1 (32t) vs U0 Default (12t) | G1 (32t) vs U0 Matched (-J 32) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Geometric Mean Speedup (All 59 Scenarios)** | **22.07×** | **5.10×** | **3.68×** | **4.33×** | **6.00×** |
| **Category B — Single Huge File (14 Scenarios)** | **19.51×** | 1.83× | 1.81× | **10.65×** | **10.79×** |
| **Category T — Tree Traversal (29 Scenarios)** | **22.54×** | 6.75× | 4.04× | **3.34×** | **5.58×** |
| **Category M — Multi-Pattern Sets (6 Scenarios)** | **125.89×** | 25.06× | 18.71× | **5.02×** | **6.73×** |
| **Category L — Invocation Latency (4 Scenarios)** | **1.04×** (zero pool cost) | 0.71× | 0.66× | **1.47×** | **1.58×** |
| **Category U — UX & Defaults (2 Scenarios)** | **27.37×** | 11.15× | 5.29× | **2.46×** | **5.18×** |
| **Category X — xargs Parallelism (1 Scenario)** | **8.81×** | 2.76× | 2.19× | **3.19×** | **4.03×** |
| **Category P — Complex Regexes (3 Scenarios)** | **68.33×** | 17.19× | 14.85× | **3.97×** | **4.60×** |
| **Win / Tie / Loss Count vs ugrep** | — | — | — | **58 Wins / 1 Tie / 0 Losses** | **58 Wins / 1 Tie / 0 Losses** |

> **Fair 12-Thread Matched Comparison (G1 @ 12t vs U0 @ 12t on 40 Scenarios):**
> Across the 40 matched scenarios (37 tree scenarios plus single-file B-01/B-04/M-06), G1 running on the same 12 threads
> achieves a **2.32× geometric mean speedup** over `ugrep` (**40 Wins / 0 Ties / 0 Losses**)!
> In Category T alone, G1 @ 12t beats U0 @ 12t with a **2.02× geomean** (**29 Wins / 0 Ties / 0 Losses**).

> Win / tie / loss counts treat speedups within ±2% as ties.

### Key Takeaways

1. **Decisive Multi-Core Speedup Over Stock GNU Grep:**
   - On whole-tree searches across the 96,034-file Linux kernel tree, G1 accelerates execution from seconds to sub-second
     runtimes (**150–287 ms**, a **22.54×** category geometric mean speedup).
   - On massive multi-pattern dictionary searches (100 to 10,000 keywords), G1 delivers up to **513.02×** speedup over G0 (`M-04`, 186.2 ms vs 95.54 s)
     and **216.59×** on large single files (`M-06`, 41.2 ms vs 8.92 s) via parallelized dense Aho-Corasick DFA and SIMD pre-filtering.
   - On complex regexes (`Category P`), G1 achieves **159.33×** speedup on backreferences (`P-01`, 260.4 ms vs 41.48 s) and **81.17×** on 1,000-way alternations (`P-02`, 185.8 ms vs 15.08 s).
   - On a 1.45 GB single file, intra-file chunking yields up to **130.13×** speedup over G0 on PCRE2 lookaround (`B-08`, 164.4 ms vs 21.40 s) and **43.93×** on case-insensitive search (`B-05`, 44.8 ms vs 1.97 s).

2. **Unprecedented Performance vs Ripgrep (Out-of-the-Box 12t AND Thread-Matched `-j 32`):**
   - **Overall Geometric Mean:** Across all 59 scenarios, G1 is **3.87× faster** than default `ripgrep` (58 Wins / 1 Tie / 0 Losses)
     and **2.77× faster** than thread-matched `ripgrep -j 32` (52 Wins / 3 Ties / 4 Losses).
   - **Single Large Files (14/14 Wins at both 12t and 32t, 9.81×–9.94× geomean):** Ripgrep processes individual files using a single thread
     regardless of `-j`; our intra-file parallel chunking outperforms `ripgrep -j 32` by **1.43× to 131.52×** (and **75.36×** on 1,000-keyword matching `M-06`).
   - **Whole-Tree Traversal (2.67× geomean vs R0 12t; 1.70× geomean vs R0 `-j 32`):**
     - Against out-of-the-box `ripgrep` (12t), G1 wins **all 29 of 29** tree scenarios (including `T-25b` immediate first-file exit at **2.7 ms vs 6.0 ms**, **2.19×**).
     - When `ripgrep` is explicitly given `-j 32` (`32` threads), G1 beats or ties `ripgrep -j 32` in **26 of 29** tree scenarios and loses only 3 (`T-02` rare literal, 198.1 ms vs 159.6 ms; `T-18` only-matching, 205.6 ms vs 194.1 ms; `T-28` sub-scope `kernel mm fs`, 17.1 ms vs 15.1 ms):
       G1 dominates high-output, count-based, PCRE2, and sparse tree scans (`T-04` dense line numbers: **158.1 ms vs 512.6 ms**, **3.24×**;
       `T-05` very dense literal: **287.3 ms vs 979.0 ms**, **3.41×**; `T-08` count per file: **151.2 ms vs 1.44 s**, **9.49×**;
       `T-09` case-insensitive: **161.1 ms vs 928.0 ms**, **5.76×**; `T-12` anchored dense regex: **166.0 ms vs 682.1 ms**, **4.11×**;
       `T-16` dense PCRE2: **228.5 ms vs 1.03 s**, **4.51×**; `T-17` rare PCRE2: **155.7 ms vs 225.4 ms**, **1.45×**;
       `T-21` inverted count: **178.3 ms vs 1.44 s**, **8.09×**; `T-23` glob sub-scope: **15.9 ms vs 24.9 ms**, **1.56×**).
     - On sparse tree traversals (`T-01..T-03`), work-stealing parallel directory walking and openat-relative file descriptors keep runtime to **150.9–198.1 ms** against `ripgrep -j 32` at 157.5–175.4 ms (G1 time ratio 0.81×–1.14×; `T-02` is slower: two of its three runs took about 50 ms longer than the third).
   - **Multi-Pattern Dictionary Searches (6/6 Wins at both 12t and `-j 32`, 4.47× geomean vs `-j 32`):** G1 beats `ripgrep -j 32` on
     `M-01` (100 literals: **160.5 ms vs 280.3 ms**, **1.75×**), `M-02` (1,000 literals: **173.3 ms vs 333.4 ms**, **1.92×**),
     `M-03` (10,000 literals: **239.4 ms vs 603.1 ms**, **2.52×**), `M-04` (1,000 literals `-i`: **186.2 ms vs 999.7 ms**, **5.37×**),
     `M-05` (1,000 literals `-w`: **173.5 ms vs 404.5 ms**, **2.33×**), and `M-06` (`$BIG`: **41.2 ms vs 3.11 s**, **75.36×**).

3. **Performance vs ugrep (Out-of-the-Box 12t AND Thread-Matched `-J 32`):**
   - **Overall Geometric Mean:** Across all 59 scenarios, G1 is **4.33× faster** than default `ugrep` (58 Wins / 1 Tie / 0 Losses)
     and **6.00× faster** than thread-matched `ugrep -J 32` (58 Wins / 1 Tie / 0 Losses).
   - **Single Large Files (10.65× geomean vs U0 12t; 10.79× vs U0 `-J 32`):** 14 Wins / 0 Ties / 0 Losses at the default, 14 Wins / 0 Ties / 0 Losses at `-J 32`.
   - **Whole-Tree Traversal (3.34× geomean vs U0 12t; 5.58× vs U0 `-J 32`):** 29 Wins / 0 Ties / 0 Losses at the default, 29 Wins / 0 Ties / 0 Losses at `-J 32`.
   - **Multi-Pattern Dictionary Searches (5.02× geomean vs U0 12t; 6.73× vs U0 `-J 32`):** 6 Wins / 0 Ties / 0 Losses at the default, 6 Wins / 0 Ties / 0 Losses at `-J 32`.
   - **Complex Regexes (3.97× geomean vs U0 12t; 4.60× vs U0 `-J 32`):** 3 Wins / 0 Ties / 0 Losses at the default, 3 Wins / 0 Ties / 0 Losses at `-J 32`.

4. **Zero Startup Overhead:**
   - Single-file micro-benchmark latency (`L-01`, `init/main.c`) measures **2.6 ms raw** (net **1.1 ms** excluding the 1.5 ms `/bin/true` harness floor), and immediate quiet match on the full tree (`T-25b`) finishes in **2.7 ms**.
   - Stock G0 measures 2.6 ms raw (net 1.1 ms), while ripgrep measures 3.6 ms raw (net 2.2 ms) on `L-01` and 6.0 ms on `T-25b`, and ugrep 4.7 ms raw (net 3.3 ms) and 12.5 ms. Worker threads start lazily after synchronous root-file probing,
     ensuring zero latency penalty for command-line interactive scripts and small single-file queries.

5. **Massive Cold Cache Throughput:**
   - Under full page-cache eviction (`posix_fadvise`), G1 achieves **833–846 MB/s** tree scan throughput (**1.96 s** on `T-01`, **1.94 s** on `T-06`), outperforming ripgrep
     at 338–366 MB/s (4.85 s on `T-01`, **2.48× faster**) and stock GNU grep at 33–34 MB/s (49.63 s on `T-01`, **25.3× faster**); ugrep reaches 346–349 MB/s (4.75 s on `T-01`, **2.42×** slower than G1). Concurrent directory worker prefetching
     saturates underlying storage I/O queues far more effectively than single-threaded traversal.

6. **100% Determinism and Zero Semantic Regression:**
   - **Gate A (100% Pass):** In all 59 benchmark scenarios, G1's output matches stock G0 byte-for-byte on `stdout`, `stderr`, and exit codes
     (whereas `ripgrep` prints files in non-deterministic thread-completion order unless `--sort=path` is passed).
   - **Gate D (100% Pass):** Output across 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, and 128 threads produces identical SHA-256 hashes.
   - **Synthetic Conformance (266/266 Pass) & Automake Suite (141 Pass, 8 Skip, 2 XFail, 0 Fail):** All edge-case regressions, context overlap rules, max-count limits, and memory limits pass.

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
  - File Size: 1,453,294,424 bytes (~1.45 GB)
- **Real Moderate Header (`big-24M.h`)**:
  - Size: 24,009,174 bytes (24.0 MB, AMD ASIC register definitions)
- **Path Manifest (`files.z`)**:
  - Size: 3,845,920 bytes (3.8 MB NUL-delimited list of all 96,034 kernel relative paths)

### Binary Specifications & SHA-256 Hashes

| Tool Identifier | Binary Name & Role | Version | Compiler Flags / Build Details | Default Threads | Binary SHA-256 Checksum |
| :--- | :--- | :---: | :--- | :---: | :--- |
| **`G0`** | Upstream Stock Baseline GNU grep | `3.12.41` | `./configure CFLAGS='-g -O3 -march=native'` | 1 | `ffbb6be702d1af4269dd63e208fef019b408fdb14b139e705aac67e82c5d77cf` |
| **`G1`** | Parallel Multi-Threaded GNU grep | `3.12.131` (`76d3503`) | `-pthread -O3 -march=native -flto=auto -fno-plt -fomit-frame-pointer -falign-functions=32 -falign-loops=32`, AVX2 kernels | 33 (32 workers + main) | `2bf59acfd26de0edaa50474d260a1b4a2874a0919af98dc79526070d778667fb` |
| **`R0`** | Stock Ripgrep | `15.1.0` | Official release binary (`+pcre2`, SIMD/AVX2 runtime) | 13 (12 workers + main; also tested with `-j 32`) | `5c3dddf97a3a0740db7e91a91706299a01fceb9371b66a2ed076c998a9e8e04d` |
| **`U0`** | Stock ugrep | `7.8.5` | Upstream release tarball built with its `./build.sh` defaults (`-O2`, runtime AVX2 dispatch, PCRE2 JIT) | 13 (12 workers + main; also tested with `-J 32`) | `7c952446726bceeaf0996931109d876b1bf3420aae0bec16902307554effd4c5` |

---

## 3. Complete Linux Kernel-Tree Benchmark Master Matrix (Default & Thread-Matched)

All 59 warm-cache scenarios evaluated under `LC_ALL=en_US.UTF-8`. Each scenario was executed for 1 unmeasured warm-up
round followed by 3 measured rounds (Category L latency scenarios executed for 30–100 measured rounds).
Both `R0 (12t)` (ripgrep's out-of-the-box 12-thread cap) and `R0 (-j 32)` (thread-matched to `G1`'s 32 worker threads) are reported side by side.

| Scenario | Description | G0 (1t) | G1 (32t) | R0 (12t) | R0 (-j 32) | G1 vs G0 | G1 vs R0 (12t) | G1 vs R0 (32t) | G1 CPU | R0 (12t) CPU | R0 (32t) CPU | G1 RSS | R0 RSS | Gate A | Gate B |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| `B-01` | rare literal ($BIG) | 536.9 ms | 19.7 ms | 171.5 ms | 174.9 ms | **27.32×** | **8.72×** | **8.90×** | 0.41 | 0.17 | 0.17 | 1378M | 1391M | **PASS** | PASS |
| `B-02` | count dense ($BIG) | 604.9 ms | 19.6 ms | 192.2 ms | 195.9 ms | **30.79×** | **9.78×** | **9.97×** | 0.39 | 0.19 | 0.20 | 1378M | 1391M | **PASS** | PASS |
| `B-03` | moderate, line numbers ($BIG) | 934.2 ms | 40.6 ms | 246.2 ms | 247.9 ms | **22.99×** | **6.06×** | **6.10×** | 0.87 | 0.25 | 0.25 | 1375M | 1392M | **PASS** | PASS |
| `B-04` | DFA regex ($BIG) | 434.7 ms | 20.0 ms | 173.8 ms | 174.0 ms | **21.75×** | **8.69×** | **8.70×** | 0.39 | 0.17 | 0.17 | 1375M | 1392M | **PASS** | PASS |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 44.8 ms | 300.3 ms | 301.9 ms | **43.93×** | **6.71×** | **6.74×** | 0.98 | 0.30 | 0.30 | 1375M | 1392M | **PASS** | PASS |
| `B-06` | word match ($BIG) | 370.8 ms | 23.0 ms | 180.6 ms | 183.4 ms | **16.12×** | **7.85×** | **7.97×** | 0.46 | 0.18 | 0.18 | 1377M | 1391M | **PASS** | PASS |
| `B-07` | literal-free regex ($BIG) | 663.0 ms | 30.1 ms | 1.58 s | 1.57 s | **22.05×** | **52.40×** | **52.34×** | 0.66 | 1.58 | 1.57 | 1377M | 1391M | **PASS** | PASS |
| `B-08` | PCRE2 lookaround ($BIG) | 21.40 s | 164.4 ms | 21.61 s | 21.63 s | **130.13×** | **131.41×** | **131.52×** | 2.38 | 21.63 | 21.61 | 1440M | 1394M | **PASS** | PASS |
| `B-09` | context ($BIG) | 916.9 ms | 36.6 ms | 272.8 ms | 277.1 ms | **25.06×** | **7.46×** | **7.57×** | 0.82 | 0.27 | 0.28 | 1374M | 1391M | **PASS** | PASS |
| `B-10` | only-matching ($BIG) | 767.3 ms | 41.9 ms | 188.0 ms | 194.3 ms | **18.32×** | **4.49×** | **4.64×** | 0.66 | 0.19 | 0.19 | 1377M | 1392M | **PASS** | PASS |
| `B-11` | inverted count ($BIG) | 978.8 ms | 46.0 ms | 1.46 s | 1.46 s | **21.28×** | **31.80×** | **31.76×** | 1.00 | 1.46 | 1.46 | 1380M | 1391M | **PASS** | PASS |
| `B-12` | stdin redirect (< $BIG) | 530.7 ms | 21.3 ms | 256.7 ms | 258.9 ms | **24.89×** | **12.04×** | **12.14×** | 0.44 | 0.26 | 0.26 | 1377M | 38M | **PASS** | PASS |
| `B-13` | FIFO pipe (cat $BIG \\|) | 1.14 s | 651.3 ms | 853.2 ms | 931.6 ms | **1.74×** | **1.31×** | **1.43×** | 0.88 | 0.62 | 0.68 | 38M | 38M | **PASS** | PASS |
| `B-14` | real 24 MB header (big-24M.h) | 51.5 ms | 16.1 ms | 44.8 ms | 44.0 ms | **3.20×** | **2.78×** | **2.73×** | 0.06 | 0.04 | 0.04 | 38M | 38M | **PASS** | PASS |
| `L-00` | harness floor (/bin/true) | 1.5 ms | 1.5 ms | 1.5 ms | 1.5 ms | **1.01×** | **1.00×** | **1.00×** | 0.00 | 0.00 | 0.00 | 38M | 38M | **PASS** | PASS |
| `L-01` | one small file (init/main.c) | 2.6 ms | 2.6 ms | 3.6 ms | 3.6 ms | **1.00×** | **1.41×** | **1.41×** | 0.00 | 0.00 | 0.00 | 38M | 38M | **PASS** | PASS |
| `L-02` | tiny tree (kernel/sched) | 7.0 ms | 6.0 ms | 7.1 ms | 10.3 ms | **1.17×** | **1.18×** | **1.71×** | 0.01 | 0.01 | 0.02 | 38M | 38M | **PASS** | PASS |
| `L-03` | quiet first match in big file | 2.6 ms | 2.6 ms | 3.7 ms | 3.6 ms | **0.99×** | **1.40×** | **1.39×** | 0.00 | 0.00 | 0.00 | 38M | 38M | **PASS** | PASS |
| `M-01` | 100 literals, tree | 12.19 s | 160.5 ms | 646.9 ms | 280.3 ms | **75.94×** | **4.03×** | **1.75×** | 4.75 | 7.67 | 8.05 | 38M | 38M | **PASS** | PASS |
| `M-02` | 1,000 literals, tree | 14.04 s | 173.3 ms | 719.0 ms | 333.4 ms | **80.98×** | **4.15×** | **1.92×** | 5.12 | 8.34 | 9.42 | 38M | 65M | **PASS** | PASS |
| `M-03` | 10,000 literals, tree | 17.12 s | 239.4 ms | 1.16 s | 603.1 ms | **71.51×** | **4.83×** | **2.52×** | 5.96 | 13.45 | 15.43 | 56M | 38M | **PASS** | PASS |
| `M-04` | 1,000 literals, -i | 95.54 s | 186.2 ms | 1.55 s | 999.7 ms | **513.02×** | **8.33×** | **5.37×** | 5.54 | 17.92 | 30.18 | 38M | 146M | **PASS** | PASS |
| `M-05` | 1,000 literals, -w | 14.13 s | 173.5 ms | 898.9 ms | 404.5 ms | **81.46×** | **5.18×** | **2.33×** | 5.11 | 10.46 | 11.57 | 38M | 74M | **PASS** | PASS |
| `M-06` | 1,000 literals, $BIG | 8.92 s | 41.2 ms | 3.11 s | 3.11 s | **216.59×** | **75.50×** | **75.36×** | 0.71 | 3.13 | 3.11 | 1384M | 1404M | **PASS** | PASS |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.48 s | 260.4 ms | 835.8 ms | 338.9 ms | **159.33×** | **3.21×** | **1.30×** | 8.12 | 9.93 | 10.38 | 51M | 51M | **PASS** | PASS |
| `P-02` | 1,000-way regex alternation | 15.08 s | 185.8 ms | 1.19 s | 692.7 ms | **81.17×** | **6.42×** | **3.73×** | 5.11 | 13.87 | 20.52 | 51M | 134M | **PASS** | PASS |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.80 s | 154.2 ms | 350.9 ms | 160.4 ms | **24.67×** | **2.28×** | **1.04×** | 4.56 | 4.11 | 4.75 | 51M | 51M | **PASS** | PASS |
| `T-01` | zero-match literal | 3.75 s | 150.9 ms | 344.3 ms | 157.5 ms | **24.83×** | **2.28×** | **1.04×** | 4.48 | 4.03 | 4.75 | 38M | 38M | **PASS** | PASS |
| `T-02` | rare literal | 3.98 s | 198.1 ms | 352.3 ms | 159.6 ms | **20.11×** | **1.78×** | **0.81×** | 5.52 | 4.18 | 4.80 | 38M | 38M | **PASS** | PASS |
| `T-03` | moderate literal, line numbers | 4.44 s | 153.7 ms | 365.0 ms | 175.4 ms | **28.89×** | **2.37×** | **1.14×** | 4.56 | 4.31 | 5.24 | 38M | 38M | **PASS** | PASS |
| `T-04` | dense literal, line numbers | 4.67 s | 158.1 ms | 505.1 ms | 512.6 ms | **29.51×** | **3.19×** | **3.24×** | 5.16 | 5.70 | 6.98 | 38M | 38M | **PASS** | PASS |
| `T-05` | very dense literal | 4.84 s | 287.3 ms | 763.0 ms | 979.0 ms | **16.84×** | **2.66×** | **3.41×** | 5.07 | 7.14 | 8.04 | 84M | 38M | **PASS** | PASS |
| `T-06` | files-with-matches | 3.87 s | 152.2 ms | 358.1 ms | 220.3 ms | **25.40×** | **2.35×** | **1.45×** | 4.45 | 4.19 | 6.29 | 109M | 38M | **PASS** | PASS |
| `T-07` | files-without-match | 3.46 s | 150.3 ms | 420.0 ms | 289.1 ms | **23.00×** | **2.79×** | **1.92×** | 4.43 | 4.91 | 8.56 | 108M | 38M | **PASS** | SOFT_DIFF |
| `T-08` | count per file | 3.78 s | 151.2 ms | 1.07 s | 1.44 s | **25.02×** | **7.10×** | **9.49×** | 4.53 | 9.87 | 10.76 | 38M | 38M | **PASS** | PASS |
| `T-09` | case-insensitive literal | 5.30 s | 161.1 ms | 723.7 ms | 928.0 ms | **32.87×** | **4.49×** | **5.76×** | 4.77 | 7.66 | 8.97 | 38M | 38M | **PASS** | KNOWN-SEMANTIC |
| `T-10` | word match | 4.19 s | 157.8 ms | 373.2 ms | 192.4 ms | **26.55×** | **2.36×** | **1.22×** | 4.64 | 4.38 | 5.77 | 38M | 38M | **PASS** | PASS |
| `T-11` | DFA regex with must-literal | 3.79 s | 155.1 ms | 351.0 ms | 162.7 ms | **24.44×** | **2.26×** | **1.05×** | 4.63 | 4.13 | 4.88 | 38M | 38M | **PASS** | PASS |
| `T-12` | anchored dense regex | 4.69 s | 166.0 ms | 592.0 ms | 682.1 ms | **28.26×** | **3.57×** | **4.11×** | 4.83 | 6.41 | 7.32 | 38M | 38M | **PASS** | PASS |
| `T-13` | literal alternation + word boundary | 3.98 s | 155.6 ms | 369.7 ms | 175.8 ms | **25.56×** | **2.38×** | **1.13×** | 4.55 | 4.36 | 5.30 | 38M | 38M | **PASS** | PASS |
| `T-14` | optional group + alternation | 3.85 s | 154.1 ms | 349.5 ms | 162.6 ms | **25.02×** | **2.27×** | **1.06×** | 4.52 | 4.08 | 4.85 | 38M | 38M | **PASS** | PASS |
| `T-15` | literal-free regex | 4.14 s | 163.5 ms | 378.5 ms | 166.7 ms | **25.34×** | **2.31×** | **1.02×** | 4.90 | 4.45 | 4.99 | 38M | 38M | **PASS** | PASS |
| `T-16` | PCRE2 lookaround, dense | 27.63 s | 228.5 ms | 2.49 s | 1.03 s | **120.91×** | **10.90×** | **4.51×** | 7.00 | 29.73 | 31.06 | 69M | 38M | **PASS** | PASS |
| `T-17` | PCRE2 rare with literal | 5.36 s | 155.7 ms | 524.9 ms | 225.4 ms | **34.43×** | **3.37×** | **1.45×** | 4.52 | 6.22 | 6.78 | 71M | 38M | **PASS** | PASS |
| `T-18` | only-matching | 4.17 s | 205.6 ms | 382.2 ms | 194.1 ms | **20.29×** | **1.86×** | **0.94×** | 5.72 | 4.50 | 5.79 | 38M | 38M | **PASS** | PASS |
| `T-19` | context, rare | 4.42 s | 156.2 ms | 364.8 ms | 161.8 ms | **28.31×** | **2.34×** | **1.04×** | 4.58 | 4.29 | 4.91 | 38M | 38M | **PASS** | PASS |
| `T-20` | context, moderate | 4.44 s | 151.8 ms | 382.0 ms | 193.0 ms | **29.27×** | **2.52×** | **1.27×** | 4.52 | 4.48 | 5.32 | 38M | 38M | **PASS** | PASS |
| `T-21` | inverted count | 4.51 s | 178.3 ms | 1.09 s | 1.44 s | **25.31×** | **6.12×** | **8.09×** | 5.41 | 10.89 | 12.16 | 38M | 38M | **PASS** | SOFT_DIFF |
| `T-22` | glob include | 2.11 s | 81.1 ms | 174.1 ms | 94.7 ms | **26.08×** | **2.15×** | **1.17×** | 2.00 | 1.99 | 2.68 | 38M | 38M | **PASS** | PASS |
| `T-23` | glob + sub-scope | 123.4 ms | 15.9 ms | 24.9 ms | 24.9 ms | **7.74×** | **1.56×** | **1.56×** | 0.16 | 0.20 | 0.26 | 38M | 38M | **PASS** | PASS |
| `T-24` | max-count per file | 3.83 s | 153.6 ms | 360.6 ms | 175.8 ms | **24.97×** | **2.35×** | **1.14×** | 4.55 | 4.21 | 5.26 | 38M | 38M | **PASS** | PASS |
| `T-25a` | quiet, no match | 3.71 s | 155.4 ms | 344.7 ms | 158.0 ms | **23.89×** | **2.22×** | **1.02×** | 4.53 | 4.04 | 4.73 | 132M | 38M | **PASS** | PASS |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | 6.0 ms | 7.1 ms | **1.00×** | **2.19×** | **2.60×** | 0.00 | 0.01 | 0.01 | 38M | 38M | **PASS** | PASS |
| `T-26` | byte offsets | 4.44 s | 152.0 ms | 363.0 ms | 176.3 ms | **29.18×** | **2.39×** | **1.16×** | 4.52 | 4.27 | 5.30 | 38M | 38M | **PASS** | PASS |
| `T-27` | sub-scope drivers/ | 2.01 s | 73.6 ms | 165.1 ms | 86.9 ms | **27.35×** | **2.24×** | **1.18×** | 1.95 | 1.90 | 2.44 | 51M | 51M | **PASS** | PASS |
| `T-28` | sub-scope kernel mm fs | 168.2 ms | 17.1 ms | 19.1 ms | 15.1 ms | **9.83×** | **1.12×** | **0.88×** | 0.17 | 0.16 | 0.18 | 51M | 51M | **PASS** | PASS |
| `U-01` | out-of-the-box UX | 4.41 s | 151.3 ms | 390.7 ms | 207.0 ms | **29.11×** | **2.58×** | **1.37×** | 4.54 | 4.58 | 6.27 | 51M | 51M | **PASS** | PASS |
| `U-02` | naive invocation (scans 6 GB .git) | 6.09 s | 236.5 ms | 390.5 ms* | 209.5 ms | **25.74×** | **1.65×** | **0.89×** | 5.35 | 4.59 | 6.29 | 1772M | 51M | **PASS** | N/A |
| `X-01` | git ls-files \\| xargs | 4.22 s | 479.4 ms | 1.09 s | 960.1 ms | **8.81×** | **2.27×** | **2.00×** | 5.30 | 5.57 | 7.59 | 51M | 51M | **PASS** | PASS |

> **Equivalence Verification:**
> - Gate A (`PASS`): Byte-for-byte exact equality with upstream stock `G0` on `stdout`, normalized `stderr`, and exit code.
> - Gate B (`PASS`): Cross-tool equivalent sorted line set against `R0` and `U0` (differences are listed in section 9).

### 3.1 Master Matrix vs ugrep (Default & Thread-Matched)

The same 59 scenarios with `ugrep` at its default (12 worker threads) and at `-J 32`. Tree scans pass `--hidden`, because `ugrep` skips dotfiles by default while `grep -r` searches them.

| Scenario | Description | G0 (1t) | G1 (32t) | U0 (12t) | U0 (-J 32) | G1 vs G0 | G1 vs U0 (12t) | G1 vs U0 (32t) | G1 CPU | U0 (12t) CPU | U0 (32t) CPU | G1 RSS | U0 RSS | Gate A | Gate B |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| `B-01` | rare literal ($BIG) | 536.9 ms | 19.7 ms | 351.9 ms | 353.8 ms | **27.32×** | **17.90×** | **18.00×** | 0.41 | 0.35 | 0.35 | 1378M | 38M | **PASS** | PASS |
| `B-02` | count dense ($BIG) | 604.9 ms | 19.6 ms | 246.1 ms | 248.3 ms | **30.79×** | **12.53×** | **12.64×** | 0.39 | 0.25 | 0.25 | 1378M | 38M | **PASS** | PASS |
| `B-03` | moderate, line numbers ($BIG) | 934.2 ms | 40.6 ms | 433.6 ms | 433.2 ms | **22.99×** | **10.67×** | **10.66×** | 0.87 | 0.43 | 0.43 | 1375M | 38M | **PASS** | PASS |
| `B-04` | DFA regex ($BIG) | 434.7 ms | 20.0 ms | 429.0 ms | 431.5 ms | **21.75×** | **21.46×** | **21.58×** | 0.39 | 0.43 | 0.43 | 1375M | 38M | **PASS** | PASS |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 44.8 ms | 300.6 ms | 304.1 ms | **43.93×** | **6.71×** | **6.79×** | 0.98 | 0.30 | 0.30 | 1375M | 38M | **PASS** | PASS |
| `B-06` | word match ($BIG) | 370.8 ms | 23.0 ms | 239.2 ms | 240.8 ms | **16.12×** | **10.40×** | **10.47×** | 0.46 | 0.24 | 0.24 | 1377M | 38M | **PASS** | PASS |
| `B-07` | literal-free regex ($BIG) | 663.0 ms | 30.1 ms | 226.5 ms | 228.0 ms | **22.05×** | **7.53×** | **7.58×** | 0.66 | 0.23 | 0.23 | 1377M | 38M | **PASS** | PASS |
| `B-08` | PCRE2 lookaround ($BIG) | 21.40 s | 164.4 ms | 20.81 s | 20.85 s | **130.13×** | **126.53×** | **126.77×** | 2.38 | 20.79 | 20.82 | 1440M | 38M | **PASS** | PASS |
| `B-09` | context ($BIG) | 916.9 ms | 36.6 ms | 380.3 ms | 383.5 ms | **25.06×** | **10.39×** | **10.48×** | 0.82 | 0.38 | 0.38 | 1374M | 38M | **PASS** | PASS |
| `B-10` | only-matching ($BIG) | 767.3 ms | 41.9 ms | 245.6 ms | 246.6 ms | **18.32×** | **5.87×** | **5.89×** | 0.66 | 0.25 | 0.25 | 1377M | 38M | **PASS** | PASS |
| `B-11` | inverted count ($BIG) | 978.8 ms | 46.0 ms | 1.06 s | 1.06 s | **21.28×** | **22.95×** | **22.97×** | 1.00 | 1.05 | 1.05 | 1380M | 38M | **PASS** | PASS |
| `B-12` | stdin redirect (< $BIG) | 530.7 ms | 21.3 ms | 349.8 ms | 352.8 ms | **24.89×** | **16.40×** | **16.54×** | 0.44 | 0.35 | 0.35 | 1377M | 38M | **PASS** | PASS |
| `B-13` | FIFO pipe (cat $BIG \\|) | 1.14 s | 651.3 ms | 813.5 ms | 894.5 ms | **1.74×** | **1.25×** | **1.37×** | 0.88 | 0.73 | 0.81 | 38M | 38M | **PASS** | PASS |
| `B-14` | real 24 MB header (big-24M.h) | 51.5 ms | 16.1 ms | 39.7 ms | 40.6 ms | **3.20×** | **2.46×** | **2.52×** | 0.06 | 0.04 | 0.04 | 38M | 38M | **PASS** | PASS |
| `L-00` | harness floor (/bin/true) | 1.5 ms | 1.5 ms | 1.5 ms | 1.5 ms | **1.01×** | **1.00×** | **1.00×** | 0.00 | 0.00 | 0.00 | 38M | 38M | **PASS** | PASS |
| `L-01` | one small file (init/main.c) | 2.6 ms | 2.6 ms | 4.7 ms | 4.7 ms | **1.00×** | **1.84×** | **1.83×** | 0.00 | 0.00 | 0.00 | 38M | 38M | **PASS** | PASS |
| `L-02` | tiny tree (kernel/sched) | 7.0 ms | 6.0 ms | 8.1 ms | 11.3 ms | **1.17×** | **1.36×** | **1.89×** | 0.01 | 0.02 | 0.03 | 38M | 38M | **PASS** | PASS |
| `L-03` | quiet first match in big file | 2.6 ms | 2.6 ms | 4.9 ms | 4.7 ms | **0.99×** | **1.86×** | **1.79×** | 0.00 | 0.00 | 0.00 | 38M | 38M | **PASS** | PASS |
| `M-01` | 100 literals, tree | 12.19 s | 160.5 ms | 565.7 ms | 939.2 ms | **75.94×** | **3.52×** | **5.85×** | 4.75 | 7.06 | 10.16 | 38M | 38M | **PASS** | PASS |
| `M-02` | 1,000 literals, tree | 14.04 s | 173.3 ms | 570.7 ms | 926.5 ms | **80.98×** | **3.29×** | **5.35×** | 5.12 | 6.93 | 9.94 | 38M | 38M | **PASS** | PASS |
| `M-03` | 10,000 literals, tree | 17.12 s | 239.4 ms | 790.2 ms | 781.6 ms | **71.51×** | **3.30×** | **3.26×** | 5.96 | 8.30 | 9.60 | 56M | 50M | **PASS** | PASS |
| `M-04` | 1,000 literals, -i | 95.54 s | 186.2 ms | 663.6 ms | 870.9 ms | **513.02×** | **3.56×** | **4.68×** | 5.54 | 7.92 | 10.78 | 38M | 38M | **PASS** | PASS |
| `M-05` | 1,000 literals, -w | 14.13 s | 173.5 ms | 575.0 ms | 947.7 ms | **81.46×** | **3.31×** | **5.46×** | 5.11 | 6.98 | 10.11 | 38M | 38M | **PASS** | PASS |
| `M-06` | 1,000 literals, $BIG | 8.92 s | 41.2 ms | 1.46 s | 1.47 s | **216.59×** | **35.51×** | **35.58×** | 0.71 | 1.46 | 1.47 | 1384M | 38M | **PASS** | PASS |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.48 s | 260.4 ms | 1.02 s | 616.9 ms | **159.33×** | **3.91×** | **2.37×** | 8.12 | 12.41 | 13.71 | 51M | 51M | **PASS** | PASS |
| `P-02` | 1,000-way regex alternation | 15.08 s | 185.8 ms | 573.4 ms | 924.7 ms | **81.17×** | **3.09×** | **4.98×** | 5.11 | 6.90 | 9.91 | 51M | 51M | **PASS** | PASS |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.80 s | 154.2 ms | 801.3 ms | 1.27 s | **24.67×** | **5.20×** | **8.26×** | 4.56 | 6.35 | 9.17 | 51M | 51M | **PASS** | PASS |
| `T-01` | zero-match literal | 3.75 s | 150.9 ms | 461.1 ms | 1.00 s | **24.83×** | **3.06×** | **6.63×** | 4.48 | 5.59 | 8.50 | 38M | 38M | **PASS** | PASS |
| `T-02` | rare literal | 3.98 s | 198.1 ms | 461.8 ms | 996.6 ms | **20.11×** | **2.33×** | **5.03×** | 5.52 | 5.73 | 8.63 | 38M | 38M | **PASS** | PASS |
| `T-03` | moderate literal, line numbers | 4.44 s | 153.7 ms | 467.6 ms | 993.6 ms | **28.89×** | **3.04×** | **6.47×** | 4.56 | 5.83 | 8.65 | 38M | 38M | **PASS** | PASS |
| `T-04` | dense literal, line numbers | 4.67 s | 158.1 ms | 627.4 ms | 711.5 ms | **29.51×** | **3.97×** | **4.50×** | 5.16 | 6.15 | 7.16 | 38M | 38M | **PASS** | PASS |
| `T-05` | very dense literal | 4.84 s | 287.3 ms | 1.07 s | 1.24 s | **16.84×** | **3.72×** | **4.33×** | 5.07 | 6.84 | 7.51 | 84M | 38M | **PASS** | PASS |
| `T-06` | files-with-matches | 3.87 s | 152.2 ms | 461.2 ms | 990.4 ms | **25.40×** | **3.03×** | **6.51×** | 4.45 | 5.46 | 8.27 | 109M | 38M | **PASS** | PASS |
| `T-07` | files-without-match | 3.46 s | 150.3 ms | 462.6 ms | 801.4 ms | **23.00×** | **3.08×** | **5.33×** | 4.43 | 5.37 | 7.47 | 108M | 38M | **PASS** | PASS |
| `T-08` | count per file | 3.78 s | 151.2 ms | 1.12 s | 1.61 s | **25.02×** | **7.43×** | **10.68×** | 4.53 | 7.05 | 7.91 | 38M | 38M | **PASS** | PASS |
| `T-09` | case-insensitive literal | 5.30 s | 161.1 ms | 784.7 ms | 1.06 s | **32.87×** | **4.87×** | **6.61×** | 4.77 | 6.68 | 7.51 | 38M | 38M | **PASS** | KNOWN-SEMANTIC |
| `T-10` | word match | 4.19 s | 157.8 ms | 461.4 ms | 1000.0 ms | **26.55×** | **2.92×** | **6.34×** | 4.64 | 5.70 | 8.77 | 38M | 38M | **PASS** | PASS |
| `T-11` | DFA regex with must-literal | 3.79 s | 155.1 ms | 460.4 ms | 1.01 s | **24.44×** | **2.97×** | **6.50×** | 4.63 | 5.75 | 8.93 | 38M | 38M | **PASS** | PASS |
| `T-12` | anchored dense regex | 4.69 s | 166.0 ms | 667.0 ms | 786.9 ms | **28.26×** | **4.02×** | **4.74×** | 4.83 | 6.30 | 7.02 | 38M | 38M | **PASS** | PASS |
| `T-13` | literal alternation + word boundary | 3.98 s | 155.6 ms | 453.0 ms | 997.6 ms | **25.56×** | **2.91×** | **6.41×** | 4.55 | 5.64 | 8.53 | 38M | 38M | **PASS** | PASS |
| `T-14` | optional group + alternation | 3.85 s | 154.1 ms | 453.3 ms | 998.5 ms | **25.02×** | **2.94×** | **6.48×** | 4.52 | 5.60 | 8.46 | 38M | 38M | **PASS** | PASS |
| `T-15` | literal-free regex | 4.14 s | 163.5 ms | 447.9 ms | 992.3 ms | **25.34×** | **2.74×** | **6.07×** | 4.90 | 5.55 | 8.44 | 38M | 38M | **PASS** | PASS |
| `T-16` | PCRE2 lookaround, dense | 27.63 s | 228.5 ms | 2.58 s | 1.02 s | **120.91×** | **11.29×** | **4.48×** | 7.00 | 31.03 | 31.66 | 69M | 38M | **PASS** | PASS |
| `T-17` | PCRE2 rare with literal | 5.36 s | 155.7 ms | 466.3 ms | 1.00 s | **34.43×** | **3.00×** | **6.44×** | 4.52 | 5.78 | 8.71 | 71M | 38M | **PASS** | PASS |
| `T-18` | only-matching | 4.17 s | 205.6 ms | 462.6 ms | 954.7 ms | **20.29×** | **2.25×** | **4.64×** | 5.72 | 5.73 | 8.57 | 38M | 38M | **PASS** | PASS |
| `T-19` | context, rare | 4.42 s | 156.2 ms | 460.8 ms | 980.6 ms | **28.31×** | **2.95×** | **6.28×** | 4.58 | 5.75 | 8.73 | 38M | 38M | **PASS** | KNOWN-SEMANTIC |
| `T-20` | context, moderate | 4.44 s | 151.8 ms | 477.0 ms | 961.0 ms | **29.27×** | **3.14×** | **6.33×** | 4.52 | 5.94 | 8.84 | 38M | 38M | **PASS** | KNOWN-SEMANTIC |
| `T-21` | inverted count | 4.51 s | 178.3 ms | 1.13 s | 1.60 s | **25.31×** | **6.32×** | **8.96×** | 5.41 | 8.06 | 8.97 | 38M | 38M | **PASS** | SOFT_DIFF |
| `T-22` | glob include | 2.11 s | 81.1 ms | 368.4 ms | 552.9 ms | **26.08×** | **4.54×** | **6.82×** | 2.00 | 2.64 | 3.74 | 38M | 38M | **PASS** | PASS |
| `T-23` | glob + sub-scope | 123.4 ms | 15.9 ms | 26.5 ms | 31.2 ms | **7.74×** | **1.66×** | **1.96×** | 0.16 | 0.21 | 0.27 | 38M | 38M | **PASS** | PASS |
| `T-24` | max-count per file | 3.83 s | 153.6 ms | 449.6 ms | 978.8 ms | **24.97×** | **2.93×** | **6.37×** | 4.55 | 5.62 | 8.45 | 38M | 38M | **PASS** | PASS |
| `T-25a` | quiet, no match | 3.71 s | 155.4 ms | 472.6 ms | 981.7 ms | **23.89×** | **3.04×** | **6.32×** | 4.53 | 5.39 | 8.15 | 132M | 38M | **PASS** | PASS |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | 12.5 ms | 11.5 ms | **1.00×** | **4.59×** | **4.20×** | 0.00 | 0.05 | 0.03 | 38M | 38M | **PASS** | PASS |
| `T-26` | byte offsets | 4.44 s | 152.0 ms | 467.2 ms | 985.5 ms | **29.18×** | **3.07×** | **6.48×** | 4.52 | 5.82 | 8.65 | 38M | 38M | **PASS** | KNOWN-SEMANTIC |
| `T-27` | sub-scope drivers/ | 2.01 s | 73.6 ms | 212.2 ms | 394.2 ms | **27.35×** | **2.88×** | **5.36×** | 1.95 | 2.51 | 3.72 | 51M | 51M | **PASS** | PASS |
| `T-28` | sub-scope kernel mm fs | 168.2 ms | 17.1 ms | 24.5 ms | 40.4 ms | **9.83×** | **1.43×** | **2.36×** | 0.17 | 0.21 | 0.34 | 51M | 51M | **PASS** | PASS |
| `U-01` | out-of-the-box UX | 4.41 s | 151.3 ms | 462.8 ms | 980.6 ms | **29.11×** | **3.06×** | **6.48×** | 4.54 | 5.78 | 8.57 | 51M | 51M | **PASS** | PASS |
| `U-02` | naive invocation (scans 6 GB .git) | 6.09 s | 236.5 ms | 466.1 ms* | 978.7 ms | **25.74×** | **1.97×** | **4.14×** | 5.35 | 5.82 | 8.67 | 1772M | 51M | **PASS** | N/A |
| `X-01` | git ls-files \\| xargs | 4.22 s | 479.4 ms | 1.53 s | 1.93 s | **8.81×** | **3.19×** | **4.03×** | 5.30 | 8.04 | 10.83 | 51M | 51M | **PASS** | PASS |

---

## 4. Category Deep-Dives & Architectural Analysis

### Category T: Whole-Tree Traversal (29 Scenarios, 96,034 Files, 1.64 GB)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | U0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 vs U0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `T-01` | zero-match literal | 3.75 s | 150.9 ms | 344.3 ms | 461.1 ms | **24.83×** | **2.28×** | **3.06×** | 4.48 | 4.03 | 38M | 38M | **PASS** |
| `T-02` | rare literal | 3.98 s | 198.1 ms | 352.3 ms | 461.8 ms | **20.11×** | **1.78×** | **2.33×** | 5.52 | 4.18 | 38M | 38M | **PASS** |
| `T-03` | moderate literal, line numbers | 4.44 s | 153.7 ms | 365.0 ms | 467.6 ms | **28.89×** | **2.37×** | **3.04×** | 4.56 | 4.31 | 38M | 38M | **PASS** |
| `T-04` | dense literal, line numbers | 4.67 s | 158.1 ms | 505.1 ms | 627.4 ms | **29.51×** | **3.19×** | **3.97×** | 5.16 | 5.70 | 38M | 38M | **PASS** |
| `T-05` | very dense literal | 4.84 s | 287.3 ms | 763.0 ms | 1.07 s | **16.84×** | **2.66×** | **3.72×** | 5.07 | 7.14 | 84M | 38M | **PASS** |
| `T-06` | files-with-matches | 3.87 s | 152.2 ms | 358.1 ms | 461.2 ms | **25.40×** | **2.35×** | **3.03×** | 4.45 | 4.19 | 109M | 38M | **PASS** |
| `T-07` | files-without-match | 3.46 s | 150.3 ms | 420.0 ms | 462.6 ms | **23.00×** | **2.79×** | **3.08×** | 4.43 | 4.91 | 108M | 38M | **PASS** |
| `T-08` | count per file | 3.78 s | 151.2 ms | 1.07 s | 1.12 s | **25.02×** | **7.10×** | **7.43×** | 4.53 | 9.87 | 38M | 38M | **PASS** |
| `T-09` | case-insensitive literal | 5.30 s | 161.1 ms | 723.7 ms | 784.7 ms | **32.87×** | **4.49×** | **4.87×** | 4.77 | 7.66 | 38M | 38M | **PASS** |
| `T-10` | word match | 4.19 s | 157.8 ms | 373.2 ms | 461.4 ms | **26.55×** | **2.36×** | **2.92×** | 4.64 | 4.38 | 38M | 38M | **PASS** |
| `T-11` | DFA regex with must-literal | 3.79 s | 155.1 ms | 351.0 ms | 460.4 ms | **24.44×** | **2.26×** | **2.97×** | 4.63 | 4.13 | 38M | 38M | **PASS** |
| `T-12` | anchored dense regex | 4.69 s | 166.0 ms | 592.0 ms | 667.0 ms | **28.26×** | **3.57×** | **4.02×** | 4.83 | 6.41 | 38M | 38M | **PASS** |
| `T-13` | literal alternation + word boundary | 3.98 s | 155.6 ms | 369.7 ms | 453.0 ms | **25.56×** | **2.38×** | **2.91×** | 4.55 | 4.36 | 38M | 38M | **PASS** |
| `T-14` | optional group + alternation | 3.85 s | 154.1 ms | 349.5 ms | 453.3 ms | **25.02×** | **2.27×** | **2.94×** | 4.52 | 4.08 | 38M | 38M | **PASS** |
| `T-15` | literal-free regex | 4.14 s | 163.5 ms | 378.5 ms | 447.9 ms | **25.34×** | **2.31×** | **2.74×** | 4.90 | 4.45 | 38M | 38M | **PASS** |
| `T-16` | PCRE2 lookaround, dense | 27.63 s | 228.5 ms | 2.49 s | 2.58 s | **120.91×** | **10.90×** | **11.29×** | 7.00 | 29.73 | 69M | 38M | **PASS** |
| `T-17` | PCRE2 rare with literal | 5.36 s | 155.7 ms | 524.9 ms | 466.3 ms | **34.43×** | **3.37×** | **3.00×** | 4.52 | 6.22 | 71M | 38M | **PASS** |
| `T-18` | only-matching | 4.17 s | 205.6 ms | 382.2 ms | 462.6 ms | **20.29×** | **1.86×** | **2.25×** | 5.72 | 4.50 | 38M | 38M | **PASS** |
| `T-19` | context, rare | 4.42 s | 156.2 ms | 364.8 ms | 460.8 ms | **28.31×** | **2.34×** | **2.95×** | 4.58 | 4.29 | 38M | 38M | **PASS** |
| `T-20` | context, moderate | 4.44 s | 151.8 ms | 382.0 ms | 477.0 ms | **29.27×** | **2.52×** | **3.14×** | 4.52 | 4.48 | 38M | 38M | **PASS** |
| `T-21` | inverted count | 4.51 s | 178.3 ms | 1.09 s | 1.13 s | **25.31×** | **6.12×** | **6.32×** | 5.41 | 10.89 | 38M | 38M | **PASS** |
| `T-22` | glob include | 2.11 s | 81.1 ms | 174.1 ms | 368.4 ms | **26.08×** | **2.15×** | **4.54×** | 2.00 | 1.99 | 38M | 38M | **PASS** |
| `T-23` | glob + sub-scope | 123.4 ms | 15.9 ms | 24.9 ms | 26.5 ms | **7.74×** | **1.56×** | **1.66×** | 0.16 | 0.20 | 38M | 38M | **PASS** |
| `T-24` | max-count per file | 3.83 s | 153.6 ms | 360.6 ms | 449.6 ms | **24.97×** | **2.35×** | **2.93×** | 4.55 | 4.21 | 38M | 38M | **PASS** |
| `T-25a` | quiet, no match | 3.71 s | 155.4 ms | 344.7 ms | 472.6 ms | **23.89×** | **2.22×** | **3.04×** | 4.53 | 4.04 | 132M | 38M | **PASS** |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | 6.0 ms | 12.5 ms | **1.00×** | **2.19×** | **4.59×** | 0.00 | 0.01 | 38M | 38M | **PASS** |
| `T-26` | byte offsets | 4.44 s | 152.0 ms | 363.0 ms | 467.2 ms | **29.18×** | **2.39×** | **3.07×** | 4.52 | 4.27 | 38M | 38M | **PASS** |
| `T-27` | sub-scope drivers/ | 2.01 s | 73.6 ms | 165.1 ms | 212.2 ms | **27.35×** | **2.24×** | **2.88×** | 1.95 | 1.90 | 51M | 51M | **PASS** |
| `T-28` | sub-scope kernel mm fs | 168.2 ms | 17.1 ms | 19.1 ms | 24.5 ms | **9.83×** | **1.12×** | **1.43×** | 0.17 | 0.16 | 51M | 51M | **PASS** |

#### Architectural Analysis (Category T):
- **High-Density Output Dominance:** G1 dramatically outclasses ripgrep on scenarios with high match volume (`T-04` dense line numbers: **158.1 ms vs 505.1 ms / 512.6 ms**, **3.19× / 3.24×**; `T-05` very dense: **287.3 ms vs 763.0 ms / 979.0 ms**, **2.66× / 3.41×**). Ripgrep serializes line formatting and terminal output through a shared mutex-locked buffer, causing severe thread lock contention when output is dense. G1 uses thread-local output slabs combined with an in-order Sequence Reorder Buffer (ROB) that commits complete file buffers locklessly.
- **Count Queries:** In count-per-file queries (`T-08` count: **151.2 ms vs 1.07 s / 1.44 s**, **7.10× / 9.49×**; `T-21` inverted count: **178.3 ms vs 1.09 s / 1.44 s**, **6.12× / 8.09×**), G1 performs line-counting using optimized AVX2 SIMD `nlscan` kernels while skipping all text output serialization. Ripgrep incurs heavy per-file state tracking and formatting overhead.
- **Case-Insensitive & PCRE2 Tree Scans:** On `T-09` (`-ri copyright`), G1 achieves **161.1 ms vs 723.7 ms / 928.0 ms** (**4.49× / 5.76×** faster than ripgrep). On `T-16` (`-rP` lookaround), G1's structural anchor-pair pre-filter (`struct ` + `;`) eliminates 99.6% of non-matching lines before invoking PCRE2 JIT, finishing in **228.5 ms vs 2.49 s / 1.03 s** (**10.90× / 4.51×** faster than ripgrep and **120.91×** faster than stock G0).
- **vs ugrep:** Against `ugrep` at its default (12t) / `-J 32`, G1's category geomean is **3.34× / 5.58×** (29 Wins / 0 Ties / 0 Losses at the default; 29 Wins / 0 Ties / 0 Losses at `-J 32`). Largest gap: `T-16` PCRE2 lookaround, dense (**228.5 ms vs 2.58 s**, **11.29×**); smallest: `T-28` sub-scope kernel mm fs (**17.1 ms vs 24.5 ms**, **1.43×**).

---

### Category B: Single Huge File Parallelism (14 Scenarios, `$BIG` ≈ 1.45 GB)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | U0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 vs U0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `B-01` | rare literal ($BIG) | 536.9 ms | 19.7 ms | 171.5 ms | 351.9 ms | **27.32×** | **8.72×** | **17.90×** | 0.41 | 0.17 | 1378M | 1391M | **PASS** |
| `B-02` | count dense ($BIG) | 604.9 ms | 19.6 ms | 192.2 ms | 246.1 ms | **30.79×** | **9.78×** | **12.53×** | 0.39 | 0.19 | 1378M | 1391M | **PASS** |
| `B-03` | moderate, line numbers ($BIG) | 934.2 ms | 40.6 ms | 246.2 ms | 433.6 ms | **22.99×** | **6.06×** | **10.67×** | 0.87 | 0.25 | 1375M | 1392M | **PASS** |
| `B-04` | DFA regex ($BIG) | 434.7 ms | 20.0 ms | 173.8 ms | 429.0 ms | **21.75×** | **8.69×** | **21.46×** | 0.39 | 0.17 | 1375M | 1392M | **PASS** |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 44.8 ms | 300.3 ms | 300.6 ms | **43.93×** | **6.71×** | **6.71×** | 0.98 | 0.30 | 1375M | 1392M | **PASS** |
| `B-06` | word match ($BIG) | 370.8 ms | 23.0 ms | 180.6 ms | 239.2 ms | **16.12×** | **7.85×** | **10.40×** | 0.46 | 0.18 | 1377M | 1391M | **PASS** |
| `B-07` | literal-free regex ($BIG) | 663.0 ms | 30.1 ms | 1.58 s | 226.5 ms | **22.05×** | **52.40×** | **7.53×** | 0.66 | 1.58 | 1377M | 1391M | **PASS** |
| `B-08` | PCRE2 lookaround ($BIG) | 21.40 s | 164.4 ms | 21.61 s | 20.81 s | **130.13×** | **131.41×** | **126.53×** | 2.38 | 21.63 | 1440M | 1394M | **PASS** |
| `B-09` | context ($BIG) | 916.9 ms | 36.6 ms | 272.8 ms | 380.3 ms | **25.06×** | **7.46×** | **10.39×** | 0.82 | 0.27 | 1374M | 1391M | **PASS** |
| `B-10` | only-matching ($BIG) | 767.3 ms | 41.9 ms | 188.0 ms | 245.6 ms | **18.32×** | **4.49×** | **5.87×** | 0.66 | 0.19 | 1377M | 1392M | **PASS** |
| `B-11` | inverted count ($BIG) | 978.8 ms | 46.0 ms | 1.46 s | 1.06 s | **21.28×** | **31.80×** | **22.95×** | 1.00 | 1.46 | 1380M | 1391M | **PASS** |
| `B-12` | stdin redirect (< $BIG) | 530.7 ms | 21.3 ms | 256.7 ms | 349.8 ms | **24.89×** | **12.04×** | **16.40×** | 0.44 | 0.26 | 1377M | 38M | **PASS** |
| `B-13` | FIFO pipe (cat $BIG \\|) | 1.14 s | 651.3 ms | 853.2 ms | 813.5 ms | **1.74×** | **1.31×** | **1.25×** | 0.88 | 0.62 | 38M | 38M | **PASS** |
| `B-14` | real 24 MB header (big-24M.h) | 51.5 ms | 16.1 ms | 44.8 ms | 39.7 ms | **3.20×** | **2.78×** | **2.46×** | 0.06 | 0.04 | 38M | 38M | **PASS** |

#### Architectural Analysis (Category B):
- **Intra-File Parallelism Gap:** Ripgrep processes single files with a single thread regardless of `-j` flags, taking 171 ms to 300 ms (and 21.6 s on PCRE2 lookaround). G1 maps large files and partitions them across the worker pool into parallel chunks with seamless line boundary handling, achieving **19.7 ms on literals** (**8.72× faster than ripgrep**), **44.8 ms on case-insensitive** (**6.71× faster**), and **164.4 ms on PCRE2 lookaround** (**131.41× faster**).
- **Stream and FIFO Slicing:** Even when reading from stdin redirect (`B-12`: **21.3 ms vs 256.7 ms**, **12.04×**), G1 slices the incoming stream into chunked worker buffers for parallel execution.
- **vs ugrep:** Against `ugrep` at its default (12t) / `-J 32`, G1's category geomean is **10.65× / 10.79×** (14 Wins / 0 Ties / 0 Losses at the default; 14 Wins / 0 Ties / 0 Losses at `-J 32`). Largest gap: `B-08` PCRE2 lookaround ($BIG) (**164.4 ms vs 20.81 s**, **126.53×**); smallest: `B-13` FIFO pipe (cat $BIG \|) (**651.3 ms vs 813.5 ms**, **1.25×**).

---

### Category M: Multi-Pattern Dictionary Sets (6 Scenarios)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | U0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 vs U0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `M-01` | 100 literals, tree | 12.19 s | 160.5 ms | 646.9 ms | 565.7 ms | **75.94×** | **4.03×** | **3.52×** | 4.75 | 7.67 | 38M | 38M | **PASS** |
| `M-02` | 1,000 literals, tree | 14.04 s | 173.3 ms | 719.0 ms | 570.7 ms | **80.98×** | **4.15×** | **3.29×** | 5.12 | 8.34 | 38M | 65M | **PASS** |
| `M-03` | 10,000 literals, tree | 17.12 s | 239.4 ms | 1.16 s | 790.2 ms | **71.51×** | **4.83×** | **3.30×** | 5.96 | 13.45 | 56M | 38M | **PASS** |
| `M-04` | 1,000 literals, -i | 95.54 s | 186.2 ms | 1.55 s | 663.6 ms | **513.02×** | **8.33×** | **3.56×** | 5.54 | 17.92 | 38M | 146M | **PASS** |
| `M-05` | 1,000 literals, -w | 14.13 s | 173.5 ms | 898.9 ms | 575.0 ms | **81.46×** | **5.18×** | **3.31×** | 5.11 | 10.46 | 38M | 74M | **PASS** |
| `M-06` | 1,000 literals, $BIG | 8.92 s | 41.2 ms | 3.11 s | 1.46 s | **216.59×** | **75.50×** | **35.51×** | 0.71 | 3.13 | 1384M | 1404M | **PASS** |

#### Architectural Analysis (Category M):
- **Dense Aho-Corasick & AVX2 Teddy Filter:** Stock GNU grep scales poorly on large dictionary sets because its kwset trie degrades under thousands of patterns (taking 12.2 s to 95.5 s). G1 compiles pattern sets into a dense, cache-friendly Aho-Corasick automaton backed by 16-way and 32-way AVX2 SIMD pre-filter vector kernels.
- **Tree and Single File Speedups:** Across 100 to 10,000 patterns, G1 scans the entire 1.64 GB kernel tree in **160.5 ms to 239.4 ms** (**71.51× to 513.02× faster than G0**, and **4.03× to 8.33× faster than ripgrep**). On single large files (`M-06`), G1 finishes in **41.2 ms** (**216.59× faster than G0**, and **75.50× faster than ripgrep**).
- **vs ugrep:** Against `ugrep` at its default (12t) / `-J 32`, G1's category geomean is **5.02× / 6.73×** (6 Wins / 0 Ties / 0 Losses at the default; 6 Wins / 0 Ties / 0 Losses at `-J 32`). Largest gap: `M-06` 1,000 literals, $BIG (**41.2 ms vs 1.46 s**, **35.51×**); smallest: `M-02` 1,000 literals, tree (**173.3 ms vs 570.7 ms**, **3.29×**).

---

### Category L: Invocation Latency & Micro-Benchmarks

#### Detailed Latency Percentiles (Raw vs Net)

| Scenario | Tool | N | Raw p50 | Raw p90 | Raw p99 | Net p50 | Net p90 | Net p99 | CPU (s) | RSS |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `L-00` | **G0** | 100 | 1.5 ms | 1.7 ms | 1.8 ms | 0.00 ms | 0.22 ms | 0.34 ms | 0.00 | 38M |
| `L-00` | **G1** | 100 | 1.5 ms | 1.6 ms | 1.7 ms | 0.00 ms | 0.17 ms | 0.26 ms | 0.00 | 38M |
| `L-00` | **R0** | 100 | 1.5 ms | 1.6 ms | 1.8 ms | 0.00 ms | 0.14 ms | 0.35 ms | 0.00 | 38M |
| `L-00` | **U0** | 100 | 1.5 ms | 1.6 ms | 1.8 ms | 0.00 ms | 0.11 ms | 0.37 ms | 0.00 | 38M |
| `L-01` | **G0** | 100 | 2.6 ms | 2.8 ms | 2.8 ms | 1.1 ms | 1.3 ms | 1.4 ms | 0.00 | 38M |
| `L-01` | **G1** | 100 | 2.6 ms | 2.7 ms | 2.9 ms | 1.1 ms | 1.3 ms | 1.4 ms | 0.00 | 38M |
| `L-01` | **R0** | 100 | 3.6 ms | 3.8 ms | 5.0 ms | 2.2 ms | 2.4 ms | 3.5 ms | 0.00 | 38M |
| `L-01` | **U0** | 100 | 4.7 ms | 6.0 ms | 6.2 ms | 3.3 ms | 4.5 ms | 4.7 ms | 0.00 | 38M |
| `L-02` | **G0** | 50 | 7.0 ms | 7.2 ms | 7.3 ms | 5.5 ms | 5.7 ms | 5.8 ms | 0.01 | 38M |
| `L-02` | **G1** | 50 | 6.0 ms | 6.9 ms | 7.3 ms | 4.5 ms | 5.4 ms | 5.8 ms | 0.01 | 38M |
| `L-02` | **R0** | 50 | 7.1 ms | 8.1 ms | 8.4 ms | 5.6 ms | 6.6 ms | 7.0 ms | 0.01 | 38M |
| `L-02` | **U0** | 50 | 8.1 ms | 9.1 ms | 9.3 ms | 6.7 ms | 7.7 ms | 7.8 ms | 0.02 | 38M |
| `L-03` | **G0** | 30 | 2.6 ms | 2.8 ms | 2.9 ms | 1.1 ms | 1.3 ms | 1.5 ms | 0.00 | 38M |
| `L-03` | **G1** | 30 | 2.6 ms | 2.8 ms | 2.9 ms | 1.2 ms | 1.3 ms | 1.4 ms | 0.00 | 38M |
| `L-03` | **R0** | 30 | 3.7 ms | 3.8 ms | 3.9 ms | 2.2 ms | 2.3 ms | 2.5 ms | 0.00 | 38M |
| `L-03` | **U0** | 30 | 4.9 ms | 6.0 ms | 6.0 ms | 3.4 ms | 4.5 ms | 4.6 ms | 0.00 | 38M |

#### Summary Comparison (Net Latency Median)

| Scenario | Description | G0 Net | G1 Net | R0 Net | U0 Net | G1 vs G0 | G1 vs R0 | G1 vs U0 | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `L-00` | harness floor (/bin/true) | 0.00 ms | 0.00 ms | 0.00 ms | 0.00 ms | **—** | **—** | **—** | **PASS** |
| `L-01` | one small file (init/main.c) | 1.1 ms | 1.1 ms | 2.2 ms | 3.3 ms | **0.99×** | **1.95×** | **2.93×** | **PASS** |
| `L-02` | tiny tree (kernel/sched) | 5.5 ms | 4.5 ms | 5.6 ms | 6.7 ms | **1.22×** | **1.24×** | **1.48×** | **PASS** |
| `L-03` | quiet first match in big file | 1.1 ms | 1.2 ms | 2.2 ms | 3.4 ms | **0.97×** | **1.90×** | **2.95×** | **PASS** |

#### Latency Insights:
- **Zero Thread-Pool Penalty:** G1 thread-pool creation is lazy: small file queries (`L-01`) and immediate early-exit tree queries (`T-25b`) search on the calling thread before spawning worker threads. Net startup latency on `L-01` is **1.1 ms** (matching single-threaded stock GNU grep at 1.1 ms), while ripgrep takes **2.2 ms** net latency.
- **ugrep Startup:** `ugrep` takes **3.3 ms** net on `L-01` (4.7 ms raw) and **12.5 ms** on the immediate quiet tree match (`T-25b`).

---

### Categories U, X, and P: UX, Tools, and Complex Regexes

#### Category U — User Experience Defaults

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | U0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 vs U0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `U-01` | out-of-the-box UX | 4.41 s | 151.3 ms | 390.7 ms | 462.8 ms | **29.11×** | **2.58×** | **3.06×** | 4.54 | 4.58 | 51M | 51M | **PASS** |
| `U-02` | naive invocation (scans 6 GB .git) | 6.09 s | 236.5 ms | 390.5 ms* | 466.1 ms* | **25.74×** | **1.65×** | **1.97×** | 5.35 | 4.59 | 1772M | 51M | **PASS** |

#### Category X — xargs / External Parallelism

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | U0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 vs U0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `X-01` | git ls-files \\| xargs | 4.22 s | 479.4 ms | 1.09 s | 1.53 s | **8.81×** | **2.27×** | **3.19×** | 5.30 | 5.57 | 51M | 51M | **PASS** |

#### Category P — Pathological / Complex Regexes

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | U0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 vs U0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.48 s | 260.4 ms | 835.8 ms | 1.02 s | **159.33×** | **3.21×** | **3.91×** | 8.12 | 9.93 | 51M | 51M | **PASS** |
| `P-02` | 1,000-way regex alternation | 15.08 s | 185.8 ms | 1.19 s | 573.4 ms | **81.17×** | **6.42×** | **3.09×** | 5.11 | 13.87 | 51M | 134M | **PASS** |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.80 s | 154.2 ms | 350.9 ms | 801.3 ms | **24.67×** | **2.28×** | **5.20×** | 4.56 | 4.11 | 51M | 51M | **PASS** |

#### Architectural Analysis vs ugrep (Categories U, X, and P):
- **Category U vs ugrep:** Against `ugrep` at its default (12t) / `-J 32`, G1's category geomean is **2.46× / 5.18×** (2 Wins / 0 Ties / 0 Losses at the default; 2 Wins / 0 Ties / 0 Losses at `-J 32`). Largest gap: `U-01` out-of-the-box UX (**151.3 ms vs 462.8 ms**, **3.06×**); smallest: `U-02` naive invocation (scans 6 GB .git) (**236.5 ms vs 466.1 ms**, **1.97×**).
- **Category X vs ugrep:** Against `ugrep` at its default (12t) / `-J 32`, G1's category geomean is **3.19× / 4.03×** (1 Win / 0 Ties / 0 Losses at the default; 1 Win / 0 Ties / 0 Losses at `-J 32`). `X-01` git ls-files \| xargs: **479.4 ms vs 1.53 s**, **3.19×**.
- **Category P vs ugrep:** Against `ugrep` at its default (12t) / `-J 32`, G1's category geomean is **3.97× / 4.60×** (3 Wins / 0 Ties / 0 Losses at the default; 3 Wins / 0 Ties / 0 Losses at `-J 32`). Largest gap: `P-03` dot-star heavy (.*_lock.*_unlock) (**154.2 ms vs 801.3 ms**, **5.20×**); smallest: `P-02` 1,000-way regex alternation (**185.8 ms vs 573.4 ms**, **3.09×**).

---

## 5. Locale Sensitivity Analysis: en_US.UTF-8 vs LC_ALL=C

### Locale Sensitivity: en_US.UTF-8 vs LC_ALL=C (22 Scenarios)

> Ratio = UTF-8 median / C median. A ratio > 1.00× indicates that the C locale is faster (multibyte/case-folding overhead in UTF-8).

| Scenario | Description | G0 UTF-8 | G0 C | G0 (U/C) | G1 UTF-8 | G1 C | G1 (U/C) | R0 UTF-8 | R0 C | R0 (U/C) | U0 UTF-8 | U0 C | U0 (U/C) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | rare literal ($BIG) | 536.9 ms | 535.9 ms | **1.00×** | 19.7 ms | 22.0 ms | **0.89×** | 171.5 ms | 172.0 ms | **1.00×** | 351.9 ms | 350.8 ms | **1.00×** |
| `B-02` | count dense ($BIG) | 604.9 ms | 603.9 ms | **1.00×** | 19.6 ms | 21.2 ms | **0.93×** | 192.2 ms | 195.9 ms | **0.98×** | 246.1 ms | 243.9 ms | **1.01×** |
| `B-03` | moderate, line numbers ($BIG) | 934.2 ms | 933.7 ms | **1.00×** | 40.6 ms | 44.5 ms | **0.91×** | 246.2 ms | 249.2 ms | **0.99×** | 433.6 ms | 433.5 ms | **1.00×** |
| `B-04` | DFA regex ($BIG) | 434.7 ms | 432.3 ms | **1.01×** | 20.0 ms | 21.8 ms | **0.92×** | 173.8 ms | 173.2 ms | **1.00×** | 429.0 ms | 430.6 ms | **1.00×** |
| `B-05` | case-insensitive dense ($BIG) | 1.97 s | 1.30 s | **1.52×** | 44.8 ms | 42.0 ms | **1.07×** | 300.3 ms | 298.6 ms | **1.01×** | 300.6 ms | 305.2 ms | **0.98×** |
| `B-07` | literal-free regex ($BIG) | 663.0 ms | 662.5 ms | **1.00×** | 30.1 ms | 31.3 ms | **0.96×** | 1.58 s | 1.58 s | **1.00×** | 226.5 ms | 225.8 ms | **1.00×** |
| `B-08` | PCRE2 lookaround ($BIG) | 21.40 s | 7.23 s | **2.96×** | 164.4 ms | 69.7 ms | **2.36×** | 21.61 s | 21.60 s | **1.00×** | 20.81 s | 20.78 s | **1.00×** |
| `M-02` | 1,000 literals, tree | 14.04 s | 14.03 s | **1.00×** | 173.3 ms | 205.1 ms | **0.85×** | 719.0 ms | 730.2 ms | **0.98×** | 570.7 ms | 588.9 ms | **0.97×** |
| `M-04` | 1,000 literals, -i | 95.54 s | 18.43 s | **5.18×** | 186.2 ms | 181.2 ms | **1.03×** | 1.55 s | 1.55 s | **1.00×** | 663.6 ms | 692.9 ms | **0.96×** |
| `T-01` | zero-match literal | 3.75 s | 3.79 s | **0.99×** | 150.9 ms | 152.7 ms | **0.99×** | 344.3 ms | 351.4 ms | **0.98×** | 461.1 ms | 468.6 ms | **0.98×** |
| `T-02` | rare literal | 3.98 s | 4.00 s | **1.00×** | 198.1 ms | 156.1 ms | **1.27×** | 352.3 ms | 350.9 ms | **1.00×** | 461.8 ms | 478.3 ms | **0.97×** |
| `T-03` | moderate literal, line numbers | 4.44 s | 4.47 s | **0.99×** | 153.7 ms | 152.3 ms | **1.01×** | 365.0 ms | 373.8 ms | **0.98×** | 467.6 ms | 484.0 ms | **0.97×** |
| `T-04` | dense literal, line numbers | 4.67 s | 4.68 s | **1.00×** | 158.1 ms | 158.6 ms | **1.00×** | 505.1 ms | 512.1 ms | **0.99×** | 627.4 ms | 664.4 ms | **0.94×** |
| `T-05` | very dense literal | 4.84 s | 4.79 s | **1.01×** | 287.3 ms | 299.4 ms | **0.96×** | 763.0 ms | 738.9 ms | **1.03×** | 1.07 s | 1.15 s | **0.93×** |
| `T-06` | files-with-matches | 3.87 s | 3.91 s | **0.99×** | 152.2 ms | 154.2 ms | **0.99×** | 358.1 ms | 363.7 ms | **0.98×** | 461.2 ms | 459.7 ms | **1.00×** |
| `T-09` | case-insensitive literal | 5.30 s | 4.46 s | **1.19×** | 161.1 ms | 157.4 ms | **1.02×** | 723.7 ms | 734.9 ms | **0.98×** | 784.7 ms | 858.8 ms | **0.91×** |
| `T-10` | word match | 4.19 s | 4.28 s | **0.98×** | 157.8 ms | 155.0 ms | **1.02×** | 373.2 ms | 385.1 ms | **0.97×** | 461.4 ms | 474.7 ms | **0.97×** |
| `T-11` | DFA regex with must-literal | 3.79 s | 3.87 s | **0.98×** | 155.1 ms | 149.8 ms | **1.04×** | 351.0 ms | 358.6 ms | **0.98×** | 460.4 ms | 480.8 ms | **0.96×** |
| `T-12` | anchored dense regex | 4.69 s | 4.57 s | **1.03×** | 166.0 ms | 164.6 ms | **1.01×** | 592.0 ms | 639.6 ms | **0.93×** | 667.0 ms | 745.5 ms | **0.89×** |
| `T-15` | literal-free regex | 4.14 s | 4.23 s | **0.98×** | 163.5 ms | 212.9 ms | **0.77×** | 378.5 ms | 388.3 ms | **0.97×** | 447.9 ms | 456.2 ms | **0.98×** |
| `T-16` | PCRE2 lookaround, dense | 27.63 s | 11.79 s | **2.34×** | 228.5 ms | 196.4 ms | **1.16×** | 2.49 s | 2.53 s | **0.98×** | 2.58 s | 2.60 s | **0.99×** |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | **1.02×** | 2.7 ms | 2.8 ms | **0.98×** | 6.0 ms | 4.9 ms | **1.22×** | 12.5 ms | 8.5 ms | **1.46×** |

### Key Observations on Locale Sensitivity:
- **ASCII Case-Fold Acceleration:** Upstream GNU grep experiences a 5.18× slowdown on multi-pattern case-insensitive search (`M-04`) and 1.52× slowdown on single-file case-insensitive search (`B-05`) in UTF-8 locales. G1's AVX2 ASCII case-folding fast path reduces the UTF-8 penalty to near zero (**1.02× to 1.07×**), matching C locale throughput.
- **ugrep Ignores the Locale:** `ugrep` matches Unicode whatever `LC_ALL` says, so it does the same work in both locales; its UTF-8/C ratio is 0.89×–1.01× on the 21 scenarios that take at least 100 ms (the 10 ms `T-25b` varies more), as ripgrep's does.

---

## 6. Cold Page Cache Performance (fadvise Evicted)

### Cold Page Cache Performance (fadvise Evicted)

> Eviction via posix_fadvise(POSIX_FADV_DONTNEED) with residency verification (< 2% resident) before every run.
> Cold/Warm Ratio = Cold median / Warm median. Cold Throughput = Corpus Size / Cold Time.

| Scenario | Description | Tool | Warm (ms) | Cold (ms) | Cold / Warm Ratio | Cold Throughput |
| :--- | :--- | :---: | ---: | ---: | ---: | ---: |
| `B-01` | rare literal ($BIG) | **G0** | 536.9 ms | 3.41 s | 6.35× | 426 MB/s |
| `B-01` | rare literal ($BIG) | **G1** | 19.7 ms | 1.21 s | 61.38× | 1.18 GB/s |
| `B-01` | rare literal ($BIG) | **R0** | 171.5 ms | 3.15 s | 18.34× | 462 MB/s |
| `B-01` | rare literal ($BIG) | **U0** | 351.9 ms | 3.11 s | 8.84× | 467 MB/s |
| `T-01` | zero-match literal | **G0** | 3.75 s | 49.63 s | 13.25× | 33 MB/s |
| `T-01` | zero-match literal | **G1** | 150.9 ms | 1.96 s | 12.98× | 838 MB/s |
| `T-01` | zero-match literal | **R0** | 344.3 ms | 4.85 s | 14.08× | 338 MB/s |
| `T-01` | zero-match literal | **U0** | 461.1 ms | 4.75 s | 10.30× | 346 MB/s |
| `T-03` | moderate literal, line numbers | **G0** | 4.44 s | 50.13 s | 11.29× | 33 MB/s |
| `T-03` | moderate literal, line numbers | **G1** | 153.7 ms | 1.97 s | 12.81× | 833 MB/s |
| `T-03` | moderate literal, line numbers | **R0** | 365.0 ms | 4.48 s | 12.28× | 366 MB/s |
| `T-03` | moderate literal, line numbers | **U0** | 467.6 ms | 4.71 s | 10.08× | 348 MB/s |
| `T-06` | files-with-matches | **G0** | 3.87 s | 48.73 s | 12.60× | 34 MB/s |
| `T-06` | files-with-matches | **G1** | 152.2 ms | 1.94 s | 12.74× | 846 MB/s |
| `T-06` | files-with-matches | **R0** | 358.1 ms | 4.50 s | 12.58× | 364 MB/s |
| `T-06` | files-with-matches | **U0** | 461.2 ms | 4.70 s | 10.19× | 349 MB/s |

### Cold Cache Performance Insights:
- Under cold cache conditions where all file data is evicted from RAM, G1 achieves **833–846 MB/s tree scan throughput** (**1.96 s** on `T-01`, **1.94 s** on `T-06`), outperforming ripgrep at **338–366 MB/s** (**4.85 s** on `T-01`, **2.48× faster**) and stock GNU grep at **33–34 MB/s** (**49.63 s** on `T-01`, **25.3× faster**). `ugrep` reaches **346–349 MB/s** (**4.75 s** on `T-01`; G1 is **2.42× faster**).
- Concurrent worker prefetching keeps deep I/O request queues filled on underlying cloud storage, eliminating the latency stalls that plague single-threaded traversal.

---

## 7. Multi-Thread Scaling & Fairness Analysis (1 to 128 Threads)

### 7.1 Multi-Thread Scaling Sweep (1 to 128 Threads)

| Scenario | Tool | T=1 | T=2 | T=4 | T=8 | T=12 | T=16 | T=24 | T=32 | T=48 | T=64 | T=96 | T=128 | Peak Speedup vs G0 |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | **G1** | 235.7 ms (2.3×) | 113.1 ms (4.7×) | 73.1 ms (7.3×) | 33.7 ms (15.9×) | 27.3 ms (19.7×) | 24.2 ms (22.2×) | 21.4 ms (25.1×) | 19.3 ms (27.9×) | 19.8 ms (27.1×) | 20.5 ms (26.2×) | 23.2 ms (23.2×) | 21.9 ms (24.5×) | **27.86×** |
| `B-01` | **R0** | 176.1 ms (3.0×) | 174.8 ms (3.1×) | 173.6 ms (3.1×) | 173.6 ms (3.1×) | 174.6 ms (3.1×) | 176.6 ms (3.0×) | 174.3 ms (3.1×) | 174.9 ms (3.1×) | 176.7 ms (3.0×) | 174.4 ms (3.1×) | 174.8 ms (3.1×) | 176.0 ms (3.1×) | **3.09×** |
| `B-01` | **U0** | 350.4 ms (1.5×) | 353.5 ms (1.5×) | 351.5 ms (1.5×) | 358.9 ms (1.5×) | 353.6 ms (1.5×) | 354.0 ms (1.5×) | 352.6 ms (1.5×) | 353.8 ms (1.5×) | 354.4 ms (1.5×) | 353.3 ms (1.5×) | 353.9 ms (1.5×) | 355.7 ms (1.5×) | **1.53×** |
| `B-04` | **G1** | 260.1 ms (1.7×) | 120.6 ms (3.6×) | 77.4 ms (5.6×) | 36.8 ms (11.8×) | 29.3 ms (14.8×) | 26.1 ms (16.6×) | 21.8 ms (19.9×) | 21.3 ms (20.4×) | 23.5 ms (18.5×) | 22.2 ms (19.6×) | 22.3 ms (19.5×) | 23.5 ms (18.5×) | **20.45×** |
| `B-04` | **R0** | 173.6 ms (2.5×) | 172.6 ms (2.5×) | 175.0 ms (2.5×) | 173.5 ms (2.5×) | 177.3 ms (2.5×) | 176.9 ms (2.5×) | 176.6 ms (2.5×) | 174.0 ms (2.5×) | 176.3 ms (2.5×) | 173.9 ms (2.5×) | 175.5 ms (2.5×) | 175.1 ms (2.5×) | **2.52×** |
| `B-04` | **U0** | 429.9 ms (1.0×) | 430.3 ms (1.0×) | 433.1 ms (1.0×) | 433.5 ms (1.0×) | 432.5 ms (1.0×) | 431.6 ms (1.0×) | 434.0 ms (1.0×) | 431.5 ms (1.0×) | 431.4 ms (1.0×) | 433.1 ms (1.0×) | 431.9 ms (1.0×) | 433.6 ms (1.0×) | **1.01×** |
| `M-01` | **G1** | — | — | — | — | 329.3 ms (37.0×) | — | — | — | — | — | — | — | **37.01×** |
| `M-01` | **R0** | — | — | — | — | — | — | — | 280.3 ms (43.5×) | — | — | — | — | **43.48×** |
| `M-01` | **U0** | — | — | — | — | — | — | — | 939.2 ms (13.0×) | — | — | — | — | **12.98×** |
| `M-02` | **G1** | 4.07 s (3.5×) | 1.31 s (10.7×) | 810.3 ms (17.3×) | 460.7 ms (30.5×) | 341.6 ms (41.1×) | 262.9 ms (53.4×) | 199.6 ms (70.3×) | 172.1 ms (81.6×) | 214.4 ms (65.5×) | 228.3 ms (61.5×) | 243.6 ms (57.6×) | 250.0 ms (56.1×) | **81.55×** |
| `M-02` | **R0** | 7.79 s (1.8×) | 3.95 s (3.6×) | 2.03 s (6.9×) | 1.05 s (13.3×) | 717.3 ms (19.6×) | 563.7 ms (24.9×) | 400.4 ms (35.1×) | 333.4 ms (42.1×) | 294.5 ms (47.7×) | 339.7 ms (41.3×) | 370.1 ms (37.9×) | 415.2 ms (33.8×) | **47.66×** |
| `M-02` | **U0** | 6.13 s (2.3×) | 3.03 s (4.6×) | 1.59 s (8.8×) | 819.1 ms (17.1×) | 574.9 ms (24.4×) | 475.1 ms (29.5×) | 737.8 ms (19.0×) | 926.5 ms (15.2×) | 943.8 ms (14.9×) | 989.6 ms (14.2×) | 1.05 s (13.4×) | 1.10 s (12.7×) | **29.54×** |
| `M-03` | **G1** | — | — | — | — | 444.7 ms (38.5×) | — | — | — | — | — | — | — | **38.50×** |
| `M-03` | **R0** | — | — | — | — | — | — | — | 603.1 ms (28.4×) | — | — | — | — | **28.39×** |
| `M-03` | **U0** | — | — | — | — | — | — | — | 781.6 ms (21.9×) | — | — | — | — | **21.90×** |
| `M-04` | **G1** | — | — | — | — | 390.2 ms (244.9×) | — | — | — | — | — | — | — | **244.87×** |
| `M-04` | **R0** | — | — | — | — | — | — | — | 999.7 ms (95.6×) | — | — | — | — | **95.57×** |
| `M-04` | **U0** | — | — | — | — | — | — | — | 870.9 ms (109.7×) | — | — | — | — | **109.70×** |
| `M-05` | **G1** | — | — | — | — | 355.9 ms (39.7×) | — | — | — | — | — | — | — | **39.71×** |
| `M-05` | **R0** | — | — | — | — | — | — | — | 404.5 ms (34.9×) | — | — | — | — | **34.94×** |
| `M-05` | **U0** | — | — | — | — | — | — | — | 947.7 ms (14.9×) | — | — | — | — | **14.91×** |
| `M-06` | **G1** | — | — | — | — | 67.3 ms (132.7×) | — | — | — | — | — | — | — | **132.68×** |
| `M-06` | **R0** | — | — | — | — | — | — | — | 3.11 s (2.9×) | — | — | — | — | **2.87×** |
| `M-06` | **U0** | — | — | — | — | — | — | — | 1.47 s (6.1×) | — | — | — | — | **6.09×** |
| `T-01` | **G1** | 3.39 s (1.1×) | 1.09 s (3.4×) | 659.8 ms (5.7×) | 380.0 ms (9.9×) | 281.0 ms (13.3×) | 216.9 ms (17.3×) | 168.8 ms (22.2×) | 153.7 ms (24.4×) | 154.1 ms (24.3×) | 218.6 ms (17.1×) | 233.2 ms (16.1×) | 243.6 ms (15.4×) | **24.37×** |
| `T-01` | **R0** | 3.78 s (1.0×) | 2.03 s (1.8×) | 1.00 s (3.7×) | 506.6 ms (7.4×) | 344.3 ms (10.9×) | 267.1 ms (14.0×) | 191.2 ms (19.6×) | 157.5 ms (23.8×) | 140.1 ms (26.8×) | 193.7 ms (19.3×) | 212.3 ms (17.6×) | 210.6 ms (17.8×) | **26.75×** |
| `T-01` | **U0** | 4.67 s (0.8×) | 2.28 s (1.6×) | 1.21 s (3.1×) | 642.8 ms (5.8×) | 451.7 ms (8.3×) | 684.1 ms (5.5×) | 901.2 ms (4.2×) | 1.00 s (3.7×) | 972.1 ms (3.9×) | 1.00 s (3.7×) | 1.05 s (3.6×) | 1.08 s (3.5×) | **8.30×** |
| `T-02` | **G1** | — | — | — | — | 297.2 ms (13.4×) | — | — | — | — | — | — | — | **13.40×** |
| `T-02` | **R0** | — | — | — | — | — | — | — | 159.6 ms (25.0×) | — | — | — | — | **24.97×** |
| `T-02` | **U0** | — | — | — | — | — | — | — | 996.6 ms (4.0×) | — | — | — | — | **4.00×** |
| `T-03` | **G1** | — | — | — | — | 305.8 ms (14.5×) | — | — | — | — | — | — | — | **14.52×** |
| `T-03` | **R0** | — | — | — | — | — | — | — | 175.4 ms (25.3×) | — | — | — | — | **25.31×** |
| `T-03` | **U0** | — | — | — | — | — | — | — | 993.6 ms (4.5×) | — | — | — | — | **4.47×** |
| `T-04` | **G1** | 3.59 s (1.3×) | 1.16 s (4.0×) | 707.6 ms (6.6×) | 414.6 ms (11.3×) | 306.1 ms (15.2×) | 231.6 ms (20.2×) | 174.9 ms (26.7×) | 155.2 ms (30.1×) | 154.2 ms (30.3×) | 214.4 ms (21.8×) | 239.0 ms (19.5×) | 261.4 ms (17.9×) | **30.27×** |
| `T-04` | **R0** | 4.71 s (1.0×) | 2.59 s (1.8×) | 1.34 s (3.5×) | 690.6 ms (6.8×) | 514.2 ms (9.1×) | 433.8 ms (10.8×) | 482.1 ms (9.7×) | 512.6 ms (9.1×) | 535.8 ms (8.7×) | 584.6 ms (8.0×) | 621.6 ms (7.5×) | 647.4 ms (7.2×) | **10.76×** |
| `T-04` | **U0** | 5.08 s (0.9×) | 2.50 s (1.9×) | 1.35 s (3.5×) | 749.4 ms (6.2×) | 618.1 ms (7.6×) | 627.2 ms (7.4×) | 660.8 ms (7.1×) | 711.5 ms (6.6×) | 766.5 ms (6.1×) | 823.7 ms (5.7×) | 887.0 ms (5.3×) | 931.9 ms (5.0×) | **7.55×** |
| `T-05` | **G1** | — | — | — | — | 314.7 ms (15.4×) | — | — | — | — | — | — | — | **15.37×** |
| `T-05` | **R0** | — | — | — | — | — | — | — | 979.0 ms (4.9×) | — | — | — | — | **4.94×** |
| `T-05` | **U0** | — | — | — | — | — | — | — | 1.24 s (3.9×) | — | — | — | — | **3.89×** |
| `T-06` | **G1** | 3.43 s (1.1×) | 1.08 s (3.6×) | 674.3 ms (5.7×) | 384.9 ms (10.0×) | 272.6 ms (14.2×) | 219.5 ms (17.6×) | 166.1 ms (23.3×) | 159.0 ms (24.3×) | 208.5 ms (18.6×) | 220.6 ms (17.5×) | 232.9 ms (16.6×) | 246.8 ms (15.7×) | **24.32×** |
| `T-06` | **R0** | 3.84 s (1.0×) | 2.06 s (1.9×) | 1.02 s (3.8×) | 521.1 ms (7.4×) | 356.2 ms (10.9×) | 275.5 ms (14.0×) | 199.4 ms (19.4×) | 220.3 ms (17.6×) | 226.9 ms (17.0×) | 223.1 ms (17.3×) | 240.1 ms (16.1×) | 247.1 ms (15.7×) | **19.39×** |
| `T-06` | **U0** | 4.45 s (0.9×) | 2.26 s (1.7×) | 1.14 s (3.4×) | 606.9 ms (6.4×) | 466.9 ms (8.3×) | 699.7 ms (5.5×) | 890.2 ms (4.3×) | 990.4 ms (3.9×) | 966.0 ms (4.0×) | 993.4 ms (3.9×) | 1.05 s (3.7×) | 1.07 s (3.6×) | **8.28×** |
| `T-07` | **G1** | — | — | — | — | 268.1 ms (12.9×) | — | — | — | — | — | — | — | **12.90×** |
| `T-07` | **R0** | — | — | — | — | — | — | — | 289.1 ms (12.0×) | — | — | — | — | **11.96×** |
| `T-07` | **U0** | — | — | — | — | — | — | — | 801.4 ms (4.3×) | — | — | — | — | **4.31×** |
| `T-08` | **G1** | — | — | — | — | 275.0 ms (13.8×) | — | — | — | — | — | — | — | **13.76×** |
| `T-08` | **R0** | — | — | — | — | — | — | — | 1.44 s (2.6×) | — | — | — | — | **2.64×** |
| `T-08` | **U0** | — | — | — | — | — | — | — | 1.61 s (2.3×) | — | — | — | — | **2.34×** |
| `T-09` | **G1** | — | — | — | — | 285.6 ms (18.5×) | — | — | — | — | — | — | — | **18.54×** |
| `T-09` | **R0** | — | — | — | — | — | — | — | 928.0 ms (5.7×) | — | — | — | — | **5.71×** |
| `T-09` | **U0** | — | — | — | — | — | — | — | 1.06 s (5.0×) | — | — | — | — | **4.97×** |
| `T-10` | **G1** | 3.48 s (1.2×) | 1.10 s (3.8×) | 680.8 ms (6.2×) | 390.6 ms (10.7×) | 280.5 ms (14.9×) | 222.5 ms (18.8×) | 168.8 ms (24.8×) | 154.2 ms (27.2×) | 151.1 ms (27.7×) | 216.1 ms (19.4×) | 239.2 ms (17.5×) | 243.7 ms (17.2×) | **27.74×** |
| `T-10` | **R0** | 3.97 s (1.1×) | 2.10 s (2.0×) | 1.06 s (4.0×) | 555.7 ms (7.5×) | 373.0 ms (11.2×) | 291.4 ms (14.4×) | 212.8 ms (19.7×) | 192.4 ms (21.8×) | 248.0 ms (16.9×) | 252.4 ms (16.6×) | 269.1 ms (15.6×) | 274.3 ms (15.3×) | **21.78×** |
| `T-10` | **U0** | 4.77 s (0.9×) | 2.36 s (1.8×) | 1.21 s (3.5×) | 646.3 ms (6.5×) | 454.3 ms (9.2×) | 629.7 ms (6.7×) | 884.4 ms (4.7×) | 1000.0 ms (4.2×) | 956.8 ms (4.4×) | 1.01 s (4.2×) | 1.04 s (4.0×) | 1.09 s (3.9×) | **9.22×** |
| `T-11` | **G1** | — | — | — | — | 275.2 ms (13.8×) | — | — | — | — | — | — | — | **13.77×** |
| `T-11` | **R0** | — | — | — | — | — | — | — | 162.7 ms (23.3×) | — | — | — | — | **23.29×** |
| `T-11` | **U0** | — | — | — | — | — | — | — | 1.01 s (3.8×) | — | — | — | — | **3.76×** |
| `T-12` | **G1** | — | — | — | — | 306.5 ms (15.3×) | — | — | — | — | — | — | — | **15.31×** |
| `T-12` | **R0** | — | — | — | — | — | — | — | 682.1 ms (6.9×) | — | — | — | — | **6.88×** |
| `T-12` | **U0** | — | — | — | — | — | — | — | 786.9 ms (6.0×) | — | — | — | — | **5.96×** |
| `T-13` | **G1** | — | — | — | — | 278.7 ms (14.3×) | — | — | — | — | — | — | — | **14.28×** |
| `T-13` | **R0** | — | — | — | — | — | — | — | 175.8 ms (22.6×) | — | — | — | — | **22.63×** |
| `T-13` | **U0** | — | — | — | — | — | — | — | 997.6 ms (4.0×) | — | — | — | — | **3.99×** |
| `T-14` | **G1** | — | — | — | — | 277.7 ms (13.9×) | — | — | — | — | — | — | — | **13.88×** |
| `T-14` | **R0** | — | — | — | — | — | — | — | 162.6 ms (23.7×) | — | — | — | — | **23.71×** |
| `T-14` | **U0** | — | — | — | — | — | — | — | 998.5 ms (3.9×) | — | — | — | — | **3.86×** |
| `T-15` | **G1** | 3.91 s (1.1×) | 1.28 s (3.2×) | 775.1 ms (5.3×) | 439.6 ms (9.4×) | 315.7 ms (13.1×) | 248.9 ms (16.6×) | 184.3 ms (22.5×) | 162.7 ms (25.5×) | 150.2 ms (27.6×) | 215.3 ms (19.2×) | 237.3 ms (17.5×) | 249.3 ms (16.6×) | **27.59×** |
| `T-15` | **R0** | 4.16 s (1.0×) | 2.18 s (1.9×) | 1.08 s (3.9×) | 557.9 ms (7.4×) | 373.7 ms (11.1×) | 290.2 ms (14.3×) | 207.7 ms (20.0×) | 166.7 ms (24.9×) | 147.7 ms (28.1×) | 192.8 ms (21.5×) | 220.5 ms (18.8×) | 231.3 ms (17.9×) | **28.06×** |
| `T-15` | **U0** | 4.69 s (0.9×) | 2.35 s (1.8×) | 1.19 s (3.5×) | 630.6 ms (6.6×) | 454.9 ms (9.1×) | 671.4 ms (6.2×) | 889.1 ms (4.7×) | 992.3 ms (4.2×) | 970.7 ms (4.3×) | 1.01 s (4.1×) | 1.07 s (3.9×) | 1.09 s (3.8×) | **9.11×** |
| `T-16` | **G1** | 5.73 s (4.8×) | 1.87 s (14.8×) | 1.13 s (24.4×) | 641.0 ms (43.1×) | 456.0 ms (60.6×) | 359.9 ms (76.8×) | 262.9 ms (105.1×) | 224.0 ms (123.3×) | 199.1 ms (138.8×) | 227.4 ms (121.5×) | 249.9 ms (110.6×) | 274.2 ms (100.8×) | **138.80×** |
| `T-16` | **R0** | 29.36 s (0.9×) | 14.79 s (1.9×) | 7.42 s (3.7×) | 3.78 s (7.3×) | 2.50 s (11.0×) | 2.01 s (13.8×) | 1.44 s (19.2×) | 1.03 s (26.8×) | 1.15 s (24.0×) | 1.02 s (27.2×) | 967.8 ms (28.5×) | 1.01 s (27.4×) | **28.55×** |
| `T-16` | **U0** | 28.49 s (1.0×) | 15.68 s (1.8×) | 7.53 s (3.7×) | 3.83 s (7.2×) | 2.58 s (10.7×) | 1.95 s (14.2×) | 1.34 s (20.6×) | 1.02 s (27.0×) | 906.9 ms (30.5×) | 947.5 ms (29.2×) | 1.05 s (26.3×) | 1.09 s (25.3×) | **30.47×** |
| `T-17` | **G1** | — | — | — | — | 276.0 ms (19.4×) | — | — | — | — | — | — | — | **19.43×** |
| `T-17` | **R0** | — | — | — | — | — | — | — | 225.4 ms (23.8×) | — | — | — | — | **23.78×** |
| `T-17` | **U0** | — | — | — | — | — | — | — | 1.00 s (5.3×) | — | — | — | — | **5.35×** |
| `T-18` | **G1** | — | — | — | — | 303.5 ms (13.7×) | — | — | — | — | — | — | — | **13.75×** |
| `T-18` | **R0** | — | — | — | — | — | — | — | 194.1 ms (21.5×) | — | — | — | — | **21.49×** |
| `T-18` | **U0** | — | — | — | — | — | — | — | 954.7 ms (4.4×) | — | — | — | — | **4.37×** |
| `T-19` | **G1** | — | — | — | — | 275.8 ms (16.0×) | — | — | — | — | — | — | — | **16.03×** |
| `T-19` | **R0** | — | — | — | — | — | — | — | 161.8 ms (27.3×) | — | — | — | — | **27.33×** |
| `T-19` | **U0** | — | — | — | — | — | — | — | 980.6 ms (4.5×) | — | — | — | — | **4.51×** |
| `T-20` | **G1** | — | — | — | — | 277.0 ms (16.0×) | — | — | — | — | — | — | — | **16.04×** |
| `T-20` | **R0** | — | — | — | — | — | — | — | 193.0 ms (23.0×) | — | — | — | — | **23.02×** |
| `T-20` | **U0** | — | — | — | — | — | — | — | 961.0 ms (4.6×) | — | — | — | — | **4.62×** |
| `T-21` | **G1** | — | — | — | — | 345.1 ms (13.1×) | — | — | — | — | — | — | — | **13.08×** |
| `T-21` | **R0** | — | — | — | — | — | — | — | 1.44 s (3.1×) | — | — | — | — | **3.13×** |
| `T-21` | **U0** | — | — | — | — | — | — | — | 1.60 s (2.8×) | — | — | — | — | **2.82×** |
| `T-22` | **G1** | — | — | — | — | 133.7 ms (15.8×) | — | — | — | — | — | — | — | **15.81×** |
| `T-22` | **R0** | — | — | — | — | — | — | — | 94.7 ms (22.3×) | — | — | — | — | **22.34×** |
| `T-22` | **U0** | — | — | — | — | — | — | — | 552.9 ms (3.8×) | — | — | — | — | **3.83×** |
| `T-23` | **G1** | — | — | — | — | 15.6 ms (7.9×) | — | — | — | — | — | — | — | **7.91×** |
| `T-23` | **R0** | — | — | — | — | — | — | — | 24.9 ms (5.0×) | — | — | — | — | **4.96×** |
| `T-23` | **U0** | — | — | — | — | — | — | — | 31.2 ms (4.0×) | — | — | — | — | **3.95×** |
| `T-24` | **G1** | — | — | — | — | 273.6 ms (14.0×) | — | — | — | — | — | — | — | **14.02×** |
| `T-24` | **R0** | — | — | — | — | — | — | — | 175.8 ms (21.8×) | — | — | — | — | **21.82×** |
| `T-24` | **U0** | — | — | — | — | — | — | — | 978.8 ms (3.9×) | — | — | — | — | **3.92×** |
| `T-25a` | **G1** | — | — | — | — | 271.9 ms (13.6×) | — | — | — | — | — | — | — | **13.65×** |
| `T-25a` | **R0** | — | — | — | — | — | — | — | 158.0 ms (23.5×) | — | — | — | — | **23.50×** |
| `T-25a` | **U0** | — | — | — | — | — | — | — | 981.7 ms (3.8×) | — | — | — | — | **3.78×** |
| `T-25b` | **G1** | — | — | — | — | 2.6 ms (1.1×) | — | — | — | — | — | — | — | **1.07×** |
| `T-25b` | **R0** | — | — | — | — | — | — | — | 7.1 ms (0.4×) | — | — | — | — | **0.39×** |
| `T-25b` | **U0** | — | — | — | — | — | — | — | 11.5 ms (0.2×) | — | — | — | — | **0.24×** |
| `T-26` | **G1** | — | — | — | — | 283.5 ms (15.6×) | — | — | — | — | — | — | — | **15.65×** |
| `T-26` | **R0** | — | — | — | — | — | — | — | 176.3 ms (25.2×) | — | — | — | — | **25.16×** |
| `T-26` | **U0** | — | — | — | — | — | — | — | 985.5 ms (4.5×) | — | — | — | — | **4.50×** |
| `T-27` | **G1** | — | — | — | — | 125.2 ms (16.1×) | — | — | — | — | — | — | — | **16.07×** |
| `T-27` | **R0** | — | — | — | — | — | — | — | 86.9 ms (23.2×) | — | — | — | — | **23.15×** |
| `T-27` | **U0** | — | — | — | — | — | — | — | 394.2 ms (5.1×) | — | — | — | — | **5.10×** |
| `T-28` | **G1** | — | — | — | — | 16.1 ms (10.5×) | — | — | — | — | — | — | — | **10.47×** |
| `T-28` | **R0** | — | — | — | — | — | — | — | 15.1 ms (11.2×) | — | — | — | — | **11.16×** |
| `T-28` | **U0** | — | — | — | — | — | — | — | 40.4 ms (4.2×) | — | — | — | — | **4.17×** |
| `P-01` | **G1** | — | — | — | — | 567.5 ms (73.1×) | — | — | — | — | — | — | — | **73.09×** |
| `P-01` | **R0** | — | — | — | — | — | — | — | 338.9 ms (122.4×) | — | — | — | — | **122.38×** |
| `P-01` | **U0** | — | — | — | — | — | — | — | 616.9 ms (67.2×) | — | — | — | — | **67.25×** |
| `P-02` | **G1** | — | — | — | — | 334.9 ms (45.0×) | — | — | — | — | — | — | — | **45.04×** |
| `P-02` | **R0** | — | — | — | — | — | — | — | 692.7 ms (21.8×) | — | — | — | — | **21.77×** |
| `P-02` | **U0** | — | — | — | — | — | — | — | 924.7 ms (16.3×) | — | — | — | — | **16.31×** |
| `P-03` | **G1** | — | — | — | — | 276.7 ms (13.7×) | — | — | — | — | — | — | — | **13.75×** |
| `P-03` | **R0** | — | — | — | — | — | — | — | 160.4 ms (23.7×) | — | — | — | — | **23.72×** |
| `P-03` | **U0** | — | — | — | — | — | — | — | 1.27 s (3.0×) | — | — | — | — | **2.99×** |

### 7.2 Matched Thread Comparison Table: G1 (32t) vs G1 (12t) vs R0 (Default 12t) vs R0 (-j 32)

> Side-by-side comparison of G1 (default 32 threads and matched 12 threads) against both out-of-the-box R0 (default 12 threads) and thread-matched `R0 -j 32` (32 threads).

| Scenario | Cat | Description | G0 (1t) | G1 (12t) | R0 (12t) | G1 vs R0 (12t) | G1 (32t) | R0 (-j 32) | G1 vs R0 (32t) | G1 (32t) vs R0 (12t) |
| :--- | :-: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | B | rare literal ($BIG) | 536.9 ms | 27.3 ms | 171.5 ms | **6.28×** | 19.7 ms | 174.9 ms | **8.90×** | **8.72×** |
| `B-02` | B | count dense ($BIG) | 604.9 ms | — | 192.2 ms | **—** | 19.6 ms | 195.9 ms | **9.97×** | **9.78×** |
| `B-03` | B | moderate, line numbers ($BIG) | 934.2 ms | — | 246.2 ms | **—** | 40.6 ms | 247.9 ms | **6.10×** | **6.06×** |
| `B-04` | B | DFA regex ($BIG) | 434.7 ms | 29.3 ms | 173.8 ms | **5.93×** | 20.0 ms | 174.0 ms | **8.70×** | **8.69×** |
| `B-05` | B | case-insensitive dense ($BIG) | 1.97 s | — | 300.3 ms | **—** | 44.8 ms | 301.9 ms | **6.74×** | **6.71×** |
| `B-06` | B | word match ($BIG) | 370.8 ms | — | 180.6 ms | **—** | 23.0 ms | 183.4 ms | **7.97×** | **7.85×** |
| `B-07` | B | literal-free regex ($BIG) | 663.0 ms | — | 1.58 s | **—** | 30.1 ms | 1.57 s | **52.34×** | **52.40×** |
| `B-08` | B | PCRE2 lookaround ($BIG) | 21.40 s | — | 21.61 s | **—** | 164.4 ms | 21.63 s | **131.52×** | **131.41×** |
| `B-09` | B | context ($BIG) | 916.9 ms | — | 272.8 ms | **—** | 36.6 ms | 277.1 ms | **7.57×** | **7.46×** |
| `B-10` | B | only-matching ($BIG) | 767.3 ms | — | 188.0 ms | **—** | 41.9 ms | 194.3 ms | **4.64×** | **4.49×** |
| `B-11` | B | inverted count ($BIG) | 978.8 ms | — | 1.46 s | **—** | 46.0 ms | 1.46 s | **31.76×** | **31.80×** |
| `B-12` | B | stdin redirect (< $BIG) | 530.7 ms | — | 256.7 ms | **—** | 21.3 ms | 258.9 ms | **12.14×** | **12.04×** |
| `B-13` | B | FIFO pipe (cat $BIG \\|) | 1.14 s | — | 853.2 ms | **—** | 651.3 ms | 931.6 ms | **1.43×** | **1.31×** |
| `B-14` | B | real 24 MB header (big-24M.h) | 51.5 ms | — | 44.8 ms | **—** | 16.1 ms | 44.0 ms | **2.73×** | **2.78×** |
| `L-00` | L | harness floor (/bin/true) | 1.5 ms | — | 1.5 ms | **—** | 1.5 ms | 1.5 ms | **1.00×** | **1.00×** |
| `L-01` | L | one small file (init/main.c) | 2.6 ms | — | 3.6 ms | **—** | 2.6 ms | 3.6 ms | **1.41×** | **1.41×** |
| `L-02` | L | tiny tree (kernel/sched) | 7.0 ms | — | 7.1 ms | **—** | 6.0 ms | 10.3 ms | **1.71×** | **1.18×** |
| `L-03` | L | quiet first match in big file | 2.6 ms | — | 3.7 ms | **—** | 2.6 ms | 3.6 ms | **1.39×** | **1.40×** |
| `M-01` | M | 100 literals, tree | 12.19 s | 329.3 ms | 646.9 ms | **1.96×** | 160.5 ms | 280.3 ms | **1.75×** | **4.03×** |
| `M-02` | M | 1,000 literals, tree | 14.04 s | 341.6 ms | 719.0 ms | **2.11×** | 173.3 ms | 333.4 ms | **1.92×** | **4.15×** |
| `M-03` | M | 10,000 literals, tree | 17.12 s | 444.7 ms | 1.16 s | **2.60×** | 239.4 ms | 603.1 ms | **2.52×** | **4.83×** |
| `M-04` | M | 1,000 literals, -i | 95.54 s | 390.2 ms | 1.55 s | **3.98×** | 186.2 ms | 999.7 ms | **5.37×** | **8.33×** |
| `M-05` | M | 1,000 literals, -w | 14.13 s | 355.9 ms | 898.9 ms | **2.53×** | 173.5 ms | 404.5 ms | **2.33×** | **5.18×** |
| `M-06` | M | 1,000 literals, $BIG | 8.92 s | 67.3 ms | 3.11 s | **46.25×** | 41.2 ms | 3.11 s | **75.36×** | **75.50×** |
| `T-01` | T | zero-match literal | 3.75 s | 281.0 ms | 344.3 ms | **1.23×** | 150.9 ms | 157.5 ms | **1.04×** | **2.28×** |
| `T-02` | T | rare literal | 3.98 s | 297.2 ms | 352.3 ms | **1.19×** | 198.1 ms | 159.6 ms | **0.81×** | **1.78×** |
| `T-03` | T | moderate literal, line numbers | 4.44 s | 305.8 ms | 365.0 ms | **1.19×** | 153.7 ms | 175.4 ms | **1.14×** | **2.37×** |
| `T-04` | T | dense literal, line numbers | 4.67 s | 306.1 ms | 505.1 ms | **1.65×** | 158.1 ms | 512.6 ms | **3.24×** | **3.19×** |
| `T-05` | T | very dense literal | 4.84 s | 314.7 ms | 763.0 ms | **2.42×** | 287.3 ms | 979.0 ms | **3.41×** | **2.66×** |
| `T-06` | T | files-with-matches | 3.87 s | 272.6 ms | 358.1 ms | **1.31×** | 152.2 ms | 220.3 ms | **1.45×** | **2.35×** |
| `T-07` | T | files-without-match | 3.46 s | 268.1 ms | 420.0 ms | **1.57×** | 150.3 ms | 289.1 ms | **1.92×** | **2.79×** |
| `T-08` | T | count per file | 3.78 s | 275.0 ms | 1.07 s | **3.90×** | 151.2 ms | 1.44 s | **9.49×** | **7.10×** |
| `T-09` | T | case-insensitive literal | 5.30 s | 285.6 ms | 723.7 ms | **2.53×** | 161.1 ms | 928.0 ms | **5.76×** | **4.49×** |
| `T-10` | T | word match | 4.19 s | 280.5 ms | 373.2 ms | **1.33×** | 157.8 ms | 192.4 ms | **1.22×** | **2.36×** |
| `T-11` | T | DFA regex with must-literal | 3.79 s | 275.2 ms | 351.0 ms | **1.28×** | 155.1 ms | 162.7 ms | **1.05×** | **2.26×** |
| `T-12` | T | anchored dense regex | 4.69 s | 306.5 ms | 592.0 ms | **1.93×** | 166.0 ms | 682.1 ms | **4.11×** | **3.57×** |
| `T-13` | T | literal alternation + word boundary | 3.98 s | 278.7 ms | 369.7 ms | **1.33×** | 155.6 ms | 175.8 ms | **1.13×** | **2.38×** |
| `T-14` | T | optional group + alternation | 3.85 s | 277.7 ms | 349.5 ms | **1.26×** | 154.1 ms | 162.6 ms | **1.06×** | **2.27×** |
| `T-15` | T | literal-free regex | 4.14 s | 315.7 ms | 378.5 ms | **1.20×** | 163.5 ms | 166.7 ms | **1.02×** | **2.31×** |
| `T-16` | T | PCRE2 lookaround, dense | 27.63 s | 456.0 ms | 2.49 s | **5.46×** | 228.5 ms | 1.03 s | **4.51×** | **10.90×** |
| `T-17` | T | PCRE2 rare with literal | 5.36 s | 276.0 ms | 524.9 ms | **1.90×** | 155.7 ms | 225.4 ms | **1.45×** | **3.37×** |
| `T-18` | T | only-matching | 4.17 s | 303.5 ms | 382.2 ms | **1.26×** | 205.6 ms | 194.1 ms | **0.94×** | **1.86×** |
| `T-19` | T | context, rare | 4.42 s | 275.8 ms | 364.8 ms | **1.32×** | 156.2 ms | 161.8 ms | **1.04×** | **2.34×** |
| `T-20` | T | context, moderate | 4.44 s | 277.0 ms | 382.0 ms | **1.38×** | 151.8 ms | 193.0 ms | **1.27×** | **2.52×** |
| `T-21` | T | inverted count | 4.51 s | 345.1 ms | 1.09 s | **3.16×** | 178.3 ms | 1.44 s | **8.09×** | **6.12×** |
| `T-22` | T | glob include | 2.11 s | 133.7 ms | 174.1 ms | **1.30×** | 81.1 ms | 94.7 ms | **1.17×** | **2.15×** |
| `T-23` | T | glob + sub-scope | 123.4 ms | 15.6 ms | 24.9 ms | **1.59×** | 15.9 ms | 24.9 ms | **1.56×** | **1.56×** |
| `T-24` | T | max-count per file | 3.83 s | 273.6 ms | 360.6 ms | **1.32×** | 153.6 ms | 175.8 ms | **1.14×** | **2.35×** |
| `T-25a` | T | quiet, no match | 3.71 s | 271.9 ms | 344.7 ms | **1.27×** | 155.4 ms | 158.0 ms | **1.02×** | **2.22×** |
| `T-25b` | T | quiet, immediate match | 2.7 ms | 2.6 ms | 6.0 ms | **2.33×** | 2.7 ms | 7.1 ms | **2.60×** | **2.19×** |
| `T-26` | T | byte offsets | 4.44 s | 283.5 ms | 363.0 ms | **1.28×** | 152.0 ms | 176.3 ms | **1.16×** | **2.39×** |
| `T-27` | T | sub-scope drivers/ | 2.01 s | 125.2 ms | 165.1 ms | **1.32×** | 73.6 ms | 86.9 ms | **1.18×** | **2.24×** |
| `T-28` | T | sub-scope kernel mm fs | 168.2 ms | 16.1 ms | 19.1 ms | **1.19×** | 17.1 ms | 15.1 ms | **0.88×** | **1.12×** |
| `U-01` | U | out-of-the-box UX | 4.41 s | — | 390.7 ms | **—** | 151.3 ms | 207.0 ms | **1.37×** | **2.58×** |
| `U-02` | U | naive invocation (scans 6 GB .git) | 6.09 s | — | 390.5 ms | **—** | 236.5 ms | 209.5 ms | **0.89×** | **1.65×** |
| `X-01` | X | git ls-files \\| xargs | 4.22 s | — | 1.09 s | **—** | 479.4 ms | 960.1 ms | **2.00×** | **2.27×** |
| `P-01` | P | backreference (\b([a-z]{4,})_\1\b) | 41.48 s | 567.5 ms | 835.8 ms | **1.47×** | 260.4 ms | 338.9 ms | **1.30×** | **3.21×** |
| `P-02` | P | 1,000-way regex alternation | 15.08 s | 334.9 ms | 1.19 s | **3.56×** | 185.8 ms | 692.7 ms | **3.73×** | **6.42×** |
| `P-03` | P | dot-star heavy (.*_lock.*_unlock) | 3.80 s | 276.7 ms | 350.9 ms | **1.27×** | 154.2 ms | 160.4 ms | **1.04×** | **2.28×** |

### 7.2.1 Matched Thread Comparison Table: G1 (32t) vs G1 (12t) vs U0 (Default 12t) vs U0 (-J 32)

> Side-by-side comparison of G1 (default 32 threads and matched 12 threads) against both out-of-the-box U0 (default 12 threads) and thread-matched `U0 -J 32` (32 threads).

| Scenario | Cat | Description | G0 (1t) | G1 (12t) | U0 (12t) | G1 vs U0 (12t) | G1 (32t) | U0 (-J 32) | G1 vs U0 (32t) | G1 (32t) vs U0 (12t) |
| :--- | :-: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | B | rare literal ($BIG) | 536.9 ms | 27.3 ms | 351.9 ms | **12.88×** | 19.7 ms | 353.8 ms | **18.00×** | **17.90×** |
| `B-02` | B | count dense ($BIG) | 604.9 ms | — | 246.1 ms | **—** | 19.6 ms | 248.3 ms | **12.64×** | **12.53×** |
| `B-03` | B | moderate, line numbers ($BIG) | 934.2 ms | — | 433.6 ms | **—** | 40.6 ms | 433.2 ms | **10.66×** | **10.67×** |
| `B-04` | B | DFA regex ($BIG) | 434.7 ms | 29.3 ms | 429.0 ms | **14.64×** | 20.0 ms | 431.5 ms | **21.58×** | **21.46×** |
| `B-05` | B | case-insensitive dense ($BIG) | 1.97 s | — | 300.6 ms | **—** | 44.8 ms | 304.1 ms | **6.79×** | **6.71×** |
| `B-06` | B | word match ($BIG) | 370.8 ms | — | 239.2 ms | **—** | 23.0 ms | 240.8 ms | **10.47×** | **10.40×** |
| `B-07` | B | literal-free regex ($BIG) | 663.0 ms | — | 226.5 ms | **—** | 30.1 ms | 228.0 ms | **7.58×** | **7.53×** |
| `B-08` | B | PCRE2 lookaround ($BIG) | 21.40 s | — | 20.81 s | **—** | 164.4 ms | 20.85 s | **126.77×** | **126.53×** |
| `B-09` | B | context ($BIG) | 916.9 ms | — | 380.3 ms | **—** | 36.6 ms | 383.5 ms | **10.48×** | **10.39×** |
| `B-10` | B | only-matching ($BIG) | 767.3 ms | — | 245.6 ms | **—** | 41.9 ms | 246.6 ms | **5.89×** | **5.87×** |
| `B-11` | B | inverted count ($BIG) | 978.8 ms | — | 1.06 s | **—** | 46.0 ms | 1.06 s | **22.97×** | **22.95×** |
| `B-12` | B | stdin redirect (< $BIG) | 530.7 ms | — | 349.8 ms | **—** | 21.3 ms | 352.8 ms | **16.54×** | **16.40×** |
| `B-13` | B | FIFO pipe (cat $BIG \\|) | 1.14 s | — | 813.5 ms | **—** | 651.3 ms | 894.5 ms | **1.37×** | **1.25×** |
| `B-14` | B | real 24 MB header (big-24M.h) | 51.5 ms | — | 39.7 ms | **—** | 16.1 ms | 40.6 ms | **2.52×** | **2.46×** |
| `L-00` | L | harness floor (/bin/true) | 1.5 ms | — | 1.5 ms | **—** | 1.5 ms | 1.5 ms | **1.00×** | **1.00×** |
| `L-01` | L | one small file (init/main.c) | 2.6 ms | — | 4.7 ms | **—** | 2.6 ms | 4.7 ms | **1.83×** | **1.84×** |
| `L-02` | L | tiny tree (kernel/sched) | 7.0 ms | — | 8.1 ms | **—** | 6.0 ms | 11.3 ms | **1.89×** | **1.36×** |
| `L-03` | L | quiet first match in big file | 2.6 ms | — | 4.9 ms | **—** | 2.6 ms | 4.7 ms | **1.79×** | **1.86×** |
| `M-01` | M | 100 literals, tree | 12.19 s | 329.3 ms | 565.7 ms | **1.72×** | 160.5 ms | 939.2 ms | **5.85×** | **3.52×** |
| `M-02` | M | 1,000 literals, tree | 14.04 s | 341.6 ms | 570.7 ms | **1.67×** | 173.3 ms | 926.5 ms | **5.35×** | **3.29×** |
| `M-03` | M | 10,000 literals, tree | 17.12 s | 444.7 ms | 790.2 ms | **1.78×** | 239.4 ms | 781.6 ms | **3.26×** | **3.30×** |
| `M-04` | M | 1,000 literals, -i | 95.54 s | 390.2 ms | 663.6 ms | **1.70×** | 186.2 ms | 870.9 ms | **4.68×** | **3.56×** |
| `M-05` | M | 1,000 literals, -w | 14.13 s | 355.9 ms | 575.0 ms | **1.62×** | 173.5 ms | 947.7 ms | **5.46×** | **3.31×** |
| `M-06` | M | 1,000 literals, $BIG | 8.92 s | 67.3 ms | 1.46 s | **21.75×** | 41.2 ms | 1.47 s | **35.58×** | **35.51×** |
| `T-01` | T | zero-match literal | 3.75 s | 281.0 ms | 461.1 ms | **1.64×** | 150.9 ms | 1.00 s | **6.63×** | **3.06×** |
| `T-02` | T | rare literal | 3.98 s | 297.2 ms | 461.8 ms | **1.55×** | 198.1 ms | 996.6 ms | **5.03×** | **2.33×** |
| `T-03` | T | moderate literal, line numbers | 4.44 s | 305.8 ms | 467.6 ms | **1.53×** | 153.7 ms | 993.6 ms | **6.47×** | **3.04×** |
| `T-04` | T | dense literal, line numbers | 4.67 s | 306.1 ms | 627.4 ms | **2.05×** | 158.1 ms | 711.5 ms | **4.50×** | **3.97×** |
| `T-05` | T | very dense literal | 4.84 s | 314.7 ms | 1.07 s | **3.39×** | 287.3 ms | 1.24 s | **4.33×** | **3.72×** |
| `T-06` | T | files-with-matches | 3.87 s | 272.6 ms | 461.2 ms | **1.69×** | 152.2 ms | 990.4 ms | **6.51×** | **3.03×** |
| `T-07` | T | files-without-match | 3.46 s | 268.1 ms | 462.6 ms | **1.73×** | 150.3 ms | 801.4 ms | **5.33×** | **3.08×** |
| `T-08` | T | count per file | 3.78 s | 275.0 ms | 1.12 s | **4.09×** | 151.2 ms | 1.61 s | **10.68×** | **7.43×** |
| `T-09` | T | case-insensitive literal | 5.30 s | 285.6 ms | 784.7 ms | **2.75×** | 161.1 ms | 1.06 s | **6.61×** | **4.87×** |
| `T-10` | T | word match | 4.19 s | 280.5 ms | 461.4 ms | **1.65×** | 157.8 ms | 1000.0 ms | **6.34×** | **2.92×** |
| `T-11` | T | DFA regex with must-literal | 3.79 s | 275.2 ms | 460.4 ms | **1.67×** | 155.1 ms | 1.01 s | **6.50×** | **2.97×** |
| `T-12` | T | anchored dense regex | 4.69 s | 306.5 ms | 667.0 ms | **2.18×** | 166.0 ms | 786.9 ms | **4.74×** | **4.02×** |
| `T-13` | T | literal alternation + word boundary | 3.98 s | 278.7 ms | 453.0 ms | **1.63×** | 155.6 ms | 997.6 ms | **6.41×** | **2.91×** |
| `T-14` | T | optional group + alternation | 3.85 s | 277.7 ms | 453.3 ms | **1.63×** | 154.1 ms | 998.5 ms | **6.48×** | **2.94×** |
| `T-15` | T | literal-free regex | 4.14 s | 315.7 ms | 447.9 ms | **1.42×** | 163.5 ms | 992.3 ms | **6.07×** | **2.74×** |
| `T-16` | T | PCRE2 lookaround, dense | 27.63 s | 456.0 ms | 2.58 s | **5.66×** | 228.5 ms | 1.02 s | **4.48×** | **11.29×** |
| `T-17` | T | PCRE2 rare with literal | 5.36 s | 276.0 ms | 466.3 ms | **1.69×** | 155.7 ms | 1.00 s | **6.44×** | **3.00×** |
| `T-18` | T | only-matching | 4.17 s | 303.5 ms | 462.6 ms | **1.52×** | 205.6 ms | 954.7 ms | **4.64×** | **2.25×** |
| `T-19` | T | context, rare | 4.42 s | 275.8 ms | 460.8 ms | **1.67×** | 156.2 ms | 980.6 ms | **6.28×** | **2.95×** |
| `T-20` | T | context, moderate | 4.44 s | 277.0 ms | 477.0 ms | **1.72×** | 151.8 ms | 961.0 ms | **6.33×** | **3.14×** |
| `T-21` | T | inverted count | 4.51 s | 345.1 ms | 1.13 s | **3.27×** | 178.3 ms | 1.60 s | **8.96×** | **6.32×** |
| `T-22` | T | glob include | 2.11 s | 133.7 ms | 368.4 ms | **2.75×** | 81.1 ms | 552.9 ms | **6.82×** | **4.54×** |
| `T-23` | T | glob + sub-scope | 123.4 ms | 15.6 ms | 26.5 ms | **1.70×** | 15.9 ms | 31.2 ms | **1.96×** | **1.66×** |
| `T-24` | T | max-count per file | 3.83 s | 273.6 ms | 449.6 ms | **1.64×** | 153.6 ms | 978.8 ms | **6.37×** | **2.93×** |
| `T-25a` | T | quiet, no match | 3.71 s | 271.9 ms | 472.6 ms | **1.74×** | 155.4 ms | 981.7 ms | **6.32×** | **3.04×** |
| `T-25b` | T | quiet, immediate match | 2.7 ms | 2.6 ms | 12.5 ms | **4.90×** | 2.7 ms | 11.5 ms | **4.20×** | **4.59×** |
| `T-26` | T | byte offsets | 4.44 s | 283.5 ms | 467.2 ms | **1.65×** | 152.0 ms | 985.5 ms | **6.48×** | **3.07×** |
| `T-27` | T | sub-scope drivers/ | 2.01 s | 125.2 ms | 212.2 ms | **1.69×** | 73.6 ms | 394.2 ms | **5.36×** | **2.88×** |
| `T-28` | T | sub-scope kernel mm fs | 168.2 ms | 16.1 ms | 24.5 ms | **1.53×** | 17.1 ms | 40.4 ms | **2.36×** | **1.43×** |
| `U-01` | U | out-of-the-box UX | 4.41 s | — | 462.8 ms | **—** | 151.3 ms | 980.6 ms | **6.48×** | **3.06×** |
| `U-02` | U | naive invocation (scans 6 GB .git) | 6.09 s | — | 466.1 ms | **—** | 236.5 ms | 978.7 ms | **4.14×** | **1.97×** |
| `X-01` | X | git ls-files \\| xargs | 4.22 s | — | 1.53 s | **—** | 479.4 ms | 1.93 s | **4.03×** | **3.19×** |
| `P-01` | P | backreference (\b([a-z]{4,})_\1\b) | 41.48 s | 567.5 ms | 1.02 s | **1.80×** | 260.4 ms | 616.9 ms | **2.37×** | **3.91×** |
| `P-02` | P | 1,000-way regex alternation | 15.08 s | 334.9 ms | 573.4 ms | **1.71×** | 185.8 ms | 924.7 ms | **4.98×** | **3.09×** |
| `P-03` | P | dot-star heavy (.*_lock.*_unlock) | 3.80 s | 276.7 ms | 801.3 ms | **2.90×** | 154.2 ms | 1.27 s | **8.26×** | **5.20×** |

### 7.3 Why Ripgrep Defaults to 12 Threads vs Matched `-j 32` Behavior
1. **Default Heuristic Difference:**
   - Stock `ripgrep` caps its default directory-traversal thread count at `std::cmp::min(cores, 12)` (`12` worker threads on this 128-CPU server),
     whereas `G1` defaults to `MIN(count, 32)` (`32` worker threads).
2. **What Happens When Ripgrep Is Given `-j 32` Across All 59 Scenarios:**
   - **Single Large Files (`B-01..B-14`, `M-06`):** `-j 32` has **zero effect** on `ripgrep` (`171–302 ms`) because `ripgrep` does not parallelize
     within a single file. `G1` beats `ripgrep -j 32` on **14/14** `B-*` scenarios (**9.94× geomean**) and by **75.36×** on `M-06`.
   - **High-Output & Dense Match Tree Walks (`T-04`, `T-05`, `T-08`, `T-09`, `T-12`, `T-21`):** Increasing `ripgrep` from 12 to 32 threads
     **degrades** `ripgrep`'s runtime on 5 of these 6 scenarios (`T-05`: `763.0 ms` → `979.0 ms`; `T-08`: `1.07 s` → `1.44 s`;
     `T-09`: `723.7 ms` → `928.0 ms`; `T-12`: `592.0 ms` → `682.1 ms`; `T-21`: `1.09 s` → `1.44 s`) due to output lock contention.
     `G1` beats `ripgrep -j 32` by **3.24× to 9.49×**.
   - **Heavy Multi-Pattern & PCRE2 Tree Walks (`M-01..M-05`, `T-16`, `T-17`):** Even at matched 32 threads, `G1` beats `ripgrep -j 32`
     on all multi-pattern sets (`M-01`: **1.75×**, `M-02`: **1.92×**, `M-03`: **2.52×**, `M-04`: **5.37×**, `M-05`: **2.33×**),
     dense PCRE2 lookaround (`T-16`: **228.5 ms vs 1.03 s**, **4.51×**), and rare PCRE2 (`T-17`: **155.7 ms vs 225.4 ms**, **1.45×**).
   - **Matched 12 Threads (`G1 @ 12t` vs `R0 @ 12t`):** When both tools run on 12 threads on the 40 matched scenarios, G1 wins **all 40 out of 40** scenarios (**2.01× geomean**); on the 29 Category T tree scenarios, G1 wins **29 out of 29** scenarios (**1.62× category geomean**),
     showing that our directory walker and search algorithms are fundamentally faster per-core.
3. **ugrep's Default and `-J 32`:**
   - Stock `ugrep` defaults to `min(cores − 1, 12)` worker threads (`DEFAULT_MAX_JOBS`; `12` here). Its main thread walks the directory tree and hands
     files to the workers, which steal jobs from each other; a single file is searched by one thread whatever `-J` says.
   - Raising `ugrep` from 12 to 32 threads makes 27 of 29 tree scenarios slower and 2 faster (by more than 2%); on geometric mean, a tree scan takes 1.67× as long as at the default.
     The sweep shows where it turns: `T-01` takes 451.7 ms at 12 threads, 684.1 ms at 16 and 1.00 s at 32, while CPU time keeps rising; CPU-heavy `T-16` (PCRE2) keeps scaling to 906.9 ms at 48 threads.
   - **Matched 12 Threads (`G1 @ 12t` vs `U0 @ 12t`):** on 40 scenarios G1 has a **2.32× geomean** (40 Wins / 0 Ties / 0 Losses); in Category T, **2.02×** (29 Wins / 0 Ties / 0 Losses).

---

## 8. Synthetic Benchmark Suite & Edge-Case Conformance

In addition to real-world kernel tree benchmarks, the project maintains an automated 13-workload synthetic benchmark
suite (`benchmarks/run_benchmarks.py`), a 266-test automated edge-case, boundary, and multithreaded conformance harness, 151 Automake tests, and a continuous stress test harness (`benchmarks/stress_test.py`).

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
- **Pairwise Matrix, Fault Injection & Coverage Edge Cases (Checks 201–266 + 151 Automake Tests):** Exhaustive pairwise engine × input-path × modifier × cross-file transition matrix (`tests/parallel-matrix`), fault-injection boundary shrinking (`WALK_FD_CAP_MAX=2`, `MMAP_SCAN` match overflow, `mmap` failure fallback, short pipe reads, `EINTR`, `EPIPE`, `/dev/full` `ENOSPC` in `tests/parallel-fault-injection`), and full gcov line/branch coverage exercises (`tests/parallel-gcov-coverage`) pass 100% byte-for-byte.

---

## 9. Correctness Audit & Hypothesis Scorecard

### Multi-Tier Verification Gates
- **Gate A (Hard Correctness)**: `G1` output is byte-exact identical to `G0` on `stdout`, normalized `stderr`, and exit code.
  **Result: 59 / 59 Scenarios PASS (100%)**.
- **Gate B (Cross-Tool Equivalence)**: `R0` and `U0` output sets match `G0` after sorting.
  **Result: PASS on all matching scenarios**. T-07 and T-21 soft diffs stem from binary file message differences; T-09 soft diff
  stems from ripgrep stripping the UTF-8 BOM in `sparse.rst`. `U0` differs from `G0` on `T-09` (KNOWN-SEMANTIC), `T-19` (KNOWN-SEMANTIC), `T-20` (KNOWN-SEMANTIC), `T-21` (SOFT_DIFF), `T-26` (KNOWN-SEMANTIC); see `equivalence.md` in the results directory for the diff samples and their causes.
- **Gate C (Exit Code Consistency)**: `G1` and `G0` exit codes match identically across all tests (0 for match, 1 for no match, 123 for xargs).
- **Gate D (Thread Invariance Determinism)**: `G1` output across all thread counts {1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128}
  produces identical SHA-256 hashes.
  **Result: 9 / 9 Scenarios PASS (100%)**.

| # | Hypothesis | Target Criterion | Observed Outcome | Status |
| :-: | :--- | :--- | :--- | :---: |
| **H1** | Tree literal scan speedup | Ours ≥ 5× G0; within ±30% of R0 | T-01: 24.83× vs G0, 2.28× vs R0; T-04: 29.51× vs G0, 3.19× vs R0 | ✅ PASS |
| **H2** | $BIG intra-file parallelism | Ours ≥ 4× R0 (R0 searches 1 file with 1 thread) | B-01: 8.72× vs R0; B-03: 6.06× vs R0 | ✅ PASS |
| **H3** | Tiny invocation startup latency | Ours ≤ 1.5× G0; R0 ≈ 2–3× G0 | G0: 2.6 ms, G1: 2.6 ms (1.00×), R0: 3.6 ms | ✅ PASS |
| **H4** | Output-bound queries (~1.3M lines) | Ours never slower than G0 | T-05: G1 is 16.84× vs G0 | ✅ PASS |
| **H5** | UTF-8 vs C locale sensitivity | Both greps slower in UTF-8 on -i; R0 unaffected | T-09 (-i): G0 1.19× slower in UTF-8, G1 1.02×; R0 0.98× (unaffected) | ⚠️ CHECK |
| **H6** | Cold page cache | All tools bounded by PD bandwidth (within 20–35%) | T-01: G1 1.96 s (838 MB/s), R0 4.85 s (338 MB/s), G0 49.63 s | ⚠️ DIVERGENT |
| **H7** | 1,000-literal multi-pattern (M-02) | Ours within 2× of R0 | M-02: 80.98× vs G0, 4.15× vs R0 | ✅ PASS |
| **H8** | PCRE2 lookaround (T-16, B-08) | Ours ≫ G0 (literal pre-filter + threads) and ≥ R0 | T-16: 120.91× vs G0, 10.90× vs R0; B-08: 130.13× vs G0 | ✅ PASS |
| **H9** | Scaling past 16 threads | Gains throughput past 16 threads | B-01: 1.18× (16→64 th); 1..128 thread sweep across 128 logical CPUs (2 NUMA nodes) | ✅ PASS |

---

*Report generated automatically from primary benchmark artifacts.*
