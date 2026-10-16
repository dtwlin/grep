<h1 align="center">⚡ Supercharged grep</h1>

<p align="center">
  <i>"Your time is valuable — don't wait a millisecond longer than you have to."</i>
</p>

<p align="center">
  <b>GNU grep, multi-threaded and SIMD-accelerated.</b><br>
  Same flags. Same output, byte for byte. On average <b>21× faster</b> than GNU grep and <b>3–4× faster</b> than <a href="https://github.com/BurntSushi/ripgrep">ripgrep</a>.
</p>

<p align="center">
  <a href="https://github.com/dtwlin/grep/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/dtwlin/grep/actions/workflows/ci.yml/badge.svg"></a>
  <a href="COPYING"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-blue.svg"></a>
  <img alt="Drop-in replacement" src="https://img.shields.io/badge/GNU%20grep-drop--in%20replacement-brightgreen.svg">
  <img alt="Output" src="https://img.shields.io/badge/output-byte--identical-success.svg">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux%20x86--64%20%7C%20macOS%20%28experimental%29-blue.svg">
</p>

---

## 🚀 Why Supercharged grep?

- **🔁 100% drop-in.** It *is* GNU grep 3.12 with a new engine underneath — same options, same output, same exit codes. Install it and every script, tool, and agent that calls `grep` gets faster. No new flags, no aliases, no config.
- **🧵 Parallel by default.** Up to 32 threads search files, directories, and even a single huge file, yet results come out in exactly the order GNU grep prints them.
- **🏎️ SIMD search engines.** AVX2 literal scanners, a dense Aho-Corasick automaton for big keyword sets, and literal pre-filters for PCRE2, backreferences, and 1,000-way alternations.
- **⏱️ No startup tax.** Threads start lazily, so a one-file `grep` takes 2.6 ms — the same as GNU grep (2.7 ms; ripgrep: 3.8 ms).
- **✅ Verified identical.** `stdout`, `stderr`, and exit status are byte-identical to upstream GNU grep across 59 kernel-tree scenarios, 200 edge-case, boundary, and multithreaded checks, continuous stress testing, 6,881 directory-traversal comparisons, and randomized multi-pattern fuzzing, at 1 to 128 threads. It passes GNU grep's own test suite and runs clean under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.

## 🤖 Built for AI coding tools

AI coding agents lean on `grep` constantly: every "where is this defined?" or "who calls this?" becomes a shell call, usually across the whole repository. Supercharged grep makes those calls cheap:

| Benefit | Why it matters for agents |
| :-- | :-- |
| ⚡ **Repo-wide search in well under a second** | Linux kernel (96k files): ~0.16 s with 32 threads, ~0.4 s with 8, vs ~4 s for GNU grep. Shorter tool round-trips → faster agent loops. |
| 🧠 **Nothing to re-learn** | Models already know `grep -rn`, `-i`, `-c`, `-l`, `-F -f`, `-P`. No prompt changes, no custom tool definitions, no `rg` flag translation. |
| 🔒 **Deterministic output** | Same bytes in the same order at any thread count → stable context windows and reproducible runs. |
| 🪶 **Same latency for small lookups** | A tiny single-file `grep` costs what it cost before (2.6 ms); the thread pool only starts when there is enough work. |
| 📚 **Big wins where agents hurt most** | Recursive, case-insensitive, counting, symbol-list (`-F -f`), and PCRE2 searches are 22–124× faster than GNU grep. |

### 🧩 Suggested agent skill

