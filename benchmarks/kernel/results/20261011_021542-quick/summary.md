# Linux Kernel-Tree 3-Way Benchmark Results

> Multi-threaded GNU `grep` fork (G1) vs Stock upstream GNU `grep` (G0) vs `ripgrep` 15.1.0 (R0) vs Distro GNU `grep` 3.12 (G2) vs `git grep` (X1).

## 1. System & Binary Provenance

- **Host**: AMD EPYC 7B13 Zen 3 (128 logical cores), 236 GiB RAM
- **Kernel**: `7.1.6`
- **THP**: `[always] madvise never`
- **Corpus**: Linux kernel clean working tree (96,034 files, 1.64 GB), `linux-all.c` (1.39 GB)
- **G0**: Upstream GNU grep baseline `3.12.41` (1 thread)
- **G1**: Our multi-threaded fork (`0f08fe1`, default 16 threads)
- **R0**: Stock ripgrep 15.1.0 (default 12 threads)
- **G2**: Distro GNU grep 3.12 (1 thread)
- **X1**: Git grep 2.56.0 (default 128 threads)

## 2. Headline Results (Core Scenario Matrix)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | G2 (ms) | X1 (ms) | G1 vs G0 | R0 vs G0 | G1 vs R0 | G1 CPU | R0 CPU | G2 CPU | X1 CPU | G1 RSS | R0 RSS | G2 RSS | X1 RSS | Gate A | Gate B |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| `B-01` | rare literal ($BIG) | 561.4 ms | 29.3 ms | 221.4 ms | 564.5 ms | 1.73 s | **19.18×** | 2.54× | **7.56×** | 0.31 | 0.22 | 0.56 | 1.74 | 1379M | 1390M | 27M | 1391M | **PASS** | PASS |
| `B-03` | moderate, line numbers ($BIG) | 960.0 ms | 58.4 ms | 295.9 ms | 953.4 ms | 1.58 s | **16.44×** | 3.24× | **5.07×** | 0.70 | 0.30 | 0.95 | 1.60 | 1380M | 1391M | 27M | 1392M | **PASS** | PASS |
| `B-04` | DFA regex ($BIG) | 466.5 ms | 29.0 ms | 224.4 ms | 466.0 ms | 87.68 s | **16.10×** | 2.08× | **7.74×** | 0.32 | 0.22 | 0.46 | 87.71 | 1379M | 1390M | 27M | 1391M | **PASS** | PASS |
| `B-13` | FIFO pipe (cat $BIG |) | 1.23 s | 640.5 ms | 1.10 s | 1.12 s | — | **1.92×** | 1.11× | **1.72×** | 0.84 | 0.79 | 1.10 | — | 27M | 27M | 27M | — | **PASS** | PASS |
| `L-00` | harness floor (/bin/true) | 1.5 ms | 1.5 ms | 1.5 ms | 1.5 ms | 1.5 ms | **0.99×** | 0.99× | **1.00×** | 0.00 | 0.00 | 0.00 | 0.00 | 27M | 27M | 27M | 27M | **PASS** | PASS |
| `L-01` | one small file (init/main.c) | 2.7 ms | 2.7 ms | 4.8 ms | 2.7 ms | 30.2 ms | **1.00×** | 0.57× | **1.76×** | 0.00 | 0.00 | 0.00 | 0.05 | 27M | 27M | 27M | 32M | **PASS** | PASS |
| `M-02` | 1,000 literals, tree | 14.10 s | 1.04 s | 715.4 ms | 14.09 s | DNF (> 600 s) | **13.56×** | 19.71× | **0.69×** | 15.90 | 8.31 | 14.08 | — | 199M | 70M | 27M | — | **PASS** | PASS |
| `T-01` | zero-match literal | 3.72 s | 537.3 ms | 346.3 ms | 3.74 s | 1.64 s | **6.93×** | 10.75× | **0.64×** | 5.21 | 4.06 | 3.74 | 11.89 | 174M | 27M | 27M | 212M | **PASS** | PASS |
| `T-03` | moderate literal, line numbers | 4.44 s | 552.6 ms | 373.3 ms | 4.41 s | 1.73 s | **8.03×** | 11.89× | **0.68×** | 5.57 | 4.39 | 4.41 | 11.85 | 168M | 27M | 27M | 232M | **PASS** | PASS |
| `T-04` | dense literal, line numbers | 4.62 s | 609.7 ms | 512.0 ms | 4.64 s | 2.20 s | **7.59×** | 9.03× | **0.84×** | 5.42 | 5.77 | 4.64 | 12.68 | 169M | 27M | 27M | 214M | **PASS** | PASS |
| `T-06` | files-with-matches | 3.86 s | 477.9 ms | 364.2 ms | 3.84 s | 1.63 s | **8.07×** | 10.59× | **0.76×** | 5.01 | 4.28 | 3.84 | 11.58 | 177M | 27M | 27M | 226M | **PASS** | PASS |
| `T-09` | case-insensitive literal | 5.18 s | 532.1 ms | 882.9 ms | 5.18 s | 2.67 s | **9.74×** | 5.87× | **1.66×** | 6.58 | 9.76 | 5.18 | 13.08 | 175M | 27M | 27M | 218M | **PASS** | KNOWN-SEMANTIC |
| `T-10` | word match | 4.22 s | 579.5 ms | 379.4 ms | 4.23 s | 1.66 s | **7.27×** | 11.11× | **0.65×** | 5.56 | 4.44 | 4.23 | 11.47 | 177M | 27M | 27M | 212M | **PASS** | PASS |
| `T-11` | DFA regex with must-literal | 3.83 s | 477.2 ms | 350.8 ms | 3.81 s | 8.38 s | **8.02×** | 10.91× | **0.74×** | 5.04 | 4.10 | 3.81 | 138.74 | 171M | 27M | 27M | 296M | **PASS** | PASS |
| `T-15` | literal-free regex | 4.19 s | 519.9 ms | 389.6 ms | 4.20 s | 3.10 s | **8.05×** | 10.74× | **0.75×** | 5.89 | 4.57 | 4.20 | 36.76 | 170M | 27M | 27M | 315M | **PASS** | PASS |
| `T-25b` | quiet, immediate match | 3.1 ms | 5.1 ms | 6.0 ms | 2.8 ms | 1.51 s | **0.61×** | 0.51× | **1.19×** | 0.01 | 0.01 | 0.00 | 11.06 | 27M | 27M | 27M | 230M | **PASS** | PASS |
| `U-01` | out-of-the-box UX | 4.42 s | 491.5 ms | 406.1 ms | 4.41 s | — | **8.99×** | 10.88× | **0.83×** | 5.20 | 4.73 | 4.41 | — | 168M | 27M | 27M | — | **PASS** | PASS |
| `U-02` | naive invocation (scans 6 GB .git) | 5.98 s | 895.9 ms | 399.3 ms* | 5.99 s | — | **6.67×** | 14.97× | **0.45×** | 5.90 | 4.66 | 5.99 | — | 193M | 27M | 27M | — | **PASS** | N/A |

