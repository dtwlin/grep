# Linux Kernel-Tree 3-Way Benchmark Results

> Multi-threaded GNU `grep` fork (G1) vs Stock upstream GNU `grep` (G0) vs `ripgrep` 15.1.0 (R0) vs Distro GNU `grep` 3.12 (G2) vs `git grep` (X1).

## 1. System & Binary Provenance

- **Host**: AMD EPYC 7B13 Zen 3 (128 logical cores), 236 GiB RAM
- **Kernel**: `7.1.6`
- **THP**: `[always] madvise never`
- **Corpus**: Linux kernel clean working tree (96,034 files, 1.64 GB), `linux-all.c` (1.39 GB)
- **Isolation**: All 128 logical CPUs (2 NUMA nodes, CPUs 0-127)
- **G0**: Upstream GNU grep baseline `3.12.41` (1 thread)
- **G1**: Our multi-threaded fork (`6e23f66`, default 33 threads)
- **R0**: Stock ripgrep 15.1.0 (default 13 threads)
- **G2**: Distro GNU grep 3.12 (1 thread)
- **X1**: Git grep 2.56.0 (default 129 threads)

## 2. Headline Results (Full Scenario Matrix)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | G1 vs G0 | R0 vs G0 | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A | Gate B |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| `B-01` | rare literal (`$BIG`) | 576.7 ms | 19.7 ms | 267.1 ms | **29.20×** | 2.16× | **13.53×** | 0.41 | 0.28 | 1382M | 1391M | **PASS** | PASS |
| `B-02` | count dense (`$BIG`) | 651.2 ms | 20.7 ms | 285.0 ms | **31.48×** | 2.29× | **13.78×** | 0.43 | 0.28 | 1382M | 1391M | **PASS** | PASS |
| `B-03` | moderate, line numbers (`$BIG`) | 981.0 ms | 40.0 ms | 336.7 ms | **24.54×** | 2.91× | **8.42×** | 0.94 | 0.33 | 1382M | 1391M | **PASS** | PASS |
| `B-04` | DFA regex (`$BIG`) | 478.7 ms | 22.5 ms | 256.0 ms | **21.28×** | 1.87× | **11.38×** | 0.45 | 0.26 | 1379M | 1391M | **PASS** | PASS |
| `B-05` | case-insensitive dense (`$BIG`) | 2.04 s | 185.3 ms | 394.4 ms | **10.98×** | 5.16× | **2.13×** | 2.46 | 0.39 | 1387M | 1391M | **PASS** | PASS |
| `B-06` | word match (`$BIG`) | 411.0 ms | 22.4 ms | 271.4 ms | **18.35×** | 1.51× | **12.12×** | 0.43 | 0.27 | 1379M | 1391M | **PASS** | PASS |
| `B-07` | literal-free regex (`$BIG`) | 704.5 ms | 39.5 ms | 1.68 s | **17.83×** | 0.42× | **42.51×** | 0.83 | 1.68 | 1373M | 1391M | **PASS** | PASS |
| `B-08` | PCRE2 lookaround (`$BIG`) | 21.44 s | 924.6 ms | 21.81 s | **23.19×** | 0.98× | **23.59×** | 22.24 | 21.80 | 1380M | 1391M | **PASS** | PASS |
| `B-09` | context (`$BIG`) | 970.7 ms | 31.0 ms | 340.8 ms | **31.29×** | 2.85× | **10.99×** | 0.74 | 0.34 | 1379M | 1391M | **PASS** | PASS |
| `B-10` | only-matching (`$BIG`) | 820.1 ms | 46.0 ms | 282.9 ms | **17.83×** | 2.90× | **6.15×** | 0.69 | 0.28 | 1374M | 1391M | **PASS** | PASS |
| `B-11` | inverted count (`$BIG`) | 1.02 s | 48.4 ms | 1.55 s | **21.15×** | 0.66× | **32.10×** | 1.06 | 1.55 | 1378M | 1391M | **PASS** | PASS |
| `B-12` | stdin redirect (`< $BIG`) | 574.1 ms | 18.6 ms | 303.7 ms | **30.82×** | 1.89× | **16.30×** | 0.41 | 0.30 | 1382M | 37M | **PASS** | PASS |
| `B-13` | FIFO pipe (cat `$BIG` \\|) | 1.12 s | 604.6 ms | 885.9 ms | **1.85×** | 1.26× | **1.47×** | 0.80 | 0.59 | 37M | 37M | **PASS** | PASS |
| `B-14` | real 24 MB header (big-24M.h) | 49.2 ms | 17.2 ms | 43.7 ms | **2.86×** | 1.13× | **2.54×** | 0.07 | 0.04 | 37M | 37M | **PASS** | PASS |
| `L-00` | harness floor (/bin/true) | 1.5 ms | 1.5 ms | 1.5 ms | **1.01×** | 1.01× | **1.00×** | 0.00 | 0.00 | 37M | 37M | **PASS** | PASS |
| `L-01` | one small file (init/main.c) | 2.6 ms | 2.6 ms | 3.7 ms | **1.00×** | 0.70× | **1.41×** | 0.00 | 0.00 | 37M | 37M | **PASS** | PASS |
| `L-02` | tiny tree (kernel/sched) | 7.0 ms | 7.0 ms | 7.1 ms | **1.00×** | 0.99× | **1.01×** | 0.02 | 0.01 | 37M | 37M | **PASS** | PASS |
| `L-03` | quiet first match in big file | 2.7 ms | 3.9 ms | 4.9 ms | **0.71×** | 0.56× | **1.27×** | 0.01 | 0.00 | 37M | 37M | **PASS** | PASS |
| `M-01` | 100 literals, tree | 12.18 s | 276.1 ms | 648.9 ms | **44.10×** | 18.77× | **2.35×** | 5.32 | 7.66 | 261M | 37M | **PASS** | PASS |
| `M-02` | 1,000 literals, tree | 14.00 s | 284.8 ms | 709.7 ms | **49.17×** | 19.73× | **2.49×** | 5.69 | 8.30 | 271M | 67M | **PASS** | PASS |
| `M-03` | 10,000 literals, tree | 17.08 s | 351.2 ms | 1.16 s | **48.64×** | 14.68× | **3.31×** | 6.52 | 13.52 | 332M | 37M | **PASS** | PASS |
| `M-04` | 1,000 literals, -i | 94.65 s | 4.42 s | 1.56 s | **21.40×** | 60.62× | **0.35×** | 112.28 | 18.14 | 698M | 147M | **PASS** | PASS |
| `M-05` | 1,000 literals, -w | 14.12 s | 293.3 ms | 897.4 ms | **48.15×** | 15.74× | **3.06×** | 5.69 | 10.50 | 273M | 72M | **PASS** | PASS |
| `M-06` | 1,000 literals, `$BIG` | 8.97 s | 43.5 ms | 3.22 s | **206.01×** | 2.78× | **74.04×** | 0.79 | 3.23 | 1386M | 1404M | **PASS** | PASS |
| `T-01` | zero-match literal | 3.72 s | 274.4 ms | 351.4 ms | **13.56×** | 10.59× | **1.28×** | 5.27 | 4.11 | 263M | 37M | **PASS** | PASS |
| `T-02` | rare literal | 3.92 s | 277.7 ms | 350.3 ms | **14.11×** | 11.18× | **1.26×** | 5.07 | 4.11 | 266M | 37M | **PASS** | PASS |
| `T-03` | moderate literal, line numbers | 4.41 s | 261.0 ms | 368.1 ms | **16.88×** | 11.97× | **1.41×** | 4.96 | 4.31 | 263M | 37M | **PASS** | PASS |
| `T-04` | dense literal, line numbers | 4.60 s | 306.0 ms | 517.3 ms | **15.03×** | 8.89× | **1.69×** | 5.21 | 5.82 | 279M | 37M | **PASS** | PASS |
| `T-05` | very dense literal | 4.75 s | 353.6 ms | 765.6 ms | **13.43×** | 6.20× | **2.16×** | 5.68 | 7.35 | 336M | 37M | **PASS** | PASS |
| `T-06` | files-with-matches | 3.81 s | 274.5 ms | 357.1 ms | **13.89×** | 10.68× | **1.30×** | 4.89 | 4.19 | 260M | 37M | **PASS** | PASS |
| `T-07` | files-without-match | 3.45 s | 275.4 ms | 426.2 ms | **12.53×** | 8.10× | **1.55×** | 4.91 | 4.97 | 239M | 37M | **PASS** | SOFT_DIFF |
| `T-08` | count per file | 3.76 s | 267.7 ms | 1.09 s | **14.04×** | 3.46× | **4.06×** | 5.05 | 10.83 | 264M | 37M | **PASS** | PASS |
| `T-09` | case-insensitive literal | 5.20 s | 298.7 ms | 751.9 ms | **17.42×** | 6.92× | **2.52×** | 6.82 | 8.05 | 264M | 37M | **PASS** | KNOWN-SEMANTIC |
| `T-10` | word match | 4.26 s | 276.5 ms | 372.1 ms | **15.42×** | 11.46× | **1.35×** | 5.03 | 4.36 | 268M | 37M | **PASS** | PASS |
| `T-11` | DFA regex with must-literal | 3.80 s | 268.2 ms | 347.1 ms | **14.18×** | 10.96× | **1.29×** | 4.84 | 4.08 | 268M | 37M | **PASS** | PASS |
| `T-12` | anchored dense regex | 4.71 s | 293.1 ms | 608.2 ms | **16.08×** | 7.75× | **2.08×** | 5.45 | 6.65 | 273M | 37M | **PASS** | PASS |
| `T-13` | literal alternation + word boundary | 3.96 s | 277.5 ms | 368.3 ms | **14.28×** | 10.75× | **1.33×** | 5.07 | 4.31 | 272M | 37M | **PASS** | PASS |
| `T-14` | optional group + alternation | 3.86 s | 275.7 ms | 346.1 ms | **13.99×** | 11.14× | **1.26×** | 4.98 | 4.05 | 266M | 37M | **PASS** | PASS |
| `T-15` | literal-free regex | 4.16 s | 283.5 ms | 377.0 ms | **14.67×** | 11.03× | **1.33×** | 5.61 | 4.41 | 268M | 37M | **PASS** | PASS |
| `T-16` | PCRE2 lookaround, dense | 27.66 s | 990.0 ms | 2.51 s | **27.94×** | 11.04× | **2.53×** | 31.44 | 29.80 | 291M | 37M | **PASS** | PASS |
| `T-17` | PCRE2 rare with literal | 5.34 s | 300.9 ms | 521.0 ms | **17.75×** | 10.25× | **1.73×** | 6.60 | 6.14 | 284M | 37M | **PASS** | PASS |
| `T-18` | only-matching | 4.21 s | 282.6 ms | 374.4 ms | **14.90×** | 11.24× | **1.32×** | 5.35 | 4.40 | 272M | 37M | **PASS** | PASS |
| `T-19` | context, rare | 4.38 s | 269.6 ms | 365.6 ms | **16.24×** | 11.97× | **1.36×** | 4.98 | 4.28 | 260M | 37M | **PASS** | PASS |
| `T-20` | context, moderate | 4.41 s | 274.9 ms | 386.4 ms | **16.05×** | 11.42× | **1.41×** | 5.12 | 4.51 | 264M | 37M | **PASS** | PASS |
| `T-21` | inverted count | 4.50 s | 288.1 ms | 1.11 s | **15.61×** | 4.03× | **3.87×** | 6.09 | 12.02 | 262M | 37M | **PASS** | SOFT_DIFF |
| `T-22` | glob include | 2.11 s | 191.5 ms | 173.0 ms | **11.03×** | 12.21× | **0.90×** | 2.13 | 1.97 | 37M | 37M | **PASS** | PASS |
| `T-23` | glob + sub-scope | 126.1 ms | 17.1 ms | 25.6 ms | **7.36×** | 4.93× | **1.49×** | 0.17 | 0.21 | 37M | 37M | **PASS** | PASS |
| `T-24` | max-count per file | 3.83 s | 262.7 ms | 358.1 ms | **14.60×** | 10.71× | **1.36×** | 4.84 | 4.20 | 264M | 37M | **PASS** | PASS |
| `T-25a` | quiet, no match | 3.71 s | 261.0 ms | 342.9 ms | **14.20×** | 10.81× | **1.31×** | 4.76 | 4.01 | 261M | 37M | **PASS** | PASS |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | 6.0 ms | **1.01×** | 0.45× | **2.23×** | 0.00 | 0.01 | 37M | 37M | **PASS** | PASS |
| `T-26` | byte offsets | 4.38 s | 262.5 ms | 362.9 ms | **16.71×** | 12.08× | **1.38×** | 4.87 | 4.25 | 262M | 37M | **PASS** | PASS |
| `T-27` | sub-scope drivers/ | 2.02 s | 152.4 ms | 166.4 ms | **13.26×** | 12.15× | **1.09×** | 2.26 | 1.90 | 262M | 37M | **PASS** | PASS |
| `T-28` | sub-scope kernel mm fs | 169.5 ms | 21.5 ms | 20.1 ms | **7.89×** | 8.45× | **0.93×** | 0.19 | 0.16 | 37M | 37M | **PASS** | PASS |
| `U-01` | out-of-the-box UX | 4.39 s | 260.9 ms | 390.6 ms | **16.82×** | 11.23× | **1.50×** | 4.93 | 4.58 | 263M | 37M | **PASS** | PASS |
| `U-02` | naive invocation (scans 6 GB .git) | 6.00 s | 705.8 ms | 389.1 ms* | **8.50×** | 15.41× | **0.55×** | 5.71 | 4.56 | 294M | 37M | **PASS** | N/A |
| `X-01` | git ls-files \\| xargs | 4.24 s | 1.01 s | 1.13 s | **4.19×** | 3.74× | **1.12×** | 8.22 | 5.68 | 62M | 37M | **PASS** | PASS |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.64 s | 1.51 s | 833.5 ms | **27.52×** | 49.96× | **0.55×** | 47.77 | 9.90 | 254M | 37M | **PASS** | PASS |
| `P-02` | 1,000-way regex alternation | 15.01 s | 1.01 s | 1.18 s | **14.81×** | 12.69× | **1.17×** | 15.09 | 13.79 | 456M | 136M | **PASS** | PASS |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.78 s | 278.9 ms | 346.1 ms | **13.55×** | 10.92× | **1.24×** | 4.90 | 4.04 | 265M | 37M | **PASS** | PASS |

