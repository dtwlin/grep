# Linux Kernel-Tree Benchmark: Before vs. After Comparison

## 9. Post-Merge Before vs. After Comparison

### 9.1 Provenance & Configuration Delta

Direct before-and-after comparison on identical hardware pinned to NUMA node 0 (64 logical CPUs):

- **Before Run**: `20261011_051200-full` (git commit `a08c744`)
  - Tree traversal: Serial directory walker (single-threaded readdir)
  - Literal search: Classic GNU kwset Aho-Corasick
  - Thread cap: `DEFAULT_MAX_THREADS = 16` (`min(64, 16) = 16` worker threads)
- **After Run**: `20261011_162043-after` (git commit `cf9f986`)
  - Tree traversal: Parallel work-stealing tree walker (`opt/parallel-walk`)
  - Literal search: Dense Aho-Corasick DFA (`opt/dense-ac`)
  - Correctness: `safe_dfaexec` state validation fix (`aa02aff`)
  - Thread cap: `DEFAULT_MAX_THREADS = 32` (`min(64, 32) = 32` worker threads)

> **Disambiguating Algorithmic vs Thread Scaling Gains**: Because G1's default thread count expanded from 16 to 32 under the 64-CPU affinity mask, G1 was also measured explicitly at `GREP_THREADS=16` on the 8 sweep scenarios. The **Engine Gain (16th)** isolates the pure algorithmic speedup under identical thread counts, while **Total Speedup** reflects the full out-of-the-box user experience.

### 9.2 Thread-Cap Isolation on Sweep Scenarios (16 vs 32 Threads)

| Scenario | Description | G1 Before (16th) | G1 After (16th) | Engine Gain (16th) | G1 After (32th) | Scaling (16→32) | Total Speedup | R0 After | G1 (32th) vs R0 |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **T-01** | zero-match literal | 488.2 | 261.2 | **1.87×** | 234.0 | 1.12× | **2.09×** | 344.4 | **1.47×** |
| **T-04** | dense literal, line numbers | 609.1 | 286.6 | **2.13×** | 251.7 | 1.14× | **2.42×** | 515.8 | **2.05×** |
| **T-06** | files-with-matches | 490.5 | 262.8 | **1.87×** | 236.1 | 1.11× | **2.08×** | 355.5 | **1.51×** |
| **T-10** | word match | 494.8 | 268.1 | **1.85×** | 242.5 | 1.11× | **2.04×** | 373.9 | **1.54×** |
| **T-15** | literal-free regex | 490.2 | 296.2 | **1.66×** | 271.0 | 1.09× | **1.81×** | 377.3 | **1.39×** |
| **B-01** | rare literal ($BIG) | 26.6 | 25.1 | **1.06×** | 18.7 | 1.34× | **1.43×** | 195.2 | **10.45×** |
| **B-04** | DFA regex ($BIG) | 29.7 | 26.1 | **1.14×** | 21.1 | 1.24× | **1.41×** | 198.0 | **9.39×** |
| **M-02** | 1,000 literals, tree | 1.02 s | 300.7 | **3.38×** | 257.6 | 1.17× | **3.95×** | 711.1 | **2.76×** |

### 9.3 Full Scenario Matrix (59 Warm Scenarios)