## 3. Correctness & Equivalence Gates

- **Gate A (Hard)**: G1 byte-exact match against G0 on stdout, normalized stderr, and exit code.
- **Gate B (Soft)**: R0 / G2 / X1 equivalence against G0 (sorted match set).
- **Gate C**: Exit-code consistency.
- **Gate D**: G1 thread invariance (identical output across 1..128 threads).

Detailed equivalence records are available in [equivalence.md](./equivalence.md).

## 4. Per-Category Breakdowns

### Category T

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | G2 (ms) | X1 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G2 CPU | X1 CPU | G1 RSS | R0 RSS | G2 RSS | X1 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `T-01` | zero-match literal | 3.72 s | 537.3 ms | 346.3 ms | 3.74 s | 1.64 s | **6.93×** | **0.64×** | 5.21 | 4.06 | 3.74 | 11.89 | 174M | 27M | 27M | 212M | **PASS** |
| `T-03` | moderate literal, line numbers | 4.44 s | 552.6 ms | 373.3 ms | 4.41 s | 1.73 s | **8.03×** | **0.68×** | 5.57 | 4.39 | 4.41 | 11.85 | 168M | 27M | 27M | 232M | **PASS** |
| `T-04` | dense literal, line numbers | 4.62 s | 609.7 ms | 512.0 ms | 4.64 s | 2.20 s | **7.59×** | **0.84×** | 5.42 | 5.77 | 4.64 | 12.68 | 169M | 27M | 27M | 214M | **PASS** |
| `T-06` | files-with-matches | 3.86 s | 477.9 ms | 364.2 ms | 3.84 s | 1.63 s | **8.07×** | **0.76×** | 5.01 | 4.28 | 3.84 | 11.58 | 177M | 27M | 27M | 226M | **PASS** |
| `T-09` | case-insensitive literal | 5.18 s | 532.1 ms | 882.9 ms | 5.18 s | 2.67 s | **9.74×** | **1.66×** | 6.58 | 9.76 | 5.18 | 13.08 | 175M | 27M | 27M | 218M | **PASS** |
| `T-10` | word match | 4.22 s | 579.5 ms | 379.4 ms | 4.23 s | 1.66 s | **7.27×** | **0.65×** | 5.56 | 4.44 | 4.23 | 11.47 | 177M | 27M | 27M | 212M | **PASS** |
| `T-11` | DFA regex with must-literal | 3.83 s | 477.2 ms | 350.8 ms | 3.81 s | 8.38 s | **8.02×** | **0.74×** | 5.04 | 4.10 | 3.81 | 138.74 | 171M | 27M | 27M | 296M | **PASS** |
| `T-15` | literal-free regex | 4.19 s | 519.9 ms | 389.6 ms | 4.20 s | 3.10 s | **8.05×** | **0.75×** | 5.89 | 4.57 | 4.20 | 36.76 | 170M | 27M | 27M | 315M | **PASS** |
| `T-25b` | quiet, immediate match | 3.1 ms | 5.1 ms | 6.0 ms | 2.8 ms | 1.51 s | **0.61×** | **1.19×** | 0.01 | 0.01 | 0.00 | 11.06 | 27M | 27M | 27M | 230M | **PASS** |

