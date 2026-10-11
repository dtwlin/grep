# Linux Kernel-Tree 3-Way Benchmark Results

> Multi-threaded GNU `grep` fork (G1) vs Stock upstream GNU `grep` (G0) vs `ripgrep` 15.1.0 (R0) vs Distro GNU `grep` 3.12 (G2) vs `git grep` (X1).

## 1. System & Binary Provenance

- **Host**: AMD EPYC 7B13 Zen 3 (128 logical cores), 236 GiB RAM
- **Kernel**: `7.1.6`
- **THP**: `[always] madvise never`
- **Corpus**: Linux kernel clean working tree (96,034 files, 1.64 GB), `linux-all.c` (1.39 GB)
- **Isolation**: NUMA node 0,1 (CPUs 0-127, 128 logical CPUs)
- **G0**: Upstream GNU grep baseline `3.12.41` (1 thread)
- **G1**: Our multi-threaded fork (`394272b`, default 17 threads)
- **R0**: Stock ripgrep 15.1.0 (default 13 threads)
- **G2**: Distro GNU grep 3.12 (1 thread)
- **X1**: Git grep 2.56.0 (default 129 threads)

## 2. Headline Results (Full Scenario Matrix)

| Scenario | Description | G0 (ms) | G1 (ms) | R0 (ms) | G2 (ms) | X1 (ms) | G1 vs G0 | R0 vs G0 | G1 vs R0 | G1 CPU | R0 CPU | G2 CPU | X1 CPU | G1 RSS | R0 RSS | G2 RSS | X1 RSS | Gate A | Gate B |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| `B-01` |  | — | invalid (mismatch) | — | — | — | **—** | — | **—** | — | — | — | — | — | — | — | — | ❌ N/A | N/A |
| `B-04` |  | — | invalid (mismatch) | — | — | — | **—** | — | **—** | — | — | — | — | — | — | — | — | ❌ N/A | N/A |
| `M-02` |  | — | invalid (mismatch) | — | — | DNF (> 600 s) | **—** | — | **—** | — | — | — | — | — | — | — | — | ❌ N/A | N/A |
| `T-01` |  | — | invalid (mismatch) | — | — | — | **—** | — | **—** | — | — | — | — | — | — | — | — | ❌ N/A | N/A |
| `T-04` |  | — | invalid (mismatch) | — | — | — | **—** | — | **—** | — | — | — | — | — | — | — | — | ❌ N/A | N/A |
| `T-06` |  | — | invalid (mismatch) | — | — | — | **—** | — | **—** | — | — | — | — | — | — | — | — | ❌ N/A | N/A |
| `T-10` |  | — | invalid (mismatch) | — | — | — | **—** | — | **—** | — | — | — | — | — | — | — | — | ❌ N/A | N/A |
| `T-15` |  | — | invalid (mismatch) | — | — | — | **—** | — | **—** | — | — | — | — | — | — | — | — | ❌ N/A | N/A |

## 3. Correctness & Equivalence Gates

- **Gate A (Hard)**: G1 byte-exact match against G0 on stdout, normalized stderr, and exit code.
- **Gate B (Soft)**: R0 / G2 / X1 equivalence against G0 (sorted match set).
- **Gate C**: Exit-code consistency.
- **Gate D**: G1 thread invariance (identical output across 1..64 threads).

Detailed equivalence records are available in [equivalence.md](./equivalence.md).

## 4. Per-Category Breakdowns

## 7. Thread Scaling Analysis

### Multi-Thread Scaling Sweep

