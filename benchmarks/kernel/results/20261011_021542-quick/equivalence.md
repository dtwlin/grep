# Benchmark Correctness and Equivalence Audit

## 1. Gate A: G1 vs G0 Byte-Exact Identity

| Scenario | Description | G0 Exit | G1 Exit | G0 Stdout SHA-256 | G1 Stdout SHA-256 | Gate A Verdict |
| :--- | :--- | :---: | :---: | :--- | :--- | :---: |
| `B-01` | rare literal ($BIG) | 0 | 0 | `577b9115903a48ea...` | `577b9115903a48ea...` | **PASS** |
| `B-03` | moderate, line numbers ($BIG) | 0 | 0 | `a4ee27abab3ee2db...` | `a4ee27abab3ee2db...` | **PASS** |
| `B-04` | DFA regex ($BIG) | 0 | 0 | `417f066225890de5...` | `417f066225890de5...` | **PASS** |
| `B-13` | FIFO pipe (cat $BIG \|) | 0 | 0 | `a4ee27abab3ee2db...` | `a4ee27abab3ee2db...` | **PASS** |
| `L-00` | harness floor (/bin/true) | 0 | 0 | `e3b0c44298fc1c14...` | `e3b0c44298fc1c14...` | **PASS** |
| `L-01` | one small file (init/main.c) | 0 | 0 | `b26f4931fbf919dc...` | `b26f4931fbf919dc...` | **PASS** |
| `M-02` | 1,000 literals, tree | 0 | 0 | `bb127c7e0c72ff9b...` | `bb127c7e0c72ff9b...` | **PASS** |
| `T-01` | zero-match literal | 1 | 1 | `e3b0c44298fc1c14...` | `e3b0c44298fc1c14...` | **PASS** |
| `T-03` | moderate literal, line numbers | 0 | 0 | `de0565f1650b325b...` | `de0565f1650b325b...` | **PASS** |
| `T-04` | dense literal, line numbers | 0 | 0 | `d38c05890c69c62b...` | `d38c05890c69c62b...` | **PASS** |
| `T-06` | files-with-matches | 0 | 0 | `4c05d9bbc8122c6c...` | `4c05d9bbc8122c6c...` | **PASS** |
| `T-09` | case-insensitive literal | 0 | 0 | `89c9ed2595f10e24...` | `89c9ed2595f10e24...` | **PASS** |
| `T-10` | word match | 0 | 0 | `5a9dd3354494b976...` | `5a9dd3354494b976...` | **PASS** |
| `T-11` | DFA regex with must-literal | 0 | 0 | `a2b36fe7e849926b...` | `a2b36fe7e849926b...` | **PASS** |
| `T-15` | literal-free regex | 0 | 0 | `9d38dd89069e3cc1...` | `9d38dd89069e3cc1...` | **PASS** |
| `T-25b` | quiet, immediate match | 0 | 0 | `e3b0c44298fc1c14...` | `e3b0c44298fc1c14...` | **PASS** |
| `U-01` | out-of-the-box UX | 0 | 0 | `de0565f1650b325b...` | `de0565f1650b325b...` | **PASS** |
| `U-02` | naive invocation (scans 6 GB .git) | 0 | 0 | `de0565f1650b325b...` | `de0565f1650b325b...` | **PASS** |

## 2. Gate B: Cross-Tool Normalized Match Set Equivalence