### Category B

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | G2 (ms) | X1 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G2 CPU | X1 CPU | G1 RSS | R0 RSS | G2 RSS | X1 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `B-01` | rare literal ($BIG) | 561.4 ms | 29.3 ms | 221.4 ms | 564.5 ms | 1.73 s | **19.18×** | **7.56×** | 0.31 | 0.22 | 0.56 | 1.74 | 1379M | 1390M | 27M | 1391M | **PASS** |
| `B-03` | moderate, line numbers ($BIG) | 960.0 ms | 58.4 ms | 295.9 ms | 953.4 ms | 1.58 s | **16.44×** | **5.07×** | 0.70 | 0.30 | 0.95 | 1.60 | 1380M | 1391M | 27M | 1392M | **PASS** |
| `B-04` | DFA regex ($BIG) | 466.5 ms | 29.0 ms | 224.4 ms | 466.0 ms | 87.68 s | **16.10×** | **7.74×** | 0.32 | 0.22 | 0.46 | 87.71 | 1379M | 1390M | 27M | 1391M | **PASS** |
| `B-13` | FIFO pipe (cat $BIG |) | 1.23 s | 640.5 ms | 1.10 s | 1.12 s | — | **1.92×** | **1.72×** | 0.84 | 0.79 | 1.10 | — | 27M | 27M | 27M | — | **PASS** |

### Category M

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | G2 (ms) | X1 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G2 CPU | X1 CPU | G1 RSS | R0 RSS | G2 RSS | X1 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `M-02` | 1,000 literals, tree | 14.10 s | 1.04 s | 715.4 ms | 14.09 s | DNF (> 600 s) | **13.56×** | **0.69×** | 15.90 | 8.31 | 14.08 | — | 199M | 70M | 27M | — | **PASS** |

### Category L (Per-Invocation Latency)

> Iteration count: 100 measured rounds (5 warm-ups). Net latency = Raw latency − L-00 harness floor median (/bin/true).