| Scenario | Tool | T=1 | T=2 | T=4 | T=8 | T=12 | T=16 | T=24 | T=32 | T=48 | T=64 | T=96 | T=128 | Peak Speedup vs G0 |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `B-01` | **G1** | 244.2 ms (0.0×) | 130.3 ms (0.0×) | 87.6 ms (0.0×) | 32.1 ms (0.0×) | 27.3 ms (0.0×) | 22.5 ms (0.0×) | 19.8 ms (0.1×) | 19.3 ms (0.1×) | 18.7 ms (0.1×) | 18.9 ms (0.1×) | 20.3 ms (0.0×) | 20.3 ms (0.0×) | **0.05×** |
| `B-01` | **R0** | 189.4 ms (0.0×) | 192.8 ms (0.0×) | 192.9 ms (0.0×) | 193.2 ms (0.0×) | 193.7 ms (0.0×) | 193.4 ms (0.0×) | 195.2 ms (0.0×) | 192.5 ms (0.0×) | 194.9 ms (0.0×) | 190.9 ms (0.0×) | 198.5 ms (0.0×) | 191.9 ms (0.0×) | **0.01×** |
| `B-04` | **G1** | 271.0 ms (0.0×) | 140.1 ms (0.0×) | 92.6 ms (0.0×) | 37.5 ms (0.0×) | 28.5 ms (0.0×) | 25.8 ms (0.0×) | 21.9 ms (0.0×) | 21.0 ms (0.0×) | 23.4 ms (0.0×) | 21.4 ms (0.0×) | 21.5 ms (0.0×) | 23.4 ms (0.0×) | **0.05×** |
| `B-04` | **R0** | 195.8 ms (0.0×) | 191.2 ms (0.0×) | 191.6 ms (0.0×) | 195.3 ms (0.0×) | 192.2 ms (0.0×) | 195.4 ms (0.0×) | 195.3 ms (0.0×) | 195.6 ms (0.0×) | 195.3 ms (0.0×) | 198.9 ms (0.0×) | 194.4 ms (0.0×) | 193.2 ms (0.0×) | **0.01×** |
| `M-02` | **G1** | 4.07 s (0.0×) | 1.43 s (0.0×) | 899.4 ms (0.0×) | 514.0 ms (0.0×) | 377.1 ms (0.0×) | 310.7 ms (0.0×) | 274.5 ms (0.0×) | 251.3 ms (0.0×) | 281.8 ms (0.0×) | 314.3 ms (0.0×) | 342.0 ms (0.0×) | 388.2 ms (0.0×) | **0.00×** |
| `M-02` | **R0** | 7.70 s (0.0×) | 3.93 s (0.0×) | 2.03 s (0.0×) | 1.05 s (0.0×) | 716.5 ms (0.0×) | 553.9 ms (0.0×) | 398.7 ms (0.0×) | 338.3 ms (0.0×) | 360.0 ms (0.0×) | 361.0 ms (0.0×) | 387.7 ms (0.0×) | 405.9 ms (0.0×) | **0.00×** |
| `T-01` | **G1** | 3.42 s (0.0×) | 1.20 s (0.0×) | 753.0 ms (0.0×) | 436.5 ms (0.0×) | 322.9 ms (0.0×) | 271.7 ms (0.0×) | 247.3 ms (0.0×) | 238.5 ms (0.0×) | 274.5 ms (0.0×) | 310.6 ms (0.0×) | 352.5 ms (0.0×) | 387.4 ms (0.0×) | **0.00×** |
| `T-01` | **R0** | 3.74 s (0.0×) | 1.97 s (0.0×) | 1.01 s (0.0×) | 508.1 ms (0.0×) | 342.2 ms (0.0×) | 266.5 ms (0.0×) | 187.8 ms (0.0×) | 156.7 ms (0.0×) | 135.7 ms (0.0×) | 168.7 ms (0.0×) | 223.5 ms (0.0×) | 224.4 ms (0.0×) | **0.01×** |
| `T-04` | **G1** | 3.65 s (0.0×) | 1.24 s (0.0×) | 777.7 ms (0.0×) | 460.9 ms (0.0×) | 351.3 ms (0.0×) | 288.2 ms (0.0×) | 255.5 ms (0.0×) | 275.1 ms (0.0×) | 267.9 ms (0.0×) | 324.6 ms (0.0×) | 348.6 ms (0.0×) | 366.2 ms (0.0×) | **0.00×** |
| `T-04` | **R0** | 4.74 s (0.0×) | 2.57 s (0.0×) | 1.32 s (0.0×) | 706.3 ms (0.0×) | 510.0 ms (0.0×) | 438.7 ms (0.0×) | 477.4 ms (0.0×) | 509.0 ms (0.0×) | 539.0 ms (0.0×) | 552.8 ms (0.0×) | 608.9 ms (0.0×) | 631.6 ms (0.0×) | **0.00×** |
| `T-06` | **G1** | 3.42 s (0.0×) | 1.25 s (0.0×) | 740.0 ms (0.0×) | 439.1 ms (0.0×) | 331.7 ms (0.0×) | 265.6 ms (0.0×) | 242.3 ms (0.0×) | 235.1 ms (0.0×) | 258.9 ms (0.0×) | 293.7 ms (0.0×) | 348.4 ms (0.0×) | 371.2 ms (0.0×) | **0.00×** |
| `T-06` | **R0** | 3.82 s (0.0×) | 2.02 s (0.0×) | 1.05 s (0.0×) | 524.8 ms (0.0×) | 357.8 ms (0.0×) | 279.8 ms (0.0×) | 202.4 ms (0.0×) | 178.9 ms (0.0×) | 170.2 ms (0.0×) | 228.4 ms (0.0×) | 252.5 ms (0.0×) | 253.6 ms (0.0×) | **0.01×** |
| `T-10` | **G1** | 3.47 s (0.0×) | 1.22 s (0.0×) | 757.3 ms (0.0×) | 443.3 ms (0.0×) | 328.5 ms (0.0×) | 274.7 ms (0.0×) | 251.6 ms (0.0×) | 236.2 ms (0.0×) | 270.3 ms (0.0×) | 297.6 ms (0.0×) | 352.9 ms (0.0×) | 357.5 ms (0.0×) | **0.00×** |
| `T-10` | **R0** | 4.00 s (0.0×) | 2.11 s (0.0×) | 1.08 s (0.0×) | 545.0 ms (0.0×) | 382.1 ms (0.0×) | 290.0 ms (0.0×) | 216.3 ms (0.0×) | 194.5 ms (0.0×) | 193.8 ms (0.0×) | 253.8 ms (0.0×) | 267.1 ms (0.0×) | 276.4 ms (0.0×) | **0.01×** |
| `T-15` | **G1** | 4.04 s (0.0×) | 1.43 s (0.0×) | 864.8 ms (0.0×) | 516.8 ms (0.0×) | 376.2 ms (0.0×) | 310.9 ms (0.0×) | 264.0 ms (0.0×) | 246.3 ms (0.0×) | 270.3 ms (0.0×) | 303.6 ms (0.0×) | 348.1 ms (0.0×) | 364.4 ms (0.0×) | **0.00×** |
| `T-15` | **R0** | 4.11 s (0.0×) | 2.18 s (0.0×) | 1.09 s (0.0×) | 566.4 ms (0.0×) | 386.8 ms (0.0×) | 288.7 ms (0.0×) | 201.7 ms (0.0×) | 169.5 ms (0.0×) | 141.6 ms (0.0×) | 200.3 ms (0.0×) | 227.0 ms (0.0×) | 230.1 ms (0.0×) | **0.01×** |

