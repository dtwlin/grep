<h1 align="center">⚡ Supercharged grep</h1>

<p align="center">
  <b>GNU grep, multi-threaded and SIMD-accelerated.</b><br>
  Same flags. Same output, byte for byte. On average <b>17× faster</b> than GNU grep and <b>2–3× faster</b> than <a href="https://github.com/BurntSushi/ripgrep">ripgrep</a>.
</p>

<p align="center">
  <a href="COPYING"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-blue.svg"></a>
  <img alt="Drop-in replacement" src="https://img.shields.io/badge/GNU%20grep-drop--in%20replacement-brightgreen.svg">
  <img alt="Output" src="https://img.shields.io/badge/output-byte--identical-success.svg">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-blue.svg">
</p>

---

## 🚀 Why Supercharged grep?

- **🔁 100% drop-in.** It *is* GNU grep 3.12 with a new engine underneath — same options, same output, same exit codes. Install it and every script, tool and agent that calls `grep` gets faster. No new flags, no aliases, no config.
- **🧵 Parallel by default.** Files, directories and even a single huge file are searched by up to 32 threads, yet results come out in exactly the order GNU grep prints them.
- **🏎️ SIMD search engines.** AVX2 literal scanners, a dense Aho-Corasick automaton for big keyword sets, and literal pre-filters for PCRE, backreferences and giant alternations.
- **⏱️ No startup tax.** Threads start lazily, so a one-file `grep` takes 2.7 ms — the same as GNU grep (ripgrep: 3.8 ms).
- **✅ Verified identical.** Byte-identical `stdout`, `stderr` and exit status versus upstream GNU grep in 59 kernel-tree scenarios, 53 edge-case checks, 6,433 directory-traversal comparisons and randomized multi-pattern fuzzing, at 1 to 128 threads. Passes GNU grep's own test suite; clean under ASan, UBSan and TSan.

## 🤖 Built for AI coding tools

AI coding agents lean on `grep` all the time: every "where is this defined?" or "who calls this?" becomes a shell call, usually across the whole repository. Supercharged grep makes those calls cheap:

| | Why it matters for agents |
| :-- | :-- |
| ⚡ **Repo-wide search in well under a second** | Linux kernel (96k files): ~0.2 s with 32 threads, ~0.4 s with 8, vs ~4 s for GNU grep. Shorter tool round-trips → faster agent loops. |
| 🧠 **Nothing to re-learn** | Models already know `grep -rn`, `-i`, `-c`, `-l`, `-F -f`, `-P`. No prompt changes, no custom tool definitions, no `rg` flag translation. |
| 🔒 **Deterministic output** | Same bytes in the same order at any thread count → stable context windows and reproducible runs. |
| 🪶 **No latency regression** | Tiny single-file lookups cost exactly what they cost before. |
| 📚 **Big wins where agents hurt most** | Recursive, case-insensitive, counting and symbol-list (`-F -f`) searches are 16–63× faster. |

## 📊 Benchmarks