## 3. Correctness & Equivalence Gates

- **Gate A (Hard)**: G1 byte-exact match against G0 on stdout, normalized stderr, and exit code.
- **Gate B (Soft)**: R0 / G2 / X1 equivalence against G0 (sorted match set).
- **Gate C**: Exit-code consistency.
- **Gate D**: G1 thread invariance (identical output across 1..64 threads).

Detailed equivalence records are available in [equivalence.md](./equivalence.md).

## 4. Per-Category Breakdowns

### Category T — Tree Traversal (96k files, 1.64 GB)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `T-01` | zero-match literal | 3.72 s | 274.4 ms | 351.4 ms | **13.56×** | **1.28×** | 5.27 | 4.11 | 263M | 37M | **PASS** |
| `T-02` | rare literal | 3.92 s | 277.7 ms | 350.3 ms | **14.11×** | **1.26×** | 5.07 | 4.11 | 266M | 37M | **PASS** |
| `T-03` | moderate literal, line numbers | 4.41 s | 261.0 ms | 368.1 ms | **16.88×** | **1.41×** | 4.96 | 4.31 | 263M | 37M | **PASS** |
| `T-04` | dense literal, line numbers | 4.60 s | 306.0 ms | 517.3 ms | **15.03×** | **1.69×** | 5.21 | 5.82 | 279M | 37M | **PASS** |
| `T-05` | very dense literal | 4.75 s | 353.6 ms | 765.6 ms | **13.43×** | **2.16×** | 5.68 | 7.35 | 336M | 37M | **PASS** |
| `T-06` | files-with-matches | 3.81 s | 274.5 ms | 357.1 ms | **13.89×** | **1.30×** | 4.89 | 4.19 | 260M | 37M | **PASS** |
| `T-07` | files-without-match | 3.45 s | 275.4 ms | 426.2 ms | **12.53×** | **1.55×** | 4.91 | 4.97 | 239M | 37M | **PASS** |
| `T-08` | count per file | 3.76 s | 267.7 ms | 1.09 s | **14.04×** | **4.06×** | 5.05 | 10.83 | 264M | 37M | **PASS** |
| `T-09` | case-insensitive literal | 5.20 s | 298.7 ms | 751.9 ms | **17.42×** | **2.52×** | 6.82 | 8.05 | 264M | 37M | **PASS** |
| `T-10` | word match | 4.26 s | 276.5 ms | 372.1 ms | **15.42×** | **1.35×** | 5.03 | 4.36 | 268M | 37M | **PASS** |
| `T-11` | DFA regex with must-literal | 3.80 s | 268.2 ms | 347.1 ms | **14.18×** | **1.29×** | 4.84 | 4.08 | 268M | 37M | **PASS** |
| `T-12` | anchored dense regex | 4.71 s | 293.1 ms | 608.2 ms | **16.08×** | **2.08×** | 5.45 | 6.65 | 273M | 37M | **PASS** |
| `T-13` | literal alternation + word boundary | 3.96 s | 277.5 ms | 368.3 ms | **14.28×** | **1.33×** | 5.07 | 4.31 | 272M | 37M | **PASS** |
| `T-14` | optional group + alternation | 3.86 s | 275.7 ms | 346.1 ms | **13.99×** | **1.26×** | 4.98 | 4.05 | 266M | 37M | **PASS** |
| `T-15` | literal-free regex | 4.16 s | 283.5 ms | 377.0 ms | **14.67×** | **1.33×** | 5.61 | 4.41 | 268M | 37M | **PASS** |
| `T-16` | PCRE2 lookaround, dense | 27.66 s | 990.0 ms | 2.51 s | **27.94×** | **2.53×** | 31.44 | 29.80 | 291M | 37M | **PASS** |
| `T-17` | PCRE2 rare with literal | 5.34 s | 300.9 ms | 521.0 ms | **17.75×** | **1.73×** | 6.60 | 6.14 | 284M | 37M | **PASS** |
| `T-18` | only-matching | 4.21 s | 282.6 ms | 374.4 ms | **14.90×** | **1.32×** | 5.35 | 4.40 | 272M | 37M | **PASS** |
| `T-19` | context, rare | 4.38 s | 269.6 ms | 365.6 ms | **16.24×** | **1.36×** | 4.98 | 4.28 | 260M | 37M | **PASS** |
| `T-20` | context, moderate | 4.41 s | 274.9 ms | 386.4 ms | **16.05×** | **1.41×** | 5.12 | 4.51 | 264M | 37M | **PASS** |
| `T-21` | inverted count | 4.50 s | 288.1 ms | 1.11 s | **15.61×** | **3.87×** | 6.09 | 12.02 | 262M | 37M | **PASS** |
| `T-22` | glob include | 2.11 s | 191.5 ms | 173.0 ms | **11.03×** | **0.90×** | 2.13 | 1.97 | 37M | 37M | **PASS** |
| `T-23` | glob + sub-scope | 126.1 ms | 17.1 ms | 25.6 ms | **7.36×** | **1.49×** | 0.17 | 0.21 | 37M | 37M | **PASS** |
| `T-24` | max-count per file | 3.83 s | 262.7 ms | 358.1 ms | **14.60×** | **1.36×** | 4.84 | 4.20 | 264M | 37M | **PASS** |
| `T-25a` | quiet, no match | 3.71 s | 261.0 ms | 342.9 ms | **14.20×** | **1.31×** | 4.76 | 4.01 | 261M | 37M | **PASS** |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | 6.0 ms | **1.01×** | **2.23×** | 0.00 | 0.01 | 37M | 37M | **PASS** |
| `T-26` | byte offsets | 4.38 s | 262.5 ms | 362.9 ms | **16.71×** | **1.38×** | 4.87 | 4.25 | 262M | 37M | **PASS** |
| `T-27` | sub-scope drivers/ | 2.02 s | 152.4 ms | 166.4 ms | **13.26×** | **1.09×** | 2.26 | 1.90 | 262M | 37M | **PASS** |
| `T-28` | sub-scope kernel mm fs | 169.5 ms | 21.5 ms | 20.1 ms | **7.89×** | **0.93×** | 0.19 | 0.16 | 37M | 37M | **PASS** |