## 8. Hypothesis Scorecard

### Hypothesis Scorecard (H1 – H9)

| # | Hypothesis | Target Criterion | Observed Outcome | Status |
| :-: | :--- | :--- | :--- | :---: |
| **H1** | Tree literal scan speedup | Ours ≥ 5× G0; within ±30% of R0 | T-01: 0.00× vs G0, 0.00× vs R0; T-04: 0.00× vs G0, 0.00× vs R0 | ⚠️ PARTIAL (G0: 0.0×–0.0× ≥ 5×; R0: 0.00×–0.00×, outside ±30% on T-01/T-03) |
| **H2** | $BIG intra-file parallelism | Ours ≥ 4× R0 (R0 searches 1 file with 1 thread) | B-01: 0.00× vs R0; B-03: 0.00× vs R0 | ⚠️ CHECK |
| **H3** | Tiny invocation startup latency | Ours ≤ 1.5× G0; R0 ≈ 2–3× G0 | G0: 0.00 ms, G1: 0.00 ms (1.00×), R0: 0.00 ms | ✅ PASS |
| **H4** | Output-bound queries | Ours never slower than G0 | T-05 not run in --quick | ⏸️ PENDING FULL |
| **H5** | UTF-8 vs C locale sensitivity | Both greps slower in UTF-8 on -i; R0 unaffected | Evaluated in Phase 2 (C locale matrix) | ⏸️ PENDING FULL |
| **H6** | Cold page cache | All tools bounded by PD bandwidth (within 20%) | Evaluated in Phase 3 (cold cache matrix) | ⏸️ PENDING FULL |
| **H7** | 1,000-literal multi-pattern (M-02) | Ours within 2× of R0 | Not available | ⏸️ N/A |
| **H8** | PCRE2 lookaround (T-16, B-08) | Ours ≫ G0 (literal pre-filter + threads) and ≥ R0 | Evaluated in Category T / B full matrix | ⏸️ PENDING FULL |
| **H9** | Scaling past 16 threads | Gains throughput past 16 threads | B-01: 1.19× (16→64 th); 1..64 sweep on NUMA node 0 | ✅ PASS |