Linux kernel source tree (96,034 files, 1.64 GB) on a 128-CPU AMD EPYC server; warm page cache unless noted, `LC_ALL=en_US.UTF-8`, median of 3 runs after a warm-up. GNU grep 3.12 is single-threaded and Supercharged grep runs at its default (32 threads); [ripgrep](https://github.com/BurntSushi/ripgrep) 15.1 is shown at its default (12 threads) and with `-j 32`. Tree searches skip `.git` (`grep -r --exclude-dir=.git`, `rg -uu -g '!.git'`), so every tool searches the same files.

| Workload | GNU grep | ripgrep | ripgrep<br>`-j 32` | ⚡ Supercharged grep | vs GNU | vs ripgrep¹ |
| :-- | --: | --: | --: | --: | --: | --: |
| Dense literal: `grep -rn 'static int'` | 4.53 s | 509 ms | 548 ms | **284 ms** | 16× | 1.8× |
| Case-insensitive: `grep -ri copyright` | 5.10 s | 779 ms | 1.01 s | **208 ms** | 25× | 3.8× |
| Count per file: `grep -rc '#include <linux/'`² | 3.67 s | 1.11 s | 1.54 s | **205 ms** | 18× | 5.4× |
| 1,000 fixed strings: `grep -rF -f sym_1000.txt` | 14.0 s | 718 ms | 352 ms | **223 ms** | 63× | 1.6× |
| PCRE lookaround: `grep -rP '(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)'` | 27.6 s | 2.51 s | 1.14 s | **977 ms** | 28× | 1.2× |
| Backreference: `grep -rE '\b([a-z]{4,})_\1\b'`³ | 41.6 s | 842 ms | 357 ms | **342 ms** | 122× | 1.04× |
| 1.4 GB file: `grep -F kmalloc_array_node linux-all.c` | 498 ms | 105 ms | 109 ms | **19 ms** | 27× | 5.6× |
| 1.4 GB file: `grep -ni copyright linux-all.c` | 1.97 s | 240 ms | 267 ms | **48 ms** | 41× | 5.1× |
| One small file: `grep -n start_kernel init/main.c` | 2.7 ms | 3.8 ms | 3.7 ms | **2.7 ms** | 1.0× | 1.4× |
| Whole tree, cold cache⁴ | 53.5 s | 4.96 s | — | **2.15 s** | 25× | 2.3× |

<sub>¹ Against the faster of the two ripgrep runs. ² ripgrep needs `--include-zero` to print the `file:0` lines that `grep -c` prints. ³ ripgrep needs `-P` (PCRE2) for backreferences. ⁴ Zero-match literal after evicting the tree's file data from the page cache with `posix_fadvise` (directory metadata stays cached); `rg -j 32` was not measured cold.</sub>

**Across all 59 scenarios (geometric mean): 16.97× faster than GNU grep, 2.91× faster than ripgrep at its defaults, and 2.14× faster than `rg -j 32`.** Supercharged grep's output was byte-identical to GNU grep's in every scenario.

Is it a fair fight?

- 🧮 **Same thread count:** with both tools on 12 threads (`GREP_THREADS=12` vs ripgrep's default of 12), Supercharged grep is still **1.73× faster** (geometric mean of 40 scenarios: 39 wins, 1 loss).
- 🙈 **ripgrep's own `.gitignore` mode** (no `-uu`) is no faster: 391 ms (234 ms with `-j 32`) vs **193 ms** for `grep -rn --exclude-dir=.git spin_lock_irqsave`.
- 💻 **Fewer cores:** capped at 8 threads (`GREP_THREADS=8`), it is still 8–10× faster than GNU grep on whole-tree scans and 29× faster with 1,000 patterns.

📄 Full matrix, thread-scaling curves (1–128 threads), locale and cold-cache analysis: [`BENCHMARK_RESULTS.md`](BENCHMARK_RESULTS.md). Reproduce with [`benchmarks/kernel/`](benchmarks/kernel/README.md).

## 🔧 What we changed

Everything below lives behind GNU grep's unchanged command-line interface.

**🧵 Parallelism without changing a single byte of output**
- **Worker pool + Sequence Reorder Buffer** — files are searched concurrently; output is committed in GNU grep's exact order, including diagnostics on `stderr` and the exit status.
- **Parallel directory walker** that reproduces `fts` traversal order and all of grep's semantics: `-r`/`-R` symlink rules, `--include`/`--exclude`/`--exclude-dir`, loop detection, unreadable entries, `-l`/`-L`/`-q`/`-m`, and the "input file is also the output" check.
- **Intra-file chunking** — one multi-GB file is split across threads; pipes and `stdin` are sliced and searched in parallel too.
- **Lazy start-up** — the thread pool is created only when there is enough work, so small invocations stay as fast as stock grep.

**🏎️ Faster search engines**
- **AVX2 kernels** for newline/NUL scanning, a packed-pair literal scanner, and a Teddy-style multi-literal pre-filter.
- **Dense Aho-Corasick automaton with q-gram sampling** for large `-F -f` keyword sets (hundreds to tens of thousands of patterns).
- **Literal pre-filters** for PCRE2 (`-P`), backreferences, and 1,000-way `|` alternations, so the slow engine only sees candidate lines.
- **ASCII fast path for UTF-8 `-i`** — case-insensitive search in UTF-8 locales runs at nearly C-locale speed.

**💾 I/O**
- Per-worker I/O buffers, `mmap` for large files, and many reads in flight at once — which is why cold-cache tree scans finish 25× sooner than with GNU grep.

## 📦 Install

```bash
# Debian/Ubuntu build prerequisites (adjust for your distro)
sudo apt install build-essential git autoconf automake autopoint gettext gperf \
  texinfo pkg-config wget xz-utils libpcre2-dev

# macOS (Homebrew)
brew install autoconf automake gettext gperf texinfo pkg-config xz pcre2

git clone --recurse-submodules https://github.com/dtwlin/grep.git
cd grep
./bootstrap --skip-po
./configure CFLAGS='-O3 -march=native'   # or add --prefix="$HOME/.local" to install without sudo
make -j"$(nproc)"
make check                               # optional: GNU grep's test suite + the parallel tests
sudo make install                        # installs /usr/local/bin/grep, ahead of /usr/bin/grep on most PATHs
hash -r && grep --version                # look for: Supercharged grep (up to 32 threads, AVX2)
```

`-march=native` enables the AVX2 kernels on CPUs that have them; leave it out for a portable binary that uses the plain C fallbacks. Changed your mind? `sudo make uninstall` brings back your system grep.

## ⚙️ Tuning

Supercharged grep uses up to 32 threads (bounded by the CPUs available to the process). One environment variable overrides it:

```bash
GREP_THREADS=8 grep -rn pattern .   # use 8 threads
GREP_THREADS=1 grep -rn pattern .   # single-threaded
```

Changing `GREP_THREADS` only affects speed — the output and line order are always identical.

## ⚠️ Honest notes

- **CPU and memory for time.** Parallel runs burn more CPU-seconds to finish sooner, and a whole-tree search peaks at ~250–360 MB of RSS (GNU grep and ripgrep: ≤ 40 MB). Turn it down with `GREP_THREADS` on shared machines.
- **One known latency regression:** `grep -rq` with a match in the very first files of a tree takes ~10 ms instead of ~3 ms.
- **Thread-matched ripgrep** (`rg -j 32`) is faster on 20 of the 59 scenarios: mostly sparse whole-tree scans (by 3–26%), plus two sub-20 ms runs and one `.*`-heavy regex (by up to ~50%). Supercharged grep wins 37, often by 2–80×; 2 are ties.
- **Linux & macOS.** Builds cleanly on Linux and macOS. Vectorized AVX2 search kernels run on x86-64 processors; systems without AVX2 (including Apple Silicon ARM64) use the portable SWAR and 64-bit word fallbacks.

## 🙏 Credits & license

Supercharged grep is a modified version of [GNU grep](https://www.gnu.org/software/grep/), forked from upstream commit `e187ca5` (shortly after 3.12) and changed in 2026; the git history records every change. It keeps GNU grep's license, [GPLv3 or later](COPYING), and upstream's documentation (`README`, `NEWS`, `doc/`) still applies because the user-facing behaviour is unchanged. Please report problems with this fork in its [GitHub issues](https://github.com/dtwlin/grep/issues), not to the GNU grep maintainers. Benchmarks compare against [ripgrep](https://github.com/BurntSushi/ripgrep) 15.1.0, an excellent tool that inspired several techniques used here.