Many agents reach for `rg`, `git grep`, or `find | xargs grep` because they assume `grep` is slow. The skill below tells them to check for Supercharged grep and, when it is installed, to search with plain `grep` instead. It follows the [Agent Skills](https://agentskills.io) `SKILL.md` format: save it as `supercharged-grep/SKILL.md` in your agent's skills directory (for example `~/.claude/skills/` for Claude Code). For agents that read an instructions file such as `AGENTS.md` instead, paste in everything below the front matter.

<details>
<summary><b>SKILL.md</b></summary>

````markdown
---
name: supercharged-grep
description: Use for any search of file contents (finding definitions, callers, usages, TODOs, config keys, or log lines). When the installed grep is Supercharged grep, search with plain `grep` instead of ripgrep (`rg`), `ag`, `ack`, `git grep`, or `find | xargs grep`.
---

# Search with Supercharged grep

## Check once per session

```bash
grep --version 2>/dev/null | tail -n 1
```

If the line starts with `Supercharged grep`, the `grep` on `PATH` is a
multi-threaded, SIMD-accelerated drop-in replacement for GNU grep
(https://github.com/dtwlin/grep). Follow the rules below. Otherwise, ignore
this skill and search as you normally would.

## Rules

- Use `grep` for every content search, including whole-repository searches.
  Do not switch to `rg`, `ag`, `ack`, or `git grep` for speed: `grep -r` is
  as fast or faster, and its output is byte-identical to GNU grep's.
- If a shell is available, prefer running `grep` over a built-in search tool
  that wraps another engine.
- Use the GNU grep options you already know; there are no new flags. Write
  `grep -E` and `grep -F`, not `egrep` or `fgrep`, which print an
  obsolescence warning.
- Let `grep -r` walk directories itself. It searches files in parallel;
  `find ... | xargs grep` or `find -exec grep` only adds processes.
- `grep -r` does not read `.gitignore`. Skip generated and vendored trees
  explicitly, for example `--exclude-dir=.git --exclude-dir=node_modules`,
  and add `-I` to skip binary files.
- Leave `GREP_THREADS` unset; the default (up to 32 threads) is fastest. Set
  a lower value such as `GREP_THREADS=4` only on a shared or busy machine.
- Output order is deterministic at any thread count (the same order as GNU
  grep), so results from repeated runs can be compared directly.

## Recipes

| Task | Command |
| :-- | :-- |
| Find a symbol | `grep -rnw -I --exclude-dir=.git 'parse_config' .` |
| Literal text that contains regex characters | `grep -rnF 'a[i].b(' src/` |
| Case-insensitive search | `grep -rni 'todo' src/` |
| Only some file types | `grep -rn --include='*.py' 'import requests' .` |
| Files that match | `grep -rl 'EXPORT_SYMBOL' .` |
| Files that do not match | `grep -rL --include='*.c' 'SPDX-License-Identifier' .` |
| Count matches per file | `grep -rc 'malloc' --include='*.c' .` |
| Many literals in one pass | `grep -rnF -f symbols.txt .` (faster than one search per pattern) |
| Lookaround, `\d`, lazy quantifiers | `grep -rnP '(?<=def )\w+' .` |
| Surrounding context | `grep -rn -C 3 'panic(' .` |
| At most N matches per file | `grep -rn -m 5 'FIXME' .` |
| Just check whether anything matches | `grep -rq 'deprecated_api' . && echo found` |
````

</details>

## 📊 Benchmarks

Setup: the Linux kernel source tree (96,034 files, 1.64 GB) on an AMD EPYC server, warm page cache unless noted, `LC_ALL=en_US.UTF-8`, median of 3 runs after a warm-up. GNU grep 3.12 is single-threaded; Supercharged grep runs at its default of 32 threads; [ripgrep](https://github.com/BurntSushi/ripgrep) 15.1 is shown at its default of 12 threads and with `-j 32`. Tree searches skip `.git` (`grep -r --exclude-dir=.git`, `rg -uu -g '!.git'`), so every tool searches the same files.

| Workload | GNU grep | ripgrep | ripgrep<br>`-j 32` | ⚡ Supercharged grep | vs GNU grep | vs ripgrep¹ |
| :-- | --: | --: | --: | --: | --: | --: |
| Dense literal: `grep -rn 'static int'` | 4.53 s | 509 ms | 548 ms | **162 ms** | 28× | 3.2× |
| Case-insensitive: `grep -ri copyright` | 5.10 s | 779 ms | 1.01 s | **156 ms** | 33× | 5.0× |
| Count per file: `grep -rc '#include <linux/'`² | 3.67 s | 1.11 s | 1.54 s | **165 ms** | 22× | 6.7× |
| 1,000 fixed strings: `grep -rF -f sym_1000.txt` | 14.0 s | 718 ms | 352 ms | **173 ms** | 81× | 2.0× |
| PCRE2 lookaround: `grep -rP '(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)'` | 27.6 s | 2.51 s | 1.14 s | **223 ms** | 124× | 5.1× |
| Backreference: `grep -rE '\b([a-z]{4,})_\1\b'`³ | 41.6 s | 842 ms | 357 ms | **267 ms** | 156× | 1.34× |
| 1.4 GB file: `grep -F kmalloc_array_node linux-all.c` | 498 ms | 105 ms | 109 ms | **21.8 ms** | 23× | 4.8× |
| 1.4 GB file: `grep -ni copyright linux-all.c` | 1.97 s | 240 ms | 267 ms | **44.1 ms** | 45× | 5.4× |
| One small file: `grep -n start_kernel init/main.c` | 2.7 ms | 3.8 ms | 3.7 ms | **2.6 ms** | 1.0× | 1.4× |
| Whole tree, cold cache⁴ | 53.5 s | 4.96 s | — | **2.07 s** | 26× | 2.4× |

<sub>¹ Against the faster of the two ripgrep runs. ² ripgrep needs `--include-zero` to print the `file:0` lines that `grep -c` prints. ³ ripgrep needs `-P` (PCRE2) for backreferences. ⁴ Zero-match literal after evicting the tree's file data from the page cache with `posix_fadvise` (directory metadata stays cached); `rg -j 32` was not measured cold.</sub>

**Across all 59 scenarios (geometric mean): 21.17× faster than GNU grep, 3.63× faster than ripgrep at its defaults, and 2.68× faster than `rg -j 32`.** Supercharged grep's output was byte-identical to GNU grep's in every scenario.

### Is it a fair fight?

- 🧮 **Same thread count:** with both tools on 12 threads (`GREP_THREADS=12` vs ripgrep's default of 12), Supercharged grep is still **1.90× faster** (geometric mean of 40 scenarios: 40 wins, 0 losses).
- 🙈 **ripgrep's default `.gitignore` mode** (without `-uu`) does not close the gap: 391 ms (234 ms with `-j 32`) vs **155 ms** for `grep -rn --exclude-dir=.git spin_lock_irqsave`.
- 💻 **Fewer cores:** capped at 8 threads (`GREP_THREADS=8`), it is still 9–11× faster than GNU grep on whole-tree scans and 30× faster with 1,000 patterns.

📄 Full matrix, thread-scaling curves (1–128 threads), locale and cold-cache analysis: [`BENCHMARK_RESULTS.md`](BENCHMARK_RESULTS.md). Reproduce with [`benchmarks/kernel/`](benchmarks/kernel/README.md).

## 🔧 What we changed

Everything below lives behind GNU grep's unchanged command-line interface.

### 🧵 Parallelism without changing a single byte of output

- **Worker pool + Sequence Reorder Buffer** — files are searched concurrently; output is committed in GNU grep's exact order, including diagnostics on `stderr` and the exit status.
- **Parallel directory walker** that reproduces GNU grep's `fts` traversal order and all of its semantics: `-r`/`-R` symlink rules, `--include`/`--exclude`/`--exclude-dir`, loop detection, unreadable entries, `-l`/`-L`/`-q`/`-m`, and the "input file is also the output" check.
- **Intra-file chunking** — one multi-GB file is split across threads; pipes and `stdin` are sliced and searched in parallel too.
- **Lazy start-up** — the thread pool is created only when there is enough work, so small invocations stay as fast as GNU grep.

### 🏎️ Faster search engines

- **AVX2 kernels** for newline/NUL scanning, a packed-pair literal scanner, and a [Teddy](https://github.com/BurntSushi/aho-corasick/blob/master/src/packed/teddy/README.md)-style multi-literal pre-filter.
- **Dense Aho-Corasick automaton with q-gram sampling** for large `-F -f` keyword sets (hundreds to tens of thousands of patterns).
- **Literal pre-filters** for PCRE2 (`-P`), backreferences, and 1,000-way `|` alternations, so the slow engine only sees candidate lines.
- **ASCII fast path for UTF-8 `-i`** — case-insensitive search in UTF-8 locales runs at nearly C-locale speed.

### 💾 I/O

- Per-worker I/O buffers, `mmap` for large files, and many reads in flight at once — which is why cold-cache tree scans finish 26× sooner than with GNU grep.

## 📦 Install

Requirements: a C11 compiler with `<stdatomic.h>` and POSIX threads (any recent GCC or Clang), the GNU autotools, and optionally PCRE2 for `-P`.

### 🐧 Linux (x86-64)

1. Install the build dependencies (Debian/Ubuntu shown; adjust for your distribution):

```bash
sudo apt install build-essential git autoconf automake autopoint gettext gperf \
  texinfo pkg-config wget xz-utils libpcre2-dev
```

2. Clone, build, and install:

```bash
git clone --recurse-submodules https://github.com/dtwlin/grep.git
cd grep
./bootstrap --skip-po
./configure CFLAGS='-O3 -march=native -flto=auto -fno-plt -fomit-frame-pointer -falign-functions=32 -falign-loops=32'
make -j"$(nproc)"
make check                # optional (~2 min): GNU grep test suite + parallel tests
sudo make install         # installs /usr/local/bin/grep, ahead of /usr/bin/grep on most PATHs
hash -r && grep --version # last line should start with "Supercharged grep"
```

`-march=native` enables the AVX2 kernels on CPUs that support them (`-mavx2` works too), `-flto=auto` enables link-time optimization across translation units, and `-falign-functions=32 -falign-loops=32` aligns hot inner loops to 32-byte CPU fetch boundaries. Omit `-march=native` when building a portable binary for older x86-64 machines.

### 🍎 macOS (experimental)

Requires the Xcode Command Line Tools (`xcode-select --install`) and [Homebrew](https://brew.sh).

1. Install the build dependencies and put Homebrew's keg-only tools (`texinfo` and `gettext`) and Autoconf macros on your search paths:

```bash
brew install autoconf automake gettext gperf texinfo pkg-config wget xz pcre2
export PATH="$(brew --prefix texinfo)/bin:$(brew --prefix gettext)/bin:$PATH"
export ACLOCAL_PATH="$(brew --prefix)/share/aclocal"
```

2. Clone, build, and install:

```bash
git clone --recurse-submodules https://github.com/dtwlin/grep.git
cd grep
./bootstrap --skip-po
./configure CFLAGS='-O3'
make -j"$(sysctl -n hw.ncpu)"
make check                # optional: test suite
sudo make install         # installs /usr/local/bin/grep
hash -r && grep --version # last line should start with "Supercharged grep"
```

Apple Silicon uses the portable 64-bit SWAR (SIMD within a register) kernels with `CFLAGS='-O3'`. On Intel Macs with AVX2, pass `CFLAGS='-O3 -march=native'` to `./configure`.

### 🛠️ Install options

Use these options to customize or remove the installation:

- **Install without `sudo`:** pass `--prefix="$HOME/.local"` to `./configure`, run `make install`, and ensure `~/.local/bin` comes before `/usr/bin` in `PATH`.
- **Uninstall:** run `sudo make uninstall` (or `make uninstall` for a `~/.local` install) from the build directory to restore your system `grep`.

## ⚙️ Tuning

Supercharged grep uses up to 32 threads (bounded by the CPUs available to the process). One environment variable overrides it:

```bash
GREP_THREADS=8 grep -rn pattern .   # use 8 threads
GREP_THREADS=1 grep -rn pattern .   # single-threaded
```

Changing `GREP_THREADS` only affects speed — the output and line order are always identical.

## ⚠️ Honest notes

- **CPU and memory for time.** Parallel runs burn more CPU-seconds to finish sooner, and a whole-tree search peaks at ~35–157 MB of resident memory (GNU grep and ripgrep: ≤ 40 MB). Lower `GREP_THREADS` on shared machines.
- **Thread-matched ripgrep** (`rg -j 32`) wins 3 of the 59 scenarios — one small sub-directory scan (`T-28`, 14.0 ms vs 15.9 ms), one naive invocation where `grep` scans the 6 GB `.git` directory while `rg` skips `.git` (`U-02`, 220 ms vs 313 ms), and one dot-star regex scan (`P-03`, 159 ms vs 163 ms). Supercharged grep wins 55, often by 2–100×; 1 is a tie (`L-00`).
- **Piped input is the one place GNU grep itself is not deterministic.** When data read from a pipe contains NUL bytes or encoding errors, GNU grep's output depends on how much each `read` returned. Supercharged grep is deterministic there and matches GNU grep fed by a fast writer; counts, file lists, and exit codes (`-c`, `-l`, `-L`, `-q`) always match.
- **Linux first; macOS experimental.** Every number above comes from Linux x86-64 with AVX2. The non-Linux code paths compile warning-free, but the fork has not yet been built or tested on a Mac — if you try, please report the result. Without AVX2 (including Apple Silicon) the portable 64-bit "SIMD within a register" kernels are used, and the speedups will be smaller.
- **You own updates.** `/usr/local/bin/grep` shadows your distribution's grep, so upstream GNU grep fixes reach you only when you rebuild this fork; `sudo make uninstall` restores the distribution binary.

## 🙏 Credits & license

Supercharged grep is a modified version of [GNU grep](https://www.gnu.org/software/grep/), forked from upstream commit [`e187ca5`](https://git.savannah.gnu.org/cgit/grep.git/commit/?id=e187ca53198c7aafb28da64da86d1bd59244c3cc) (shortly after 3.12) and changed in 2026; the git history records every change. It keeps GNU grep's license, [GPLv3 or later](COPYING), and upstream's documentation (`README`, `NEWS`, `doc/`) still applies because the user-facing behaviour is unchanged.

Found a case where the output differs from GNU grep? Re-run the command with `GREP_THREADS=1`, then open a [GitHub issue](https://github.com/dtwlin/grep/issues) — or email <dtwlin@gmail.com>, the address `grep --help` prints — with the command, the output of `grep --version`, and the input if you can share it. Please do not report problems with this fork to the GNU grep maintainers. Benchmarks compare against [ripgrep](https://github.com/BurntSushi/ripgrep) 15.1.0, an excellent tool that inspired several techniques used here.

## 🤝 Acknowledgement

Supercharged grep was created by Argon using [Antigravity](https://antigravity.google/).