### Category B — Single Huge File (`$BIG` ≈ 1.39 GB)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `B-01` | rare literal (`$BIG`) | 576.7 ms | 19.7 ms | 267.1 ms | **29.20×** | **13.53×** | 0.41 | 0.28 | 1382M | 1391M | **PASS** |
| `B-02` | count dense (`$BIG`) | 651.2 ms | 20.7 ms | 285.0 ms | **31.48×** | **13.78×** | 0.43 | 0.28 | 1382M | 1391M | **PASS** |
| `B-03` | moderate, line numbers (`$BIG`) | 981.0 ms | 40.0 ms | 336.7 ms | **24.54×** | **8.42×** | 0.94 | 0.33 | 1382M | 1391M | **PASS** |
| `B-04` | DFA regex (`$BIG`) | 478.7 ms | 22.5 ms | 256.0 ms | **21.28×** | **11.38×** | 0.45 | 0.26 | 1379M | 1391M | **PASS** |
| `B-05` | case-insensitive dense (`$BIG`) | 2.04 s | 185.3 ms | 394.4 ms | **10.98×** | **2.13×** | 2.46 | 0.39 | 1387M | 1391M | **PASS** |
| `B-06` | word match (`$BIG`) | 411.0 ms | 22.4 ms | 271.4 ms | **18.35×** | **12.12×** | 0.43 | 0.27 | 1379M | 1391M | **PASS** |
| `B-07` | literal-free regex (`$BIG`) | 704.5 ms | 39.5 ms | 1.68 s | **17.83×** | **42.51×** | 0.83 | 1.68 | 1373M | 1391M | **PASS** |
| `B-08` | PCRE2 lookaround (`$BIG`) | 21.44 s | 924.6 ms | 21.81 s | **23.19×** | **23.59×** | 22.24 | 21.80 | 1380M | 1391M | **PASS** |
| `B-09` | context (`$BIG`) | 970.7 ms | 31.0 ms | 340.8 ms | **31.29×** | **10.99×** | 0.74 | 0.34 | 1379M | 1391M | **PASS** |
| `B-10` | only-matching (`$BIG`) | 820.1 ms | 46.0 ms | 282.9 ms | **17.83×** | **6.15×** | 0.69 | 0.28 | 1374M | 1391M | **PASS** |
| `B-11` | inverted count (`$BIG`) | 1.02 s | 48.4 ms | 1.55 s | **21.15×** | **32.10×** | 1.06 | 1.55 | 1378M | 1391M | **PASS** |
| `B-12` | stdin redirect (`< $BIG`) | 574.1 ms | 18.6 ms | 303.7 ms | **30.82×** | **16.30×** | 0.41 | 0.30 | 1382M | 37M | **PASS** |
| `B-13` | FIFO pipe (cat `$BIG` \\|) | 1.12 s | 604.6 ms | 885.9 ms | **1.85×** | **1.47×** | 0.80 | 0.59 | 37M | 37M | **PASS** |
| `B-14` | real 24 MB header (big-24M.h) | 49.2 ms | 17.2 ms | 43.7 ms | **2.86×** | **2.54×** | 0.07 | 0.04 | 37M | 37M | **PASS** |

