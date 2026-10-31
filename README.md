<h1 align="center">⚡ Supercharged grep</h1>

<p align="center">
  <i>"Your time is valuable — don't wait a millisecond longer than you have to."</i>
</p>

<p align="center">
  <b>GNU grep, multi-threaded and SIMD-accelerated.</b><br>
  Same flags. Same output, byte for byte. On average <b>22× faster</b> than GNU grep.<br>
  And it beats <a href="https://github.com/BurntSushi/ripgrep">ripgrep</a> and <a href="https://github.com/Genivia/ugrep">ugrep</a> too: <b>about 4× faster</b> than both at their defaults.
</p>

<p align="center">
  <a href="https://github.com/dtwlin/grep/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/dtwlin/grep/actions/workflows/ci.yml/badge.svg"></a>
  <a href="COPYING"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-blue.svg"></a>
  <img alt="Drop-in replacement" src="https://img.shields.io/badge/GNU%20grep-drop--in%20replacement-brightgreen.svg">
  <img alt="Output" src="https://img.shields.io/badge/output-byte--identical-success.svg">
  <a href="#-benchmarks"><img alt="Speed" src="https://img.shields.io/badge/speed-22%C3%97%20GNU%20grep%20%7C%20~4%C3%97%20ripgrep%20%26%20ugrep-brightgreen.svg"></a>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux%20x86--64%20%7C%20macOS%20%28experimental%29-blue.svg">
</p>

---

## ⚡ At a glance

| Everyday search | ⚡ Supercharged grep | ripgrep | ugrep | 🚀 Speedup (vs rg / ugrep) |
| :-- | --: | --: | --: | :-: |
| 🔎 Search a repo: `grep -rn 'static int' .` | **158 ms** | 505 ms | 627 ms | **3.2× / 4.0×** |
| 🔡 Ignore case: `grep -ri copyright .` | **161 ms** | 724 ms | 785 ms | **4.5× / 4.9×** |
| 📂 List matching files: `grep -rl EXPORT_SYMBOL_GPL .` | **152 ms** | 220 ms | 461 ms | **1.4× / 3.0×** |
| 🔢 Count per file: `grep -rc '#include <linux/' .` | **151 ms** | 1.07 s | 1.12 s | **7.1× / 7.4×** |
| 📄 One 1.4 GB file: `grep -F kmalloc_array_node linux-all.c` | **19.7 ms** | 171 ms | 352 ms | **8.7× / 18×** |