#### Detailed Latency Percentiles (Raw vs Net)

| Scenario | Tool | N | Raw p50 | Raw p90 | Raw p99 | Net p50 | Net p90 | Net p99 | CPU (s) | RSS |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `L-00` | **G0** | 101 | 1.5 ms | 1.6 ms | 1.8 ms | 0.00 ms | 0.13 ms | 0.30 ms | 0.00 | 27M |
| `L-00` | **G1** | 101 | 1.5 ms | 1.7 ms | 1.8 ms | 0.00 ms | 0.19 ms | 0.32 ms | 0.00 | 27M |
| `L-00` | **R0** | 101 | 1.5 ms | 1.6 ms | 1.8 ms | 0.00 ms | 0.15 ms | 0.28 ms | 0.00 | 27M |
| `L-00` | **G2** | 101 | 1.5 ms | 1.7 ms | 2.8 ms | 0.00 ms | 0.21 ms | 1.4 ms | 0.00 | 27M |
| `L-00` | **X1** | 101 | 1.5 ms | 1.6 ms | 1.8 ms | 0.00 ms | 0.14 ms | 0.29 ms | 0.00 | 27M |
| `L-01` | **G0** | 102 | 2.7 ms | 2.9 ms | 3.0 ms | 1.3 ms | 1.4 ms | 1.6 ms | 0.00 | 27M |
| `L-01` | **G1** | 102 | 2.7 ms | 2.8 ms | 3.9 ms | 1.3 ms | 1.4 ms | 2.4 ms | 0.00 | 27M |
| `L-01` | **R0** | 102 | 4.8 ms | 5.0 ms | 5.1 ms | 3.3 ms | 3.5 ms | 3.6 ms | 0.00 | 27M |
| `L-01` | **G2** | 102 | 2.7 ms | 2.8 ms | 2.9 ms | 1.2 ms | 1.4 ms | 1.5 ms | 0.00 | 27M |
| `L-01` | **X1** | 102 | 30.2 ms | 34.6 ms | 38.0 ms | 28.7 ms | 33.1 ms | 36.5 ms | 0.05 | 32M |

#### Summary Comparison (Net Latency Median)

| Scenario | Description | G0 Net | G1 Net | R0 Net | G2 Net | X1 Net | G1 vs G0 | G1 vs R0 | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `L-00` | harness floor (/bin/true) | 0.00 ms | 0.00 ms | 0.00 ms | 0.00 ms | 0.00 ms | **—** | **—** | **PASS** |
| `L-01` | one small file (init/main.c) | 1.3 ms | 1.3 ms | 3.3 ms | 1.2 ms | 28.7 ms | **1.00×** | **2.63×** | **PASS** |

### Category U

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | G2 (ms) | X1 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G2 CPU | X1 CPU | G1 RSS | R0 RSS | G2 RSS | X1 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `U-01` | out-of-the-box UX | 4.42 s | 491.5 ms | 406.1 ms | 4.41 s | — | **8.99×** | **0.83×** | 5.20 | 4.73 | 4.41 | — | 168M | 27M | 27M | — | **PASS** |
| `U-02` | naive invocation (scans 6 GB .git) | 5.98 s | 895.9 ms | 399.3 ms* | 5.99 s | — | **6.67×** | **0.45×** | 5.90 | 4.66 | 5.99 | — | 193M | 27M | 27M | — | **PASS** |

> \* **Note on U-02 (`R0`)**: `rg` skips `.git` by default even without `-g '!.git'`; GNU `grep` requires explicit `--exclude-dir=.git` to avoid traversing `.git` pack files (hence scanning the 6 GB `.git` directory in U-02).

## 5. Thread Scaling Analysis

### Multi-Thread Scaling Sweep (1 → 128 Threads)