### Category M — Multi-Pattern Sets

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `M-01` | 100 literals, tree | 12.18 s | 276.1 ms | 648.9 ms | **44.10×** | **2.35×** | 5.32 | 7.66 | 261M | 37M | **PASS** |
| `M-02` | 1,000 literals, tree | 14.00 s | 284.8 ms | 709.7 ms | **49.17×** | **2.49×** | 5.69 | 8.30 | 271M | 67M | **PASS** |
| `M-03` | 10,000 literals, tree | 17.08 s | 351.2 ms | 1.16 s | **48.64×** | **3.31×** | 6.52 | 13.52 | 332M | 37M | **PASS** |
| `M-04` | 1,000 literals, -i | 94.65 s | 4.42 s | 1.56 s | **21.40×** | **0.35×** | 112.28 | 18.14 | 698M | 147M | **PASS** |
| `M-05` | 1,000 literals, -w | 14.12 s | 293.3 ms | 897.4 ms | **48.15×** | **3.06×** | 5.69 | 10.50 | 273M | 72M | **PASS** |
| `M-06` | 1,000 literals, `$BIG` | 8.97 s | 43.5 ms | 3.22 s | **206.01×** | **74.04×** | 0.79 | 3.23 | 1386M | 1404M | **PASS** |

### Category L (Per-Invocation Latency)