| Scenario | Description | R0 Match vs G0 | G2 Match vs G0 | X1 Match vs G0 | Notes |
| :--- | :--- | :---: | :---: | :---: | :--- |
| `B-01` | rare literal ($BIG) | PASS | PASS | PASS | Sorted match |
| `B-03` | moderate, line numbers ($BIG) | PASS | PASS | PASS | Sorted match |
| `B-04` | DFA regex ($BIG) | PASS | PASS | PASS | Sorted match |
| `B-13` | FIFO pipe (cat $BIG \|) | PASS | PASS | N/A | Sorted match |
| `L-00` | harness floor (/bin/true) | PASS | PASS | PASS | Exit code 0 vs 0 |
| `L-01` | one small file (init/main.c) | PASS | PASS | PASS | Sorted match |
| `M-02` | 1,000 literals, tree | PASS | PASS | SOFT_DIFF | Sorted match |
| `T-01` | zero-match literal | PASS | PASS | PASS | Exit code 1 vs 1 |
| `T-03` | moderate literal, line numbers | PASS | PASS | PASS | Sorted match |
| `T-04` | dense literal, line numbers | PASS | PASS | PASS | Sorted match |
| `T-06` | files-with-matches | PASS | PASS | PASS | Sorted match |
| `T-09` | case-insensitive literal | KNOWN-SEMANTIC | PASS | PASS | KNOWN-SEMANTIC (BOM stripping) |
| `T-10` | word match | PASS | PASS | PASS | Sorted match |
| `T-11` | DFA regex with must-literal | PASS | PASS | PASS | Sorted match |
| `T-15` | literal-free regex | PASS | PASS | PASS | Sorted match |
| `T-25b` | quiet, immediate match | PASS | PASS | PASS | Exit code 0 vs 0 |
| `U-01` | out-of-the-box UX | PASS | PASS | N/A | Sorted match |
| `U-02` | naive invocation (scans 6 GB .git) | N/A | N/A | N/A | Intentionally divergent |

### Diff Samples & Semantic Classifications

#### `T-09` vs R0 (`KNOWN-SEMANTIC (BOM stripping)`)

Ripgrep strips the UTF-8 BOM at the start of `Documentation/translations/zh_CN/dev-tools/sparse.rst` (`\ufeffCopyright 2004 Linus Torvalds`), whereas GNU grep prints the BOM bytes (12,577,386 vs 12,577,383 bytes).

```diff
--- /dev/shm/kbench_out/ref_T-09_G0.sorted	2026-10-11 03:19:43.475750474 +0000
+++ /dev/shm/kbench_out/ref_T-09_R0.sorted	2026-10-11 03:19:45.231748074 +0000
@@ -780,9 +780,9 @@
 $K/Documentation/translations/sp_SP/process/kernel-enforcement-statement.rst:Aunque existe el derecho de hacer valer un copyright distinto en las
 $K/Documentation/translations/sp_SP/process/kernel-enforcement-statement.rst:nuestro y de cualquier sucesor de nuestros derechos de autor (copyright):
 $K/Documentation/translations/zh_CN/admin-guide/reporting-issues.rst:   way if you want to contribute changes to the text, but for copyright
+$K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2004 Linus Torvalds
 $K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2004 Pavel Machek <pavel@ucw.cz>
 $K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2006 Bob Copeland <me@bobcopeland.com>
-$K/Documentation/translations/zh_CN/dev-tools/sparse.rst:﻿Copyright 2004 Linus Torvalds
```

## 3. Gate D: G1 Thread Invariance

G1 deterministic output verification across thread counts {1, 4, 16, 32, 64, 128}:

| Scenario | Description | Threads Checked | Identical Stdout SHA-256 | Gate D Verdict |
| :--- | :--- | :--- | :---: | :---: |
| `B-01` | rare literal ($BIG) | 1, 4, 16, 32, 64, 128 | YES | **PASS** |
| `B-04` | DFA regex ($BIG) | 1, 4, 16, 32, 64, 128 | YES | **PASS** |
| `M-02` | 1,000 literals, tree | 1, 4, 16, 32, 64, 128 | YES | **PASS** |
| `T-01` | zero-match literal | 1, 4, 16, 32, 64, 128 | YES | **PASS** |
| `T-04` | dense literal, line numbers | 1, 4, 16, 32, 64, 128 | YES | **PASS** |
| `T-06` | files-with-matches | 1, 4, 16, 32, 64, 128 | YES | **PASS** |
| `T-10` | word match | 1, 4, 16, 32, 64, 128 | YES | **PASS** |
| `T-15` | literal-free regex | 1, 4, 16, 32, 64, 128 | YES | **PASS** |