| Scenario | Category | Description | G1 Before (16th) | G1 After (32th) | Speedup vs Before | R0 After | G1 After vs R0 | Gate A (Before) | Gate A (After) |
| :--- | :---: | :--- | ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| **B-01** | B | rare literal ($BIG) | 26.6 | 18.7 | 1.43× | 195.2 | 10.45× | **PASS** | **PASS** |
| **B-02** | B | count dense ($BIG) | 29.7 | 19.6 | 1.51× | 229.9 | 11.71× | **PASS** | **PASS** |
| **B-03** | B | moderate, line numbers ($BIG) | 52.4 | 36.7 | 1.43× | 261.3 | 7.12× | **PASS** | **PASS** |
| **B-04** | B | DFA regex ($BIG) | 29.7 | 21.1 | 1.41× | 198.0 | 9.39× | **PASS** | **PASS** |
| **B-05** | B | case-insensitive dense ($BIG) | 222.3 | 152.8 | 1.45× | 330.6 | 2.16× | **PASS** | **PASS** |
| **B-06** | B | word match ($BIG) | 27.4 | 22.0 | 1.25× | 209.2 | 9.53× | **PASS** | **PASS** |
| **B-07** | B | literal-free regex ($BIG) | 59.2 | 36.0 | 1.64× | 1.62 s | 45.08× | **PASS** | **PASS** |
| **B-08** | B | PCRE2 lookaround ($BIG) | 1.57 s | 765.8 | 2.05× | 21.68 s | 28.32× | **PASS** | **PASS** |
| **B-09** | B | context ($BIG) | 44.1 | 31.0 | 1.42× | 277.3 | 8.95× | **PASS** | **PASS** |
| **B-10** | B | only-matching ($BIG) | invalid (exit 2) | 40.3 | — | 214.6 | 5.32× | ❌ FAIL | **PASS** |
| **B-11** | B | inverted count ($BIG) | 92.8 | 46.6 | 1.99× | 1.49 s | 32.08× | **PASS** | **PASS** |
| **B-12** | B | stdin redirect (< $BIG) | 27.3 | 18.7 | 1.46× | 250.1 | 13.38× | **PASS** | **PASS** |
| **B-13** | B | FIFO pipe (cat $BIG |) | 732.0 | 654.5 | 1.12× | 881.4 | 1.35× | **PASS** | **PASS** |
| **B-14** | B | real 24 MB header (big-24M.h) | 18.2 | 17.1 | 1.06× | 43.0 | 2.51× | **PASS** | **PASS** |
| **L-00** | L | harness floor (/bin/true) | 1.53 | 1.60 | 0.96× | 1.60 | 1.00× | **PASS** | **PASS** |
| **L-01** | L | one small file (init/main.c) | 2.73 | 2.88 | 0.95× | 3.65 | 1.27× | **PASS** | **PASS** |
| **L-02** | L | tiny tree (kernel/sched) | 7.15 | 7.25 | 0.99× | 7.26 | 1.00× | **PASS** | **PASS** |
| **L-03** | L | quiet first match in big file | 3.96 | 3.88 | 1.02× | 3.83 | 0.99× | **PASS** | **PASS** |
| **M-01** | M | 100 literals, tree | 875.8 | 256.1 | 3.42× | 646.3 | 2.52× | **PASS** | **PASS** |
| **M-02** | M | 1,000 literals, tree | 1.02 s | 257.6 | 3.95× | 711.1 | 2.76× | **PASS** | **PASS** |
| **M-03** | M | 10,000 literals, tree | 1.48 s | 331.0 | 4.48× | 1.16 s | 3.51× | **PASS** | **PASS** |
| **M-04** | M | 1,000 literals, -i | 7.19 s | 4.11 s | 1.75× | 1.54 s | 0.38× | **PASS** | **PASS** |
| **M-05** | M | 1,000 literals, -w | 1.03 s | 257.5 | 4.01× | 894.6 | 3.47× | **PASS** | **PASS** |
| **M-06** | M | 1,000 literals, $BIG | 920.9 | 41.6 | 22.11× | 3.16 s | 75.97× | **PASS** | **PASS** |
| **T-01** | T | zero-match literal | 488.2 | 234.0 | 2.09× | 344.4 | 1.47× | **PASS** | **PASS** |
| **T-02** | T | rare literal | 486.9 | 234.3 | 2.08× | 344.8 | 1.47× | **PASS** | **PASS** |
| **T-03** | T | moderate literal, line numbers | 501.0 | 239.5 | 2.09× | 361.8 | 1.51× | **PASS** | **PASS** |
| **T-04** | T | dense literal, line numbers | 609.1 | 251.7 | 2.42× | 515.8 | 2.05× | **PASS** | **PASS** |
| **T-05** | T | very dense literal | 949.1 | 414.8 | 2.29× | 753.3 | 1.82× | **PASS** | **PASS** |
| **T-06** | T | files-with-matches | 490.5 | 236.1 | 2.08× | 355.5 | 1.51× | **PASS** | **PASS** |
| **T-07** | T | files-without-match | 499.1 | 234.8 | 2.13× | 428.8 | 1.83× | **PASS** | **PASS** |
| **T-08** | T | count per file | 501.0 | 238.5 | 2.10× | 1.06 s | 4.46× | **PASS** | **PASS** |
| **T-09** | T | case-insensitive literal | 533.2 | 285.6 | 1.87× | 736.2 | 2.58× | **PASS** | **PASS** |
| **T-10** | T | word match | 494.8 | 242.5 | 2.04× | 373.9 | 1.54× | **PASS** | **PASS** |
| **T-11** | T | DFA regex with must-literal | 486.5 | 235.6 | 2.06× | 342.9 | 1.46× | **PASS** | **PASS** |
| **T-12** | T | anchored dense regex | 543.8 | 246.1 | 2.21× | 598.0 | 2.43× | **PASS** | **PASS** |
| **T-13** | T | literal alternation + word boundary | 481.9 | 247.6 | 1.95× | 362.8 | 1.47× | **PASS** | **PASS** |
| **T-14** | T | optional group + alternation | invalid (exit 2) | 236.6 | — | 343.7 | 1.45× | ❌ FAIL | **PASS** |
| **T-15** | T | literal-free regex | 490.2 | 271.0 | 1.81× | 377.3 | 1.39× | **PASS** | **PASS** |
| **T-16** | T | PCRE2 lookaround, dense | 1.92 s | 2.61 s | 0.74× | 2.48 s | 0.95× | **PASS** | **PASS** |
| **T-17** | T | PCRE2 rare with literal | 518.4 | 272.2 | 1.90× | 521.2 | 1.91× | **PASS** | **PASS** |
| **T-18** | T | only-matching | invalid (exit 2) | 253.4 | — | 366.7 | 1.45× | ❌ FAIL | **PASS** |
| **T-19** | T | context, rare | 491.6 | 244.6 | 2.01× | 363.3 | 1.49× | **PASS** | **PASS** |
| **T-20** | T | context, moderate | 507.6 | 247.8 | 2.05× | 380.4 | 1.54× | **PASS** | **PASS** |
| **T-21** | T | inverted count | 502.8 | 256.1 | 1.96× | 1.09 s | 4.25× | **PASS** | **PASS** |
| **T-22** | T | glob include | 397.0 | 181.6 | 2.19× | 169.6 | 0.93× | **PASS** | **PASS** |
| **T-23** | T | glob + sub-scope | 19.5 | 15.8 | 1.23× | 25.6 | 1.61× | **PASS** | **PASS** |
| **T-24** | T | max-count per file | 3.59 s | 237.3 | 15.11× | 359.0 | 1.51× | **PASS** | **PASS** |
| **T-25a** | T | quiet, no match | 487.5 | 252.5 | 1.93× | 345.7 | 1.37× | **PASS** | **PASS** |
| **T-25b** | T | quiet, immediate match | 5.90 | 2.65 | 2.23× | 5.88 | 2.22× | **PASS** | **PASS** |
| **T-26** | T | byte offsets | 491.0 | 243.6 | 2.02× | 365.6 | 1.50× | **PASS** | **PASS** |
| **T-27** | T | sub-scope drivers/ | 224.1 | 124.7 | 1.80× | 162.5 | 1.30× | **PASS** | **PASS** |
| **T-28** | T | sub-scope kernel mm fs | 19.2 | 21.6 | 0.89× | 20.1 | 0.93× | **PASS** | **PASS** |
| **U-01** | U | out-of-the-box UX | 493.2 | 249.6 | 1.98× | 394.0 | 1.58× | **PASS** | **PASS** |
| **U-02** | U | naive invocation (scans 6 GB .git) | 918.2 | 976.0 | 0.94× | 391.6 | 0.40× | **PASS** | **PASS** |
| **X-01** | X | git ls-files | xargs | 5.89 | 962.6 | 0.01× | 1.08 s | 1.12× | **PASS** | **PASS** |
| **P-01** | P | backreference (\b([a-z]{4,})_\1\b) | 2.93 s | 2.04 s | 1.44× | 830.9 | 0.41× | **PASS** | **PASS** |
| **P-02** | P | 1,000-way regex alternation | 1.41 s | 1.05 s | 1.35× | 1.18 s | 1.12× | **PASS** | **PASS** |
| **P-03** | P | dot-star heavy (.*_lock.*_unlock) | 476.8 | 240.4 | 1.98× | 347.5 | 1.45× | **PASS** | **PASS** |

### 9.4 Cold Cache Subset Comparison (fadvise Evicted)

| Scenario | Description | G1 Before (ms) | G1 After (ms) | Speedup vs Before | R0 After (ms) | G1 vs R0 After |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| **T-01** | zero-match literal | 3.60 s | 2.06 s | **1.74×** | 4.63 s | **2.24×** |
| **T-03** | moderate literal, line numbers | 3.58 s | 2.05 s | **1.74×** | 4.64 s | **2.26×** |
| **T-06** | files-with-matches | 3.50 s | 1.99 s | **1.76×** | 4.66 s | **2.34×** |
| **B-01** | rare literal ($BIG) | 1.16 s | 1.22 s | **0.95×** | 3.15 s | **2.59×** |