> Iteration count: 30–100 measured rounds (5 warm-ups). Net latency = Raw latency − L-00 harness floor median (/bin/true).

#### Detailed Latency Percentiles (Raw vs Net)

| Scenario | Tool | N | Raw p50 | Raw p90 | Raw p99 | Net p50 | Net p90 | Net p99 | CPU (s) | RSS |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `L-00` | **G0** | 100 | 1.5 ms | 1.6 ms | 1.7 ms | 0.00 ms | 0.17 ms | 0.28 ms | 0.00 | 37M |
| `L-00` | **G1** | 100 | 1.5 ms | 1.5 ms | 1.7 ms | 0.00 ms | 0.09 ms | 0.26 ms | 0.00 | 37M |
| `L-00` | **R0** | 100 | 1.5 ms | 1.6 ms | 1.8 ms | 0.00 ms | 0.11 ms | 0.30 ms | 0.00 | 37M |
| `L-01` | **G0** | 100 | 2.6 ms | 2.8 ms | 3.0 ms | 1.1 ms | 1.4 ms | 1.5 ms | 0.00 | 37M |
| `L-01` | **G1** | 100 | 2.6 ms | 2.8 ms | 2.9 ms | 1.2 ms | 1.3 ms | 1.4 ms | 0.00 | 37M |
| `L-01` | **R0** | 100 | 3.7 ms | 5.0 ms | 5.1 ms | 2.2 ms | 3.5 ms | 3.6 ms | 0.00 | 37M |
| `L-02` | **G0** | 50 | 7.0 ms | 7.2 ms | 7.8 ms | 5.6 ms | 5.8 ms | 6.4 ms | 0.01 | 37M |
| `L-02` | **G1** | 50 | 7.0 ms | 7.2 ms | 7.3 ms | 5.6 ms | 5.7 ms | 5.8 ms | 0.02 | 37M |
| `L-02` | **R0** | 50 | 7.1 ms | 8.1 ms | 8.8 ms | 5.6 ms | 6.7 ms | 7.3 ms | 0.01 | 37M |
| `L-03` | **G0** | 30 | 2.7 ms | 2.9 ms | 3.1 ms | 1.3 ms | 1.4 ms | 1.6 ms | 0.00 | 37M |
| `L-03` | **G1** | 30 | 3.9 ms | 5.0 ms | 5.2 ms | 2.4 ms | 3.5 ms | 3.7 ms | 0.01 | 37M |
| `L-03` | **R0** | 30 | 4.9 ms | 5.0 ms | 5.2 ms | 3.4 ms | 3.5 ms | 3.7 ms | 0.00 | 37M |

#### Summary Comparison (Net Latency Median)

| Scenario | Description | G0 Net | G1 Net | R0 Net | G1 vs G0 | G1 vs R0 | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | :---: |
| `L-00` | harness floor (/bin/true) | 0.00 ms | 0.00 ms | 0.00 ms | **—** | **—** | **PASS** |
| `L-01` | one small file (init/main.c) | 1.1 ms | 1.2 ms | 2.2 ms | **0.98×** | **1.93×** | **PASS** |
| `L-02` | tiny tree (kernel/sched) | 5.6 ms | 5.6 ms | 5.6 ms | **1.00×** | **1.01×** | **PASS** |
| `L-03` | quiet first match in big file | 1.3 ms | 2.4 ms | 3.4 ms | **0.53×** | **1.43×** | **PASS** |

### Category U — User Experience Defaults

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `U-01` | out-of-the-box UX | 4.39 s | 260.9 ms | 390.6 ms | **16.82×** | **1.50×** | 4.93 | 4.58 | 263M | 37M | **PASS** |
| `U-02` | naive invocation (scans 6 GB .git) | 6.00 s | 705.8 ms | 389.1 ms* | **8.50×** | **0.55×** | 5.71 | 4.56 | 294M | 37M | **PASS** |

> \* **Note on U-02 (`R0`)**: `rg` skips `.git` by default even without `-g '!.git'`; GNU `grep` requires explicit `--exclude-dir=.git` to avoid traversing `.git` pack files (hence scanning the 6 GB `.git` directory in U-02).

### Category X — xargs / External Parallelism

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `X-01` | git ls-files \\| xargs | 4.24 s | 1.01 s | 1.13 s | **4.19×** | **1.12×** | 8.22 | 5.68 | 62M | 37M | **PASS** |

### Category P — Pathological / Complex Regexes

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | Speedup (vs G0) | G1 vs R0 | G1 CPU | R0 CPU | G1 RSS | R0 RSS | Gate A |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 41.64 s | 1.51 s | 833.5 ms | **27.52×** | **0.55×** | 47.77 | 9.90 | 254M | 37M | **PASS** |
| `P-02` | 1,000-way regex alternation | 15.01 s | 1.01 s | 1.18 s | **14.81×** | **1.17×** | 15.09 | 13.79 | 456M | 136M | **PASS** |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 3.78 s | 278.9 ms | 346.1 ms | **13.55×** | **1.24×** | 4.90 | 4.04 | 265M | 37M | **PASS** |

