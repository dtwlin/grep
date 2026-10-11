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
| `B-13` | FIFO pipe (cat $BIG \|) | 0 | 0 | `a4ee27abab3ee2db...` | `a4ee27abab3ee2db...` | **PASS** |
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
| `X-01` | git ls-files \| xargs | 123 | 123 | `e8535c34e7434929...` | `e8535c34e7434929...` | **PASS** |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | 0 | 0 | `1ac62cb0ce208e32...` | `1ac62cb0ce208e32...` | **PASS** |
| `P-02` | 1,000-way regex alternation | 0 | 0 | `236038c5dd5034ab...` | `236038c5dd5034ab...` | **PASS** |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | 0 | 0 | `0a219b2dfbf3747f...` | `0a219b2dfbf3747f...` | **PASS** |

## 2. Gate B: Cross-Tool Normalized Match Set Equivalence

| Scenario | Description | R0 Match vs G0 | G2 Match vs G0 | X1 Match vs G0 | Notes |
| :--- | :--- | :---: | :---: | :---: | :--- |
| `B-01` | rare literal ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-02` | count dense ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-03` | moderate, line numbers ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-04` | DFA regex ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-05` | case-insensitive dense ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-06` | word match ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-07` | literal-free regex ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-08` | PCRE2 lookaround ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-09` | context ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-10` | only-matching ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-11` | inverted count ($BIG) | PASS | N/A | N/A | Sorted match |
| `B-12` | stdin redirect (< $BIG) | PASS | N/A | N/A | Sorted match |
| `B-13` | FIFO pipe (cat $BIG \|) | PASS | N/A | N/A | Sorted match |
| `B-14` | real 24 MB header (big-24M.h) | PASS | N/A | N/A | Sorted match |
| `L-00` | harness floor (/bin/true) | PASS | N/A | N/A | Exit code 0 vs 0 |
| `L-01` | one small file (init/main.c) | PASS | N/A | N/A | Sorted match |
| `L-02` | tiny tree (kernel/sched) | PASS | N/A | N/A | Sorted match |
| `L-03` | quiet first match in big file | PASS | N/A | N/A | Exit code 0 vs 0 |
| `M-01` | 100 literals, tree | PASS | N/A | N/A | Sorted match |
| `M-02` | 1,000 literals, tree | PASS | N/A | N/A | Sorted match |
| `M-03` | 10,000 literals, tree | PASS | N/A | N/A | Sorted match |
| `M-04` | 1,000 literals, -i | PASS | N/A | N/A | Sorted match |
| `M-05` | 1,000 literals, -w | PASS | N/A | N/A | Sorted match |
| `M-06` | 1,000 literals, $BIG | PASS | N/A | N/A | Sorted match |
| `T-01` | zero-match literal | PASS | N/A | N/A | Exit code 1 vs 1 |
| `T-02` | rare literal | PASS | N/A | N/A | Sorted match |
| `T-03` | moderate literal, line numbers | PASS | N/A | N/A | Sorted match |
| `T-04` | dense literal, line numbers | PASS | N/A | N/A | Sorted match |
| `T-05` | very dense literal | PASS | N/A | N/A | Sorted match |
| `T-06` | files-with-matches | PASS | N/A | N/A | Sorted match |
| `T-07` | files-without-match | SOFT_DIFF | N/A | N/A | Diff / binary formatting |
| `T-08` | count per file | PASS | N/A | N/A | Sorted match |
| `T-09` | case-insensitive literal | KNOWN-SEMANTIC | N/A | N/A | KNOWN-SEMANTIC (BOM stripping) |
| `T-10` | word match | PASS | N/A | N/A | Sorted match |
| `T-11` | DFA regex with must-literal | PASS | N/A | N/A | Sorted match |
| `T-12` | anchored dense regex | PASS | N/A | N/A | Sorted match |
| `T-13` | literal alternation + word boundary | PASS | N/A | N/A | Sorted match |
| `T-14` | optional group + alternation | PASS | N/A | N/A | Sorted match |
| `T-15` | literal-free regex | PASS | N/A | N/A | Sorted match |
| `T-16` | PCRE2 lookaround, dense | PASS | N/A | N/A | Sorted match |
| `T-17` | PCRE2 rare with literal | PASS | N/A | N/A | Sorted match |
| `T-18` | only-matching | PASS | N/A | N/A | Sorted match |
| `T-19` | context, rare | PASS | N/A | N/A | Sorted match |
| `T-20` | context, moderate | PASS | N/A | N/A | Sorted match |
| `T-21` | inverted count | SOFT_DIFF | N/A | N/A | Diff / binary formatting |
| `T-22` | glob include | PASS | N/A | N/A | Sorted match |
| `T-23` | glob + sub-scope | PASS | N/A | N/A | Sorted match |
| `T-24` | max-count per file | PASS | N/A | N/A | Sorted match |
| `T-25a` | quiet, no match | PASS | N/A | N/A | Exit code 1 vs 1 |
| `T-25b` | quiet, immediate match | PASS | N/A | N/A | Exit code 0 vs 0 |
| `T-26` | byte offsets | PASS | N/A | N/A | Sorted match |
| `T-27` | sub-scope drivers/ | PASS | N/A | N/A | Sorted match |
| `T-28` | sub-scope kernel mm fs | PASS | N/A | N/A | Sorted match |
| `U-01` | out-of-the-box UX | PASS | N/A | N/A | Sorted match |
| `U-02` | naive invocation (scans 6 GB .git) | N/A | N/A | N/A | Intentionally divergent |
| `X-01` | git ls-files \| xargs | PASS | N/A | N/A | Sorted match |
| `P-01` | backreference (\b([a-z]{4,})_\1\b) | PASS | N/A | N/A | Sorted match |
| `P-02` | 1,000-way regex alternation | PASS | N/A | N/A | Sorted match |
| `P-03` | dot-star heavy (.*_lock.*_unlock) | PASS | N/A | N/A | Sorted match |