| Scenario | Tool | T=1 | T=4 | T=16 | T=32 | T=64 | T=128 | Peak Speedup vs G0 |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | **G1** | 307.9 ms (1.8×) | 123.8 ms (4.5×) | 37.9 ms (14.8×) | 31.5 ms (17.8×) | 28.1 ms (20.0×) | 42.3 ms (13.3×) | **19.99×** |
| `B-01` | **R0** | 242.8 ms (2.3×) | 251.6 ms (2.2×) | 246.0 ms (2.3×) | 222.5 ms (2.5×) | 251.7 ms (2.2×) | 247.5 ms (2.3×) | **2.52×** |
| `B-01` | **X1** | 1.72 s (0.3×) | 1.80 s (0.3×) | 1.81 s (0.3×) | 1.80 s (0.3×) | 1.81 s (0.3×) | 1.81 s (0.3×) | **0.33×** |
| `B-04` | **G1** | 322.6 ms (1.4×) | 132.0 ms (3.5×) | 41.3 ms (11.3×) | 35.5 ms (13.1×) | 31.1 ms (15.0×) | 45.7 ms (10.2×) | **14.98×** |
| `B-04` | **R0** | 247.4 ms (1.9×) | 251.5 ms (1.9×) | 239.8 ms (1.9×) | 251.5 ms (1.9×) | 252.0 ms (1.9×) | 256.2 ms (1.8×) | **1.95×** |
| `B-04` | **X1** | 87.49 s (0.0×) | 88.77 s (0.0×) | 87.77 s (0.0×) | 87.91 s (0.0×) | 88.17 s (0.0×) | 87.76 s (0.0×) | **0.01×** |
| `M-02` | **G1** | 13.98 s (1.0×) | 3.67 s (3.8×) | 1.06 s (13.3×) | 791.3 ms (17.8×) | 845.5 ms (16.7×) | 1.11 s (12.7×) | **17.82×** |
| `M-02` | **R0** | 7.67 s (1.8×) | 2.05 s (6.9×) | 553.4 ms (25.5×) | 336.2 ms (41.9×) | 295.3 ms (47.7×) | 407.3 ms (34.6×) | **47.75×** |
| `M-02` | **X1** | 600.00 s (0.0×) | 600.00 s (0.0×) | 600.00 s (0.0×) | 600.00 s (0.0×) | 600.00 s (0.0×) | 600.00 s (0.0×) | **0.02×** |
| `T-01` | **G1** | 3.57 s (1.0×) | 1.02 s (3.7×) | 478.3 ms (7.8×) | 610.0 ms (6.1×) | 908.8 ms (4.1×) | 1.17 s (3.2×) | **7.78×** |
| `T-01` | **R0** | 3.74 s (1.0×) | 998.3 ms (3.7×) | 261.3 ms (14.2×) | 153.5 ms (24.3×) | 181.1 ms (20.6×) | 242.8 ms (15.3×) | **24.26×** |
| `T-01` | **X1** | 3.42 s (1.1×) | 1.11 s (3.4×) | 1.06 s (3.5×) | 1.35 s (2.8×) | 1.39 s (2.7×) | 1.55 s (2.4×) | **3.52×** |
| `T-04` | **G1** | 3.83 s (1.2×) | 1.18 s (3.9×) | 632.1 ms (7.3×) | 819.8 ms (5.6×) | 1.15 s (4.0×) | 1.74 s (2.7×) | **7.32×** |
| `T-04` | **R0** | 4.73 s (1.0×) | 1.33 s (3.5×) | 424.1 ms (10.9×) | 490.2 ms (9.4×) | 539.7 ms (8.6×) | 620.9 ms (7.4×) | **10.91×** |
| `T-04` | **X1** | 3.86 s (1.2×) | 1.29 s (3.6×) | 1.39 s (3.3×) | 2.00 s (2.3×) | 2.00 s (2.3×) | 2.09 s (2.2×) | **3.59×** |
| `T-06` | **G1** | 3.57 s (1.1×) | 1.09 s (3.5×) | 470.8 ms (8.2×) | 722.0 ms (5.3×) | 893.9 ms (4.3×) | 1.19 s (3.2×) | **8.19×** |
| `T-06` | **R0** | 3.81 s (1.0×) | 1.04 s (3.7×) | 279.2 ms (13.8×) | 176.7 ms (21.8×) | 226.5 ms (17.0×) | 246.7 ms (15.6×) | **21.82×** |
| `T-06` | **X1** | 3.47 s (1.1×) | 1.18 s (3.3×) | 1.40 s (2.8×) | 1.48 s (2.6×) | 1.76 s (2.2×) | 1.73 s (2.2×) | **3.27×** |
| `T-10` | **G1** | 3.65 s (1.2×) | 1.05 s (4.0×) | 507.3 ms (8.3×) | 675.6 ms (6.2×) | 938.3 ms (4.5×) | 1.25 s (3.4×) | **8.31×** |
| `T-10` | **R0** | 3.96 s (1.1×) | 1.07 s (3.9×) | 287.5 ms (14.7×) | 193.4 ms (21.8×) | 197.8 ms (21.3×) | 269.5 ms (15.6×) | **21.80×** |
| `T-10` | **X1** | 3.48 s (1.2×) | 1.12 s (3.7×) | 1.16 s (3.6×) | 1.71 s (2.5×) | 1.67 s (2.5×) | 1.65 s (2.6×) | **3.75×** |
| `T-15` | **G1** | 4.19 s (1.0×) | 1.21 s (3.4×) | 488.4 ms (8.6×) | 594.5 ms (7.0×) | 923.2 ms (4.5×) | 1.15 s (3.6×) | **8.57×** |
| `T-15` | **R0** | 4.11 s (1.0×) | 1.12 s (3.8×) | 287.9 ms (14.5×) | 171.6 ms (24.4×) | 148.1 ms (28.3×) | 215.3 ms (19.4×) | **28.27×** |
| `T-15` | **X1** | 21.50 s (0.2×) | 5.74 s (0.7×) | 2.77 s (1.5×) | 2.93 s (1.4×) | 3.02 s (1.4×) | 3.09 s (1.4×) | **1.51×** |