## 5. Locale Sensitivity Analysis

### Locale Sensitivity: en_US.UTF-8 vs LC_ALL=C (22 Scenarios)

> Ratio = UTF-8 median / C median. A ratio > 1.00× indicates that the C locale is faster (multibyte/case-folding overhead in UTF-8).

| Scenario | Description | G0 UTF-8 | G0 C | G0 (U/C) | G1 UTF-8 | G1 C | G1 (U/C) | R0 UTF-8 | R0 C | R0 (U/C) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | rare literal (`$BIG`) | 576.7 ms | 577.1 ms | **1.00×** | 19.7 ms | 20.5 ms | **0.96×** | 267.1 ms | 262.8 ms | **1.02×** |
| `B-03` | moderate, line numbers (`$BIG`) | 981.0 ms | 981.5 ms | **1.00×** | 40.0 ms | 38.5 ms | **1.04×** | 336.7 ms | 331.1 ms | **1.02×** |
| `B-04` | DFA regex (`$BIG`) | 478.7 ms | 476.1 ms | **1.01×** | 22.5 ms | 21.6 ms | **1.04×** | 256.0 ms | 256.8 ms | **1.00×** |
| `B-05` | case-insensitive dense (`$BIG`) | 2.04 s | 1.35 s | **1.51×** | 185.3 ms | 41.2 ms | **4.50×** | 394.4 ms | 398.5 ms | **0.99×** |
| `B-13` | FIFO pipe (cat `$BIG` \\|) | 1.12 s | 1.18 s | **0.94×** | 604.6 ms | 709.0 ms | **0.85×** | 885.9 ms | 1.02 s | **0.87×** |
| `L-00` | harness floor (/bin/true) | 1.5 ms | 1.7 ms | **0.88×** | 1.5 ms | 1.8 ms | **0.82×** | 1.5 ms | 1.6 ms | **0.91×** |
| `L-01` | one small file (init/main.c) | 2.6 ms | 2.7 ms | **0.98×** | 2.6 ms | 2.7 ms | **0.99×** | 3.7 ms | 5.0 ms | **0.74×** |
| `M-02` | 1,000 literals, tree | 14.00 s | 13.95 s | **1.00×** | 284.8 ms | 287.5 ms | **0.99×** | 709.7 ms | 717.1 ms | **0.99×** |
| `M-04` | 1,000 literals, -i | 94.65 s | 18.46 s | **5.13×** | 4.42 s | 306.0 ms | **14.46×** | 1.56 s | 1.56 s | **1.00×** |
| `T-01` | zero-match literal | 3.72 s | 3.70 s | **1.01×** | 274.4 ms | 274.2 ms | **1.00×** | 351.4 ms | 344.6 ms | **1.02×** |
| `T-03` | moderate literal, line numbers | 4.41 s | 4.38 s | **1.01×** | 261.0 ms | 263.1 ms | **0.99×** | 368.1 ms | 362.7 ms | **1.01×** |
| `T-04` | dense literal, line numbers | 4.60 s | 4.62 s | **1.00×** | 306.0 ms | 319.1 ms | **0.96×** | 517.3 ms | 512.2 ms | **1.01×** |
| `T-06` | files-with-matches | 3.81 s | 3.83 s | **0.99×** | 274.5 ms | 268.2 ms | **1.02×** | 357.1 ms | 355.5 ms | **1.00×** |
| `T-09` | case-insensitive literal | 5.20 s | 4.38 s | **1.19×** | 298.7 ms | 296.3 ms | **1.01×** | 751.9 ms | 747.0 ms | **1.01×** |
| `T-10` | word match | 4.26 s | 4.19 s | **1.02×** | 276.5 ms | 290.1 ms | **0.95×** | 372.1 ms | 377.0 ms | **0.99×** |
| `T-11` | DFA regex with must-literal | 3.80 s | 3.80 s | **1.00×** | 268.2 ms | 275.3 ms | **0.97×** | 347.1 ms | 343.9 ms | **1.01×** |
| `T-12` | anchored dense regex | 4.71 s | 4.51 s | **1.04×** | 293.1 ms | 277.7 ms | **1.06×** | 608.2 ms | 612.2 ms | **0.99×** |
| `T-15` | literal-free regex | 4.16 s | 4.16 s | **1.00×** | 283.5 ms | 297.5 ms | **0.95×** | 377.0 ms | 383.3 ms | **0.98×** |
| `T-16` | PCRE2 lookaround, dense | 27.66 s | 11.56 s | **2.39×** | 990.0 ms | 489.7 ms | **2.02×** | 2.51 s | 2.51 s | **1.00×** |
| `T-25b` | quiet, immediate match | 2.7 ms | 2.7 ms | **1.00×** | 2.7 ms | 2.6 ms | **1.03×** | 6.0 ms | 6.1 ms | **0.99×** |
| `U-01` | out-of-the-box UX | 4.39 s | 4.39 s | **1.00×** | 260.9 ms | 276.0 ms | **0.95×** | 390.6 ms | 393.4 ms | **0.99×** |
| `U-02` | naive invocation (scans 6 GB .git) | 6.00 s | 6.00 s | **1.00×** | 705.8 ms | 747.9 ms | **0.94×** | 389.1 ms | 397.4 ms | **0.98×** |

## 6. Cold Page Cache Analysis

### Cold Page Cache Performance (fadvise Evicted)

> Eviction via posix_fadvise(POSIX_FADV_DONTNEED) with residency verification (< 2% resident) before every run.
> Cold/Warm Ratio = Cold median / Warm median. Cold Throughput = Corpus Size / Cold Time.