### Diff Samples & Semantic Classifications

#### `T-07` vs R0 (`SOFT_DIFF`)

```diff
--- /dev/shm/kbench_out/ref_T-07_G0.sorted	2026-10-11 16:39:39.052395119 +0000
+++ /dev/shm/kbench_out/ref_T-07_R0.sorted	2026-10-11 16:39:39.760552170 +0000
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
--- /dev/shm/kbench_out/ref_T-09_G0.sorted	2026-10-11 16:40:25.774230820 +0000
+++ /dev/shm/kbench_out/ref_T-09_R0.sorted	2026-10-11 16:40:27.126229210 +0000
@@ -780,9 +780,9 @@
 $K/Documentation/translations/sp_SP/process/kernel-enforcement-statement.rst:Aunque existe el derecho de hacer valer un copyright distinto en las
 $K/Documentation/translations/sp_SP/process/kernel-enforcement-statement.rst:nuestro y de cualquier sucesor de nuestros derechos de autor (copyright):
 $K/Documentation/translations/zh_CN/admin-guide/reporting-issues.rst:   way if you want to contribute changes to the text, but for copyright
+$K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2004 Linus Torvalds
 $K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2004 Pavel Machek <pavel@ucw.cz>
 $K/Documentation/translations/zh_CN/dev-tools/sparse.rst:Copyright 2006 Bob Copeland <me@bobcopeland.com>
-$K/Documentation/translations/zh_CN/dev-tools/sparse.rst:﻿Copyright 2004 Linus Torvalds
```

#### `T-21` vs R0 (`SOFT_DIFF`)

```diff
--- /dev/shm/kbench_out/ref_T-21_G0.sorted	2026-10-11 16:46:43.738583038 +0000
+++ /dev/shm/kbench_out/ref_T-21_R0.sorted	2026-10-11 16:46:45.654464255 +0000
@@ -1 +1 @@
-SUM:32347857
+SUM:32247077
```

## 3. Gate D: G1 Thread Invariance

G1 deterministic output verification across thread counts:

| Scenario | Description | Threads Checked | Identical Stdout SHA-256 | Gate D Verdict |
| :--- | :--- | :--- | :---: | :---: |