## 6. Hypothesis Scorecard

### Hypothesis Scorecard (H1 – H9)

| # | Hypothesis | Target Criterion | Observed Outcome | Status |
| :-: | :--- | :--- | :--- | :---: |
| **H1** | Tree literal scan speedup | Ours ≥ 5× G0; within ±30% of R0 | T-01: 6.93× vs G0, 0.64× vs R0; T-04: 7.59× vs G0, 0.84× vs R0 | ⚠️ PARTIAL (G0: 6.9×–7.6× ≥ 5×; R0: 0.64×–0.84×, outside ±30% on T-01/T-03) |
| **H2** | $BIG intra-file parallelism | Ours ≥ 4× R0 (R0 searches 1 file with 1 thread) | B-01: 7.56× vs R0; B-03: 5.07× vs R0 | ✅ PASS |
| **H3** | Tiny invocation startup latency | Ours ≤ 1.5× G0; R0 ≈ 2–3× G0 | G0: 2.7 ms, G1: 2.7 ms (1.00×), R0: 4.8 ms | ✅ PASS |
| **H4** | Output-bound queries | Ours never slower than G0 | T-05 not run in --quick | ⏸️ PENDING FULL |
| **H5** | UTF-8 vs C locale sensitivity | Both greps slower in UTF-8 on -i; R0 unaffected | Evaluated in Phase 4 (C locale matrix) | ⏸️ PENDING FULL |
| **H6** | Cold page cache | All tools bounded by PD bandwidth (within 20%) | Evaluated in Phase 5 (cold cache matrix) | ⏸️ PENDING FULL |
| **H7** | 1,000-literal multi-pattern (M-02) | Ours within 2× of R0 | M-02: 13.56× vs G0, 0.69× vs R0 | ✅ PASS |
| **H8** | PCRE2 lookaround (T-16, B-08) | Ours ≫ G0 (literal pre-filter + threads) and ≥ R0 | Evaluated in Category T / B full matrix | ⏸️ PENDING FULL |
| **H9** | Scaling past 16 threads | Gains throughput past 16 threads | B-01 ($BIG): 1.35× (16→64 th); Tree regresses past 16 th (478 ms @16 → 909 ms @64) | ❌ FAIL (scaling bottleneck / regression on tree) |