| Scenario | Description | Tool | Warm (ms) | Cold (ms) | Cold / Warm Ratio | Cold Throughput |
| :--- | :--- | :---: | ---: | ---: | ---: | ---: |
| `B-01` | rare literal (`$BIG`) | **G0** | 576.7 ms | 3.49 s | 6.04× | 399 MB/s |
| `B-01` | rare literal (`$BIG`) | **G1** | 19.7 ms | 1.27 s | 64.18× | 1.07 GB/s |
| `B-01` | rare literal (`$BIG`) | **R0** | 267.1 ms | 3.23 s | 12.09× | 431 MB/s |
| `T-01` | zero-match literal | **G0** | 3.72 s | 52.41 s | 14.08× | 31 MB/s |
| `T-01` | zero-match literal | **G1** | 274.4 ms | 2.12 s | 7.73× | 773 MB/s |
| `T-01` | zero-match literal | **R0** | 351.4 ms | 4.69 s | 13.35× | 350 MB/s |
| `T-03` | moderate literal, line numbers | **G0** | 4.41 s | 52.13 s | 11.83× | 31 MB/s |
| `T-03` | moderate literal, line numbers | **G1** | 261.0 ms | 2.08 s | 7.98× | 788 MB/s |
| `T-03` | moderate literal, line numbers | **R0** | 368.1 ms | 4.73 s | 12.84× | 347 MB/s |
| `T-06` | files-with-matches | **G0** | 3.81 s | 51.71 s | 13.56× | 32 MB/s |
| `T-06` | files-with-matches | **G1** | 274.5 ms | 2.14 s | 7.80× | 767 MB/s |
| `T-06` | files-with-matches | **R0** | 357.1 ms | 4.82 s | 13.51× | 340 MB/s |

## 7. Thread Scaling Analysis

### Multi-Thread Scaling Sweep

| Scenario | Tool | T=1 | T=2 | T=4 | T=8 | T=12 | T=16 | T=24 | T=32 | T=48 | T=64 | T=96 | T=128 | Peak Speedup vs G0 |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | **G1** | 271.6 ms (2.1×) | 172.0 ms (3.4×) | 120.1 ms (4.8×) | 36.8 ms (15.7×) | 26.8 ms (21.5×) | 24.8 ms (23.2×) | 20.9 ms (27.6×) | 19.4 ms (29.7×) | 19.3 ms (29.8×) | 19.9 ms (28.9×) | 20.2 ms (28.5×) | 21.4 ms (26.9×) | **29.80×** |
| `B-01` | **R0** | 245.0 ms (2.4×) | 241.8 ms (2.4×) | 244.3 ms (2.4×) | 243.7 ms (2.4×) | 242.1 ms (2.4×) | 244.3 ms (2.4×) | 244.8 ms (2.4×) | 246.0 ms (2.3×) | 242.8 ms (2.4×) | 245.2 ms (2.4×) | 245.0 ms (2.4×) | 245.6 ms (2.3×) | **2.38×** |
| `B-04` | **G1** | 292.2 ms (1.6×) | 179.8 ms (2.7×) | 126.0 ms (3.8×) | 35.7 ms (13.4×) | 31.6 ms (15.2×) | 26.9 ms (17.8×) | 22.8 ms (21.0×) | 20.7 ms (23.1×) | 21.1 ms (22.7×) | 22.1 ms (21.7×) | 22.1 ms (21.6×) | 23.5 ms (20.4×) | **23.13×** |
| `B-04` | **R0** | 241.0 ms (2.0×) | 246.2 ms (1.9×) | 242.5 ms (2.0×) | 245.2 ms (2.0×) | 245.1 ms (2.0×) | 243.2 ms (2.0×) | 244.6 ms (2.0×) | 245.4 ms (2.0×) | 243.9 ms (2.0×) | 243.8 ms (2.0×) | 244.1 ms (2.0×) | 246.3 ms (1.9×) | **1.99×** |
| `M-02` | **G1** | 4.06 s (3.5×) | 1.40 s (10.0×) | 891.5 ms (15.7×) | 515.0 ms (27.2×) | 395.4 ms (35.4×) | 333.6 ms (42.0×) | 296.9 ms (47.2×) | 291.1 ms (48.1×) | 327.7 ms (42.7×) | 358.1 ms (39.1×) | 394.4 ms (35.5×) | 428.1 ms (32.7×) | **48.11×** |
| `M-02` | **R0** | 7.67 s (1.8×) | 3.86 s (3.6×) | 1.98 s (7.1×) | 1.04 s (13.5×) | 711.8 ms (19.7×) | 560.9 ms (25.0×) | 411.7 ms (34.0×) | 361.9 ms (38.7×) | 339.5 ms (41.3×) | 354.6 ms (39.5×) | 380.6 ms (36.8×) | 405.7 ms (34.5×) | **41.26×** |
| `T-01` | **G1** | 3.38 s (1.1×) | 1.18 s (3.2×) | 763.2 ms (4.9×) | 457.4 ms (8.1×) | 345.7 ms (10.8×) | 298.6 ms (12.5×) | 284.1 ms (13.1×) | 282.6 ms (13.2×) | 312.5 ms (11.9×) | 339.7 ms (11.0×) | 370.2 ms (10.1×) | 411.0 ms (9.1×) | **13.17×** |
| `T-01` | **R0** | 3.70 s (1.0×) | 1.92 s (1.9×) | 982.1 ms (3.8×) | 503.0 ms (7.4×) | 350.1 ms (10.6×) | 270.1 ms (13.8×) | 202.3 ms (18.4×) | 178.6 ms (20.8×) | 169.5 ms (22.0×) | 171.2 ms (21.7×) | 235.5 ms (15.8×) | 240.0 ms (15.5×) | **21.96×** |
| `T-04` | **G1** | 3.61 s (1.3×) | 1.27 s (3.6×) | 789.1 ms (5.8×) | 474.1 ms (9.7×) | 354.5 ms (13.0×) | 313.4 ms (14.7×) | 293.4 ms (15.7×) | 279.6 ms (16.5×) | 387.7 ms (11.9×) | 454.4 ms (10.1×) | 387.5 ms (11.9×) | 406.1 ms (11.3×) | **16.45×** |
| `T-04` | **R0** | 4.69 s (1.0×) | 2.55 s (1.8×) | 1.33 s (3.5×) | 705.5 ms (6.5×) | 504.0 ms (9.1×) | 442.4 ms (10.4×) | 475.9 ms (9.7×) | 498.8 ms (9.2×) | 529.7 ms (8.7×) | 555.7 ms (8.3×) | 638.6 ms (7.2×) | 636.5 ms (7.2×) | **10.40×** |
| `T-06` | **G1** | 3.38 s (1.1×) | 1.16 s (3.3×) | 756.6 ms (5.0×) | 448.3 ms (8.5×) | 330.7 ms (11.5×) | 286.8 ms (13.3×) | 264.9 ms (14.4×) | 263.3 ms (14.5×) | 312.0 ms (12.2×) | 339.6 ms (11.2×) | 369.6 ms (10.3×) | 383.6 ms (9.9×) | **14.48×** |
| `T-06` | **R0** | 3.82 s (1.0×) | 2.01 s (1.9×) | 1.02 s (3.8×) | 519.6 ms (7.3×) | 353.6 ms (10.8×) | 276.3 ms (13.8×) | 206.0 ms (18.5×) | 178.7 ms (21.3×) | 172.3 ms (22.1×) | 228.5 ms (16.7×) | 257.6 ms (14.8×) | 261.5 ms (14.6×) | **22.13×** |
| `T-10` | **G1** | 3.44 s (1.2×) | 1.21 s (3.5×) | 754.4 ms (5.7×) | 449.7 ms (9.5×) | 338.9 ms (12.6×) | 290.7 ms (14.7×) | 268.8 ms (15.9×) | 276.9 ms (15.4×) | 313.6 ms (13.6×) | 344.3 ms (12.4×) | 378.5 ms (11.3×) | 392.3 ms (10.9×) | **15.87×** |
| `T-10` | **R0** | 3.96 s (1.1×) | 2.07 s (2.1×) | 1.07 s (4.0×) | 538.1 ms (7.9×) | 365.5 ms (11.7×) | 286.5 ms (14.9×) | 213.6 ms (20.0×) | 250.3 ms (17.0×) | 247.9 ms (17.2×) | 268.3 ms (15.9×) | 274.8 ms (15.5×) | 268.4 ms (15.9×) | **19.96×** |
| `T-15` | **G1** | 4.02 s (1.0×) | 1.36 s (3.1×) | 853.2 ms (4.9×) | 513.4 ms (8.1×) | 379.9 ms (10.9×) | 317.5 ms (13.1×) | 267.0 ms (15.6×) | 281.8 ms (14.8×) | 300.6 ms (13.8×) | 350.7 ms (11.9×) | 380.3 ms (10.9×) | 405.3 ms (10.3×) | **15.57×** |
| `T-15` | **R0** | 4.08 s (1.0×) | 2.14 s (1.9×) | 1.08 s (3.8×) | 551.6 ms (7.5×) | 376.1 ms (11.1×) | 284.8 ms (14.6×) | 201.7 ms (20.6×) | 183.6 ms (22.6×) | 173.7 ms (23.9×) | 174.2 ms (23.9×) | 237.4 ms (17.5×) | 233.0 ms (17.8×) | **23.93×** |
| `T-16` | **G1** | 27.59 s (1.0×) | 9.17 s (3.0×) | 5.54 s (5.0×) | 3.10 s (8.9×) | 2.18 s (12.7×) | 1.68 s (16.5×) | 1.18 s (23.3×) | 967.4 ms (28.6×) | 917.6 ms (30.1×) | 989.5 ms (27.9×) | 890.7 ms (31.1×) | 962.4 ms (28.7×) | **31.05×** |
| `T-16` | **R0** | 29.26 s (0.9×) | 14.82 s (1.9×) | 7.45 s (3.7×) | 3.73 s (7.4×) | 2.48 s (11.1×) | 1.89 s (14.6×) | 1.38 s (20.0×) | 1.11 s (24.8×) | 1.08 s (25.7×) | 1.03 s (26.9×) | 931.9 ms (29.7×) | 996.4 ms (27.8×) | **29.68×** |