<sub>Linux kernel source tree (96,034 files, 1.64 GB), warm cache, 128-CPU server. ripgrep and ugrep get the faster of their default (12 threads) and 32-thread runs. Exact flags and the full 59-scenario matrix are in [📊 Benchmarks](#-benchmarks).</sub>

## 🚀 Why Supercharged grep?

- **🔁 100% drop-in.** It *is* GNU grep 3.12 with a new engine underneath — same options, same output, same exit codes. Install it and every script, tool, and agent that calls `grep` gets faster. No new flags, no aliases, no config.
- **🏆 Beats ripgrep and ugrep, too.** Against both at their defaults, it wins all 58 search workloads in our Linux kernel benchmark and is 3.9× faster than ripgrep and 4.4× faster than ugrep on average. With all three on the same 12 threads, it is still 2.0× and 2.3× faster on average.
- **🧵 Parallel by default.** Up to 32 threads search files, directories, and even a single huge file, yet results come out in exactly the order GNU grep prints them.
- **🏎️ SIMD search engines.** AVX2 literal scanners, a dense Aho-Corasick automaton for big keyword sets, and literal pre-filters for PCRE2, backreferences, and 1,000-way alternations.
- **⏱️ No startup tax.** Threads start lazily, so a one-file `grep` takes 2.6 ms — the same as GNU grep (2.6 ms; ripgrep: 3.6 ms; ugrep: 4.7 ms).
- **✅ Verified identical.** `stdout`, `stderr`, and exit status are byte-identical to upstream GNU grep across 59 kernel-tree scenarios, 266 edge-case, boundary, and multithreaded checks, continuous stress testing, 6,881 directory-traversal comparisons, and randomized multi-pattern fuzzing, at 1 to 128 threads. It passes GNU grep's own test suite and runs clean under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.

## 🤖 Built for AI coding tools

AI coding agents lean on `grep` constantly: every "where is this defined?" or "who calls this?" becomes a shell call, usually across the whole repository. Supercharged grep makes those calls cheap:

| Benefit | Why it matters for agents |
| :-- | :-- |
| ⚡ **Repo-wide search in well under a second** | Linux kernel (96k files): ~0.15 s with 32 threads, ~0.4 s with 8, vs ~4 s for GNU grep. Shorter tool round-trips → faster agent loops. |
| 🏁 **No need for `rg` or `ugrep`** | Agents often switch to ripgrep or ugrep for speed. At their defaults, Supercharged grep beats both on every search workload we measured, by about 4× on average, so plain `grep` is the faster choice. |
| 🧠 **Nothing to re-learn** | Models already know `grep -rn`, `-i`, `-c`, `-l`, `-F -f`, `-P`. No prompt changes, no custom tool definitions, no `rg` flag translation. |
| 🔒 **Deterministic output** | Same bytes in the same order at any thread count → stable context windows and reproducible runs. |
| 🪶 **Same latency for small lookups** | A tiny single-file `grep` costs what it cost before (2.6 ms); the thread pool only starts when there is enough work. |
| 📚 **Big wins where agents hurt most** | Recursive, case-insensitive, counting, symbol-list (`-F -f`), and PCRE2 searches are 23–121× faster than GNU grep. |

### 🧩 Suggested agent skill

Coding agents are usually told to avoid `grep`. Codex's default instructions say to prefer `rg` because it "is much faster than alternatives like `grep`", and Claude Code sends searches to a built-in Grep tool that runs ripgrep. That advice assumes stock GNU grep. The skill in [`skills/supercharged-grep/SKILL.md`](skills/supercharged-grep/SKILL.md) explains to agents why it doesn't hold for Supercharged grep and how to turn their ripgrep and ugrep habits into correct `grep` commands.

- **Install:** copy [`skills/supercharged-grep`](skills/supercharged-grep/SKILL.md) into your agent's skills directory, for example `~/.claude/skills/` (Claude Code) or `~/.codex/skills/` (Codex):
  ```bash
  mkdir -p ~/.claude/skills && cp -r skills/supercharged-grep ~/.claude/skills/
  ```
  It follows the [Agent Skills](https://agentskills.io) format. For agents that read an instructions file such as `AGENTS.md` instead, paste in everything below the front matter of [`skills/supercharged-grep/SKILL.md`](skills/supercharged-grep/SKILL.md).
- **Optional, for Claude Code:** to send every search through `grep`, add `"Grep"` to `permissions.deny` in `~/.claude/settings.json`. The skill's `allowed-tools: Bash(grep:*)` line lets Claude Code run `grep` without a permission prompt while the skill is active; agents that don't support the field ignore it.

## 📊 Benchmarks

Setup: the Linux kernel source tree (96,034 files, 1.64 GB) on an AMD EPYC server, warm page cache unless noted, `LC_ALL=en_US.UTF-8`, median of 3 runs after a warm-up, all tools measured in one session. GNU grep 3.12 is single-threaded; Supercharged grep runs at its default of 32 threads. [ripgrep](https://github.com/BurntSushi/ripgrep) 15.1 and [ugrep](https://github.com/Genivia/ugrep) 7.8.5 both default to 12 threads and were also measured at 32 (`rg -j 32`, `ugrep -J 32`). Tree searches skip `.git` (`grep -r --exclude-dir=.git`, `rg -uu -g '!.git'`, `ugrep -r --hidden --exclude-dir=.git`), so every tool searches the same files.

| Workload | GNU grep | ripgrep¹ | ugrep¹ | ⚡ Supercharged grep | vs GNU grep | vs ripgrep | vs ugrep |
| :-- | --: | --: | --: | --: | --: | --: | --: |
| Dense literal: `grep -rn 'static int'` | 4.67 s | 505 ms | 627 ms | **158 ms** | 30× | 3.2× | 4.0× |
| Case-insensitive: `grep -ri copyright` | 5.30 s | 724 ms | 785 ms | **161 ms** | 33× | 4.5× | 4.9× |
| Count per file: `grep -rc '#include <linux/'`² | 3.78 s | 1.07 s | 1.12 s | **151 ms** | 25× | 7.1× | 7.4× |
| 1,000 fixed strings: `grep -rF -f sym_1000.txt` | 14.0 s | 333 ms | 571 ms | **173 ms** | 81× | 1.9× | 3.3× |
| PCRE2 lookaround: `grep -rP '(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)'` | 27.6 s | 1.03 s | 1.02 s | **229 ms** | 121× | 4.5× | 4.5× |
| Backreference: `grep -rE '\b([a-z]{4,})_\1\b'`³ | 41.5 s | 339 ms | 617 ms | **260 ms** | 159× | 1.3× | 2.4× |
| 1.4 GB file: `grep -F kmalloc_array_node linux-all.c` | 537 ms | 171 ms | 352 ms | **19.7 ms** | 27× | 8.7× | 18× |
| 1.4 GB file: `grep -ni copyright linux-all.c` | 1.97 s | 300 ms | 301 ms | **44.8 ms** | 44× | 6.7× | 6.7× |
| One small file: `grep -n start_kernel init/main.c` | 2.6 ms | 3.6 ms | 4.7 ms | **2.6 ms** | 1.0× | 1.4× | 1.8× |
| Whole tree, cold cache⁴ | 49.6 s | 4.85 s | 4.75 s | **1.96 s** | 25× | 2.5× | 2.4× |

<sub>¹ The faster of the tool's default (12 threads) and 32-thread runs; [`BENCHMARK_RESULTS.md`](BENCHMARK_RESULTS.md) lists both. ² ripgrep needs `--include-zero` to print the `file:0` lines that `grep -c` prints; ugrep prints them by default. ³ ripgrep and ugrep need `-P` (PCRE2) for backreferences. ⁴ Zero-match literal after evicting the tree's file data from the page cache with `posix_fadvise` (directory metadata stays cached); the 32-thread runs were not measured cold.</sub>

**Across all 59 scenarios (geometric mean): 22.20× faster than GNU grep; 3.89× faster than ripgrep and 4.35× faster than ugrep at their defaults; 2.79× faster than `rg -j 32` and 6.03× faster than `ugrep -J 32`.** At their defaults, neither ripgrep nor ugrep wins a single search: Supercharged grep wins all 58 against each, and the 59th scenario, an empty `/bin/true` baseline, is a tie. Its output was byte-identical to GNU grep's in every scenario.

### Is it a fair fight?

- 🧮 **Same thread count:** with all three on 12 threads (`GREP_THREADS=12` vs the 12-thread default of ripgrep and ugrep), Supercharged grep is still **2.01× faster than ripgrep** and **2.32× faster than ugrep** (geometric mean of 40 scenarios: 40 wins, 0 losses against each).
- 🙈 **The other tools' defaults** (ripgrep skips hidden and `.gitignore`d files; ugrep skips hidden files and directories) do not close the gap: ripgrep takes 391 ms (207 ms with `-j 32`) and ugrep 463 ms (981 ms with `-J 32`) vs **151 ms** for `grep -rn --exclude-dir=.git spin_lock_irqsave`.
- 🧵 **Each tool at its best thread count:** on this machine ugrep is fastest at its default of 12 threads; with `-J 32` its tree searches take 1.67× as long (geometric mean). That is why the table shows the faster of each tool's two runs.
- 💻 **Fewer cores:** capped at 8 threads (`GREP_THREADS=8`), it is still 9–11× faster than GNU grep on whole-tree scans and 30× faster with 1,000 patterns.

📄 Full matrix, thread-scaling curves (1–128 threads), locale and cold-cache analysis: [`BENCHMARK_RESULTS.md`](BENCHMARK_RESULTS.md), generated by `benchmarks/kernel/publish.py`. Reproduce with [`benchmarks/kernel/`](benchmarks/kernel/README.md).

🔬 Refactors of the C code must not cost speed; the gates that check this
are in [`benchmarks/refactor/`](benchmarks/refactor/README.md).

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

- Per-worker I/O buffers, `mmap` for large files, and many reads in flight at once — which is why cold-cache tree scans finish 27× sooner than with GNU grep.

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

### 🧪 Test-only builds

`make check` also builds `src/grep-hooks`, `src/grep-shrunk`, and
`src/dfa-prefilter-test`; none of them is installed. `grep-hooks` and
`grep-shrunk` are compiled with `-DGREP_TEST_HOOKS`, which enables three
environment variables used by the tests:

- `GREP_DELAY_AT=SITE:USEC[,...]` sleeps at thread hand-off points to widen race windows.
- `GREP_ROUTE_LOG=FILE` appends one line per operand naming the search path taken (`file-chunk`, `stdin-slice`, `walk-dirs`, ...); `GREP_ROUTE_LOG=list` prints every route. `tests/parallel-route-trace` requires a case for each route.
- `GREP_PREFILTER_LOG=FILE` appends one line per compiled regex pattern
  recording the literal and back-reference prefilters extracted before search.

The installed `grep` is built without `-DGREP_TEST_HOOKS`: the hooks compile
to nothing, it ignores all three variables, and its machine code is identical
to a build without them. The route list and the steps for adding one are
documented at `GREP_TEST_ROUTE` in `src/grep-test-hooks.h`, and the
capacities that `src/grep-shrunk` shrinks are in `src/grep-tunables.h`.

## ⚠️ Honest notes

- **CPU and memory for time.** Parallel runs burn more CPU-seconds to finish sooner, and a whole-tree search peaks at ~37–132 MB of resident memory (GNU grep, ripgrep, and ugrep: ≤ 51 MB). Lower `GREP_THREADS` on shared machines.
- **Thread-matched ripgrep** (`rg -j 32`) wins 4 of the 59 scenarios: one small sub-directory scan (`T-28`, 15.1 ms vs 16.0 ms), the naive invocation where `grep` scans the 6 GB `.git` directory while `rg` skips it (`U-02`, 209 ms vs 236 ms), and two whole-tree scans (`T-02`, `T-18`) where two of Supercharged grep's three timed runs took about 50 ms longer than the third. Supercharged grep wins 52, often by 2–132×; 3 are ties (`L-00`, `T-15`, `T-25a`). In the 1–128-thread sweep of 9 scenarios, `rg -j 48` also beats Supercharged grep at any thread count on the zero-match whole-tree literal (`T-01`, 140 ms vs 151 ms) and ties it on a literal-free regex (`T-15`); Supercharged grep wins the other 7 swept scenarios. ugrep wins none of the 59 at either thread count (`L-00`, the empty `/bin/true` baseline, is a tie).
- **Piped input is the one place GNU grep itself is not deterministic.** When data read from a pipe contains NUL bytes or encoding errors, GNU grep's output depends on how much each `read` returned. Supercharged grep is deterministic there and matches GNU grep fed by a fast writer; counts, file lists, and exit codes (`-c`, `-l`, `-L`, `-q`) always match.
- **Linux first; macOS experimental.** Every number above comes from Linux x86-64 with AVX2. The non-Linux code paths compile warning-free, but the fork has not yet been built or tested on a Mac — if you try, please report the result. Without AVX2 (including Apple Silicon) the portable 64-bit "SIMD within a register" kernels are used, and the speedups will be smaller.
- **You own updates.** `/usr/local/bin/grep` shadows your distribution's grep, so upstream GNU grep fixes reach you only when you rebuild this fork; `sudo make uninstall` restores the distribution binary.

## 🙏 Credits & license

Supercharged grep is a modified version of [GNU grep](https://www.gnu.org/software/grep/), forked from upstream commit [`e187ca5`](https://git.savannah.gnu.org/cgit/grep.git/commit/?id=e187ca53198c7aafb28da64da86d1bd59244c3cc) (shortly after 3.12) and changed in 2026; the git history records every change. It keeps GNU grep's license, [GPLv3 or later](COPYING), and upstream's documentation (`README`, `NEWS`, `doc/`) still applies because the user-facing behaviour is unchanged.

Found a case where the output differs from GNU grep? Re-run the command with `GREP_THREADS=1`, then open a [GitHub issue](https://github.com/dtwlin/grep/issues) — or email <dtwlin@gmail.com>, the address `grep --help` prints — with the command, the output of `grep --version`, and the input if you can share it. Please do not report problems with this fork to the GNU grep maintainers. Benchmarks compare against [ripgrep](https://github.com/BurntSushi/ripgrep) 15.1.0, an excellent tool that inspired several techniques used here, and [ugrep](https://github.com/Genivia/ugrep) 7.8.5.

## 🤝 Acknowledgement

Supercharged grep was created by Argon using [Antigravity](https://antigravity.google/).

