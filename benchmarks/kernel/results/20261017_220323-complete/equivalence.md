# Benchmark Correctness and Equivalence Audit

## 1. Gate A: G1 vs G0 Byte-Exact Identity

| Scenario | Description | G0 Exit | G1 Exit | G0 Stdout SHA-256 | G1 Stdout SHA-256 | Gate A Verdict |
| :--- | :--- | :---: | :---: | :--- | :--- | :---: |
| `B-01` | rare literal ($BIG) | 0 | 0 | `577b9115903a48ea...` | `577b9115903a48ea...` | **PASS** |
| `B-02` | count dense ($BIG) | 0 | 0 | `1a2e8d12838af347...` | `1a2e8d12838af347...` | **PASS** |
| `B-03` | moderate, line numbers ($BIG) | 0 | 0 | `a4ee27abab3ee2db...` | `a4ee27abab3ee2db...` | **PASS** |
| `B-04` | DFA regex ($BIG) | 0 | 0 | `417f066225890de5...` | `417f066225890de5...` | **PASS** |
| `B-05` | case-insensitive dense ($BIG) | 0 | 0 | `b3cdc2921acb455f...` | `b3cdc2921acb455f...` | **PASS** |
| `B-06` | word match ($BIG) | 0 | 0 | `dd2228c28769d372...` | `dd2228c28769d372...` | **PASS** |
| `B-07` | literal-free regex ($BIG) | 0 | 0 | `f96b39e81f5cd85d...` | `f96b39e81f5cd85d...` | **PASS** |
| `B-08` | PCRE2 lookaround ($BIG) | 0 | 0 | `77cc98d8885d1307...` | `77cc98d8885d1307...` | **PASS** |
| `B-09` | context ($BIG) | 0 | 0 | `6dc0069e67ea8ced...` | `6dc0069e67ea8ced...` | **PASS** |
| `B-10` | only-matching ($BIG) | 0 | 0 | `9af5628abd8a637c...` | `9af5628abd8a637c...` | **PASS** |
| `B-11` | inverted count ($BIG) | 0 | 0 | `492efdd7b0a16d22...` | `492efdd7b0a16d22...` | **PASS** |
| `B-12` | stdin redirect (< $BIG) | 0 | 0 | `577b9115903a48ea...` | `577b9115903a48ea...` | **PASS** |
| `B-13` | FIFO pipe (cat $BIG \\|) | 0 | 0 | `a4ee27abab3ee2db...` | `a4ee27abab3ee2db...` | **PASS** |
| `B-14` | real 24 MB header (big-24M.h) | 0 | 0 | `3862b23249847b1a...` | `3862b23249847b1a...` | **PASS** |
| `L-00` | harness floor (/bin/true) | 0 | 0 | `e3b0c44298fc1c14...` | `e3b0c44298fc1c14...` | **PASS** |
| `L-01` | one small file (init/main.c) | 0 | 0 | `b26f4931fbf919dc...` | `b26f4931fbf919dc...` | **PASS** |
| `L-02` | tiny tree (kernel/sched) | 0 | 0 | `1cb7f41ee56d185e...` | `1cb7f41ee56d185e...` | **PASS** |
| `L-03` | quiet first match in big file | 0 | 0 | `e3b0c44298fc1c14...` | `e3b0c44298fc1c14...` | **PASS** |
| `M-01` | 100 literals, tree | 0 | 0 | `12baa744282bb0f4...` | `12baa744282bb0f4...` | **PASS** |
| `M-02` | 1,000 literals, tree | 0 | 0 | `bb127c7e0c72ff9b...` | `bb127c7e0c72ff9b...` | **PASS** |
| `M-03` | 10,000 literals, tree | 0 | 0 | `1e6826317d3ece2a...` | `1e6826317d3ece2a...` | **PASS** |
| `M-04` | 1,000 literals, -i | 0 | 0 | `782b6221de102b1c...` | `782b6221de102b1c...` | **PASS** |
| `M-05` | 1,000 literals, -w | 0 | 0 | `e666fb80e6b91441...` | `e666fb80e6b91441...` | **PASS** |
| `M-06` | 1,000 literals, $BIG | 0 | 0 | `133478ef4b3ce96a...` | `133478ef4b3ce96a...` | **PASS** |
| `T-01` | zero-match literal | 1 | 1 | `e3b0c44298fc1c14...` | `e3b0c44298fc1c14...` | **PASS** |
| `T-02` | rare literal | 0 | 0 | `7265d589c1a4c288...` | `7265d589c1a4c288...` | **PASS** |
| `T-03` | moderate literal, line numbers | 0 | 0 | `de0565f1650b325b...` | `de0565f1650b325b...` | **PASS** |
| `T-04` | dense literal, line numbers | 0 | 0 | `d38c05890c69c62b...` | `d38c05890c69c62b...` | **PASS** |
| `T-05` | very dense literal | 0 | 0 | `c58ca46cf6b8d54c...` | `c58ca46cf6b8d54c...` | **PASS** |
| `T-06` | files-with-matches | 0 | 0 | `4c05d9bbc8122c6c...` | `4c05d9bbc8122c6c...` | **PASS** |
| `T-07` | files-without-match | 0 | 0 | `b4addaf96a000d64...` | `b4addaf96a000d64...` | **PASS** |
| `T-08` | count per file | 0 | 0 | `7ea609d682efa8cb...` | `7ea609d682efa8cb...` | **PASS** |
| `T-09` | case-insensitive literal | 0 | 0 | `89c9ed2595f10e24...` | `89c9ed2595f10e24...` | **PASS** |
| `T-10` | word match | 0 | 0 | `5a9dd3354494b976...` | `5a9dd3354494b976...` | **PASS** |
| `T-11` | DFA regex with must-literal | 0 | 0 | `a2b36fe7e849926b...` | `a2b36fe7e849926b...` | **PASS** |
| `T-12` | anchored dense regex | 0 | 0 | `50ba3029c90ef482...` | `50ba3029c90ef482...` | **PASS** |
| `T-13` | literal alternation + word boundary | 0 | 0 | `a6bdf20482edb7e0...` | `a6bdf20482edb7e0...` | **PASS** |
| `T-14` | optional group + alternation | 0 | 0 | `b2232a388ddc3152...` | `b2232a388ddc3152...` | **PASS** |
| `T-15` | literal-free regex | 0 | 0 | `9d38dd89069e3cc1...` | `9d38dd89069e3cc1...` | **PASS** |
| `T-16` | PCRE2 lookaround, dense | 0 | 0 | `9925a782699b5009...` | `9925a782699b5009...` | **PASS** |
| `T-17` | PCRE2 rare with literal | 0 | 0 | `c33757b23a52a946...` | `c33757b23a52a946...` | **PASS** |
| `T-18` | only-matching | 0 | 0 | `1873072aef2dd209...` | `1873072aef2dd209...` | **PASS** |
| `T-19` | context, rare | 0 | 0 | `c56da17108abb88f...` | `c56da17108abb88f...` | **PASS** |
| `T-20` | context, moderate | 0 | 0 | `d45039ec994ac894...` | `d45039ec994ac894...` | **PASS** |
| `T-21` | inverted count | 0 | 0 | `4fe6ac60b5ace1da...` | `4fe6ac60b5ace1da...` | **PASS** |
| `T-22` | glob include | 0 | 0 | `c48a6c3950e74d17...` | `c48a6c3950e74d17...` | **PASS** |
| `T-23` | glob + sub-scope | 0 | 0 | `4a1833d79077f973...` | `4a1833d79077f973...` | **PASS** |
| `T-24` | max-count per file | 0 | 0 | `aabafeed7fd59195...` | `aabafeed7fd59195...` | **PASS** |
| `T-25a` | quiet, no match | 1 | 1 | `e3b0c44298fc1c14...` | `e3b0c44298fc1c14...` | **PASS** |
| `T-25b` | quiet, immediate match | 0 | 0 | `e3b0c44298fc1c14...` | `e3b0c44298fc1c14...` | **PASS** |
| `T-26` | byte offsets | 0 | 0 | `c22cffd7181b6057...` | `c22cffd7181b6057...` | **PASS** |
| `T-27` | sub-scope drivers/ | 0 | 0 | `661e5c3c3d6e9e54...` | `661e5c3c3d6e9e54...` | **PASS** |
| `T-28` | sub-scope kernel mm fs | 0 | 0 | `e69324c9b04f6387...` | `e69324c9b04f6387...` | **PASS** |
| `U-01` | out-of-the-box UX | 0 | 0 | `de0565f1650b325b...` | `de0565f1650b325b...` | **PASS** |
| `U-02` | naive invocation (scans 6 GB .git) | 0 | 0 | `de0565f1650b325b...` | `de0565f1650b325b...` | **PASS** |
| `X-01` | git ls-files \\| xargs | 123 | 123 | `e8535c34e7434929...` | `e8535c34e7434929...` | **PASS** |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 0 | 0 | `1ac62cb0ce208e32...` | `1ac62cb0ce208e32...` | **PASS** |
| `P-02` | 1,000-way regex alternation | 0 | 0 | `236038c5dd5034ab...` | `236038c5dd5034ab...` | **PASS** |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 0 | 0 | `0a219b2dfbf3747f...` | `0a219b2dfbf3747f...` | **PASS** |