## 8. Hypothesis Scorecard

### Hypothesis Scorecard (H1 – H9)

| # | Hypothesis | Target Criterion | Observed Outcome | Status |
| :-: | :--- | :--- | :--- | :---: |
| **H1** | Tree literal scan speedup | Ours ≥ 5× G0; within ±30% of R0 | T-01: 13.56× vs G0, 1.28× vs R0; T-04: 15.03× vs G0, 1.69× vs R0 | ✅ PASS |
| **H2** | `$BIG` intra-file parallelism | Ours ≥ 4× R0 (R0 searches 1 file with 1 thread) | B-01: 13.53× vs R0; B-03: 8.42× vs R0 | ✅ PASS |
| **H3** | Tiny invocation startup latency | Ours ≤ 1.5× G0; R0 ≈ 2–3× G0 | G0: 2.6 ms, G1: 2.6 ms (1.00×), R0: 3.7 ms | ✅ PASS |
| **H4** | Output-bound queries (~1.3M lines) | Ours never slower than G0 | T-05: G1 is 13.43× vs G0 | ✅ PASS |
| **H5** | UTF-8 vs C locale sensitivity | Both greps slower in UTF-8 on -i; R0 unaffected | T-09 (-i): G0 1.19× slower in UTF-8, G1 1.01×; R0 1.01× (unaffected) | ⚠️ CHECK |
| **H6** | Cold page cache | All tools bounded by PD bandwidth (within 20–35%) | T-01: G1 2.12 s (773 MB/s), R0 4.69 s (350 MB/s), G0 52.41 s | ⚠️ DIVERGENT |
| **H7** | 1,000-literal multi-pattern (M-02) | Ours within 2× of R0 | M-02: 49.17× vs G0, 2.49× vs R0 | ✅ PASS |
| **H8** | PCRE2 lookaround (T-16, B-08) | Ours ≫ G0 (literal pre-filter + threads) and ≥ R0 | T-16: 27.94× vs G0, 2.53× vs R0; B-08: 23.19× vs G0 | ✅ PASS |
| **H9** | Scaling past 16 threads | Gains throughput past 16 threads | B-01: 1.25× (16→64 th); 1..128 thread sweep across 128 logical CPUs (2 NUMA nodes) | ✅ PASS |