## 2. Gate B: Cross-Tool Normalized Match Set Equivalence

| Scenario | Description | R0 Match vs G0 | U0 Match vs G0 | G2 Match vs G0 | X1 Match vs G0 | Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| `B-01` | rare literal ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-02` | count dense ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-03` | moderate, line numbers ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-04` | DFA regex ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-05` | case-insensitive dense ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-06` | word match ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-07` | literal-free regex ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-08` | PCRE2 lookaround ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-09` | context ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-10` | only-matching ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-11` | inverted count ($BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-12` | stdin redirect (< $BIG) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-13` | FIFO pipe (cat $BIG \\|) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `B-14` | real 24 MB header (big-24M.h) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `L-00` | harness floor (/bin/true) | PASS | PASS | N/A | N/A | R0: Exit code 0 vs 0; U0: Exit code 0 vs 0 |
| `L-01` | one small file (init/main.c) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `L-02` | tiny tree (kernel/sched) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `L-03` | quiet first match in big file | PASS | PASS | N/A | N/A | R0: Exit code 0 vs 0; U0: Exit code 0 vs 0 |
| `M-01` | 100 literals, tree | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `M-02` | 1,000 literals, tree | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `M-03` | 10,000 literals, tree | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `M-04` | 1,000 literals, -i | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `M-05` | 1,000 literals, -w | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `M-06` | 1,000 literals, $BIG | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-01` | zero-match literal | PASS | PASS | N/A | N/A | R0: Exit code 1 vs 1; U0: Exit code 1 vs 1 |
| `T-02` | rare literal | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-03` | moderate literal, line numbers | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-04` | dense literal, line numbers | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-05` | very dense literal | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-06` | files-with-matches | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-07` | files-without-match | SOFT_DIFF | PASS | N/A | N/A | R0: Diff / binary formatting; U0: Sorted match |
| `T-08` | count per file | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-09` | case-insensitive literal | KNOWN-SEMANTIC | KNOWN-SEMANTIC | N/A | N/A | R0: KNOWN-SEMANTIC (BOM stripping); U0: KNOWN-SEMANTIC (BOM stripping) |
| `T-10` | word match | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-11` | DFA regex with must-literal | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-12` | anchored dense regex | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-13` | literal alternation + word boundary | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-14` | optional group + alternation | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-15` | literal-free regex | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-16` | PCRE2 lookaround, dense | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-17` | PCRE2 rare with literal | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-18` | only-matching | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-19` | context, rare | PASS | KNOWN-SEMANTIC | N/A | N/A | R0: Sorted match; U0: KNOWN-SEMANTIC (no group separator between files) |
| `T-20` | context, moderate | PASS | KNOWN-SEMANTIC | N/A | N/A | R0: Sorted match; U0: KNOWN-SEMANTIC (no group separator between files) |
| `T-21` | inverted count | SOFT_DIFF | SOFT_DIFF | N/A | N/A | R0: Diff / binary formatting; U0: Diff / binary formatting |
| `T-22` | glob include | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-23` | glob + sub-scope | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-24` | max-count per file | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-25a` | quiet, no match | PASS | PASS | N/A | N/A | R0: Exit code 1 vs 1; U0: Exit code 1 vs 1 |
| `T-25b` | quiet, immediate match | PASS | PASS | N/A | N/A | R0: Exit code 0 vs 0; U0: Exit code 0 vs 0 |
| `T-26` | byte offsets | PASS | KNOWN-SEMANTIC | N/A | N/A | R0: Sorted match; U0: KNOWN-SEMANTIC (-b prints the match offset) |
| `T-27` | sub-scope drivers/ | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `T-28` | sub-scope kernel mm fs | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `U-01` | out-of-the-box UX | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `U-02` | naive invocation (scans 6 GB .git) | N/A | N/A | N/A | N/A | R0: Intentionally divergent; U0: Intentionally divergent |
| `X-01` | git ls-files \\| xargs | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `P-02` | 1,000-way regex alternation | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | PASS | PASS | N/A | N/A | R0: Sorted match; U0: Sorted match |

### Diff Samples & Semantic Classifications

#### `T-07` vs R0 (`SOFT_DIFF`)

```diff
--- /dev/shm/kbench_out/ref_T-07_G0.sorted	2026-10-17 22:25:06.595708479 +0000
+++ /dev/shm/kbench_out/ref_T-07_R0.sorted	2026-10-17 22:25:07.219383426 +0000
@@ -2450,7 +2450,6 @@
 $K/Documentation/iio/iio_configfs.rst
 $K/Documentation/iio/opt4060.rst
 $K/Documentation/images/COPYING-logo
-$K/Documentation/images/logo.gif
 $K/Documentation/images/logo.svg
 $K/Documentation/infiniband/core_locking.rst
 $K/Documentation/infiniband/ipoib.rst
```

#### `T-09` vs R0 (`KNOWN-SEMANTIC (BOM stripping)`)

Ripgrep strips the UTF-8 BOM at the start of `Documentation/translations/zh_CN/dev-tools/sparse.rst` (`\ufeffCopyright 2004 Linus Torvalds`), whereas GNU grep prints the BOM bytes (12,577,386 vs 12,577,383 bytes).

```diff
--- /dev/shm/kbench_out/ref_T-09_G0.sorted	2026-10-17 22:50:41.902197902 +0000
+++ /dev/shm/kbench_out/ref_T-09_R0.sorted	2026-10-17 23:32:47.655502624 +0000
@@ -780,9 +780,9 @@
 $K/Documentation/translations/sp_SP/process/kernel-enforcement-statement.rst:Aunque existe el derecho de hacer valer un copyright distinto en las
 $K/Documentation/translations/sp_SP/process/kernel-enforcement-statement.rst:nuestro y de cualquier sucesor de nuestros derechos de autor (copyright):
 $K/Documentation/translations/zh_CN/admin-guide/reporting-issues.rst:   way if you want to contribute changes to the text, but for copyright
+$K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2004 Linus Torvalds
 $K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2004 Pavel Machek <pavel@ucw.cz>
 $K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2006 Bob Copeland <me@bobcopeland.com>
-$K/Documentation/translations/zh_CN/dev-tools/sparse.rst:﻿Copyright 2004 Linus Torvalds
```

#### `T-09` vs U0 (`KNOWN-SEMANTIC (BOM stripping)`)

Like ripgrep, ugrep strips the UTF-8 BOM at the start of `Documentation/translations/zh_CN/dev-tools/sparse.rst`; every other line is identical.

```diff
--- /dev/shm/kbench_out/ref_T-09_G0.sorted	2026-10-17 22:50:41.902197902 +0000
+++ /dev/shm/kbench_out/ref_T-09_U0.sorted	2026-10-17 23:32:48.884973281 +0000
@@ -780,9 +780,9 @@
 $K/Documentation/translations/sp_SP/process/kernel-enforcement-statement.rst:Aunque existe el derecho de hacer valer un copyright distinto en las
 $K/Documentation/translations/sp_SP/process/kernel-enforcement-statement.rst:nuestro y de cualquier sucesor de nuestros derechos de autor (copyright):
 $K/Documentation/translations/zh_CN/admin-guide/reporting-issues.rst:   way if you want to contribute changes to the text, but for copyright
+$K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2004 Linus Torvalds
 $K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2004 Pavel Machek <pavel@ucw.cz>
 $K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2006 Bob Copeland <me@bobcopeland.com>
-$K/Documentation/translations/zh_CN/dev-tools/sparse.rst:﻿Copyright 2004 Linus Torvalds
```

#### `T-19` vs U0 (`KNOWN-SEMANTIC (no group separator between files)`)

Every match and context line is identical. GNU grep also prints the `--` group separator between matches in different files (10 of its 16 separators); ugrep prints it only between groups in the same file.

```diff
--- /dev/shm/kbench_out/ref_T-19_G0.sorted	2026-10-17 22:31:53.992150827 +0000
+++ /dev/shm/kbench_out/ref_T-19_U0.sorted	2026-10-17 22:31:54.994135902 +0000
@@ -4,16 +4,6 @@
 --
 --
 --
---
---
---
---
```

#### `T-20` vs U0 (`KNOWN-SEMANTIC (no group separator between files)`)

Every match and context line is identical. GNU grep also prints the `--` group separator between matches in different files (3,422 of its 15,719 separators); ugrep prints it only between groups in the same file.

```diff
--- /dev/shm/kbench_out/ref_T-20_G0.sorted	2026-10-17 22:32:17.125558853 +0000
+++ /dev/shm/kbench_out/ref_T-20_U0.sorted	2026-10-17 22:32:18.387215236 +0000
@@ -12295,3428 +12295,6 @@
 --
 --
 --
---
---
---
---
```

#### `T-21` vs R0 (`SOFT_DIFF`)

```diff
--- /dev/shm/kbench_out/ref_T-21_G0.sorted	2026-10-17 22:32:43.930745076 +0000
+++ /dev/shm/kbench_out/ref_T-21_R0.sorted	2026-10-17 22:32:45.746919259 +0000
@@ -1 +1 @@
-SUM:32347857
+SUM:32247077
```

#### `T-21` vs U0 (`SOFT_DIFF`)

The counts differ in 118 of 95,932 files. In the 5 binary files GNU grep counts NUL-terminated records, because it treats NUL bytes as line ends in binary files, while ugrep counts newline-terminated lines. In the 113 files whose last line has no newline, `ugrep -v -c` reports one line fewer than the lines `ugrep -v` prints (a file holding just `a` gives 1 with GNU grep and 0 with ugrep 7.8.5).

```diff
--- /dev/shm/kbench_out/ref_T-21_G0.sorted	2026-10-17 22:32:43.930745076 +0000
+++ /dev/shm/kbench_out/ref_T-21_U0.sorted	2026-10-17 22:32:47.161470186 +0000
@@ -1 +1 @@
-SUM:32347857
+SUM:32247256
```

#### `T-26` vs U0 (`KNOWN-SEMANTIC (-b prints the match offset)`)

Every file, line number and line is identical. ugrep's `-b` prints the byte offset of the first match on the line, GNU grep's the offset of the line itself; they agree on the 14 lines where the match starts the line.

```diff
--- /dev/shm/kbench_out/ref_T-26_G0.sorted	2026-10-17 22:34:10.181491576 +0000
+++ /dev/shm/kbench_out/ref_T-26_U0.sorted	2026-10-17 22:34:11.262111525 +0000
@@ -1,15794 +1,15794 @@
 $K/Documentation/PCI/msi-howto.rst:202:8393:spin_lock_irqsave() or spin_lock_irq() which disable local interrupts
-$K/Documentation/RCU/Design/Memory-Ordering/Tree-RCU-Memory-Ordering.rst:53:2371:``raw_spin_lock_irq_rcu_node()``, and ``raw_spin_lock_irqsave_rcu_node()``.
-$K/Documentation/RCU/Design/Requirements/Requirements.rst:2081:100199:       raw_spin_lock_irqsave_rcu_node(rnp, flags);
-$K/Documentation/RCU/UP.rst:131:5630:	spin_lock_irqsave().
-$K/Documentation/RCU/listRCU.rst:469:16880:			spin_lock_irqsave(&ctx->wqh.lock, flags);
-$K/Documentation/RCU/whatisRCU.rst:1316:49662:			spin_lock_irqsave().
-$K/Documentation/core-api/dma-api.rst:762:27487:	[<ffffffff8064784f>] _spin_lock_irqsave+0x1f/0x50
```

## 3. Gate D: G1 Thread Invariance

G1 deterministic output verification across thread counts:

| Scenario | Description | Threads Checked | Identical Stdout SHA-256 | Gate D Verdict |
| :--- | :--- | :--- | :---: | :---: |
| `B-01` | rare literal ($BIG) | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `B-04` | DFA regex ($BIG) | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `M-02` | 1,000 literals, tree | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-01` | zero-match literal | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-04` | dense literal, line numbers | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-06` | files-with-matches | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-10` | word match | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-15` | literal-free regex | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-16` | PCRE2 lookaround, dense | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |

