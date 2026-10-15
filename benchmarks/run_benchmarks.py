#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""
Benchmark runner for GNU Grep multi-threaded vs baseline.
Verifies byte-for-byte stdout + stderr + exit code equality and measures wall-clock speedup.
Also executes automated edge-case conformance validation.
"""

import hashlib
import os
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import time


def _default_bench_dir():
    if os.environ.get("GREP_BENCH_DIR"):
        return os.environ["GREP_BENCH_DIR"]
    if os.path.exists("/dev/shm/grep_bench"):
        return "/dev/shm/grep_bench"
    if os.path.exists("/dev/shm"):
        try:
            if shutil.disk_usage("/dev/shm").free >= int(2.5 * 1024 ** 3):
                return "/dev/shm/grep_bench"
        except OSError:
            pass
    return os.path.join(tempfile.gettempdir(), "grep_bench")


BENCH_DIR = _default_bench_dir()
SINGLE_FILE = os.path.join(BENCH_DIR, "single_2GB.log")
TREE_DIR = os.path.join(BENCH_DIR, "tree_10k")

CURRENT_BIN = os.path.abspath(os.environ.get("GREP_BIN", "src/grep"))
_DEFAULT_BASELINE = os.path.abspath("benchmarks/grep-baseline")
if os.environ.get("BASELINE_BIN"):
    BASELINE_BIN = os.path.abspath(os.environ["BASELINE_BIN"])
elif os.path.exists(_DEFAULT_BASELINE):
    BASELINE_BIN = _DEFAULT_BASELINE
else:
    BASELINE_BIN = CURRENT_BIN

WORKLOADS = [
    {
        "id": "BM-01",
        "name": "-F rare literal",
        "cmd": ["-F", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE],
        "size_source": SINGLE_FILE,
        "pipe_mode": None,
    },
    {
        "id": "BM-02",
        "name": "-Fi case-insensitive",
        "cmd": ["-Fi", "ultra_rare_token_9941", SINGLE_FILE],
        "size_source": SINGLE_FILE,
        "pipe_mode": None,
    },
    {
        "id": "BM-03",
        "name": "-E DFA medium regex",
        "cmd": ["-E", "ERROR_[0-9]{4}_HANDLER", SINGLE_FILE],
        "size_source": SINGLE_FILE,
        "pipe_mode": None,
    },
    {
        "id": "BM-04",
        "name": "-E backreference",
        "cmd": ["-E", "([a-z]+)_\\1_9941", SINGLE_FILE],
        "size_source": SINGLE_FILE,
        "pipe_mode": None,
    },
    {
        "id": "BM-05",
        "name": "-P PCRE2 lookbehind",
        "cmd": ["-P", "(?<=user=)admin_[0-9]+", SINGLE_FILE],
        "size_source": SINGLE_FILE,
        "pipe_mode": None,
    },
    {
        "id": "BM-06",
        "name": "-n -b -F line+offset",
        "cmd": ["-n", "-b", "-F", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE],
        "size_source": SINGLE_FILE,
        "pipe_mode": None,
    },
    {
        "id": "BM-07",
        "name": "-n -C 3 context",
        "cmd": ["-n", "-C", "3", "-E", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE],
        "size_source": SINGLE_FILE,
        "pipe_mode": None,
    },
    {
        "id": "BM-08",
        "name": "-c -E count",
        "cmd": ["-c", "-E", "ERROR_[0-9]{4}_HANDLER", SINGLE_FILE],
        "size_source": SINGLE_FILE,
        "pipe_mode": None,
    },
    {
        "id": "BM-09",
        "name": "-r -n tree lineno",
        "cmd": ["-r", "-n", "-F", "ULTRA_RARE_TOKEN_9941", TREE_DIR],
        "size_source": TREE_DIR,
        "pipe_mode": None,
    },
    {
        "id": "BM-10",
        "name": "-r -l tree list",
        "cmd": ["-r", "-l", "-E", "ERROR_[0-9]{4}_HANDLER", TREE_DIR],
        "size_source": TREE_DIR,
        "pipe_mode": None,
    },
    {
        "id": "BM-11",
        "name": "-r -F multi-pattern",
        "cmd": ["-r", "-F", "-e", "QUERY_PATTERN_ALPHA", "-e", "QUERY_PATTERN_BETA", TREE_DIR],
        "size_source": TREE_DIR,
        "pipe_mode": None,
    },
    {
        "id": "BM-12",
        "name": "Stdin file redirect (<)",
        "cmd": ["-F", "ULTRA_RARE_TOKEN_9941"],
        "size_source": SINGLE_FILE,
        "pipe_mode": "redirect",
    },
    {
        "id": "BM-13",
        "name": "True FIFO pipe (cat | grep)",
        "cmd": ["-n", "-F", "ULTRA_RARE_TOKEN_9941"],
        "size_source": SINGLE_FILE,
        "pipe_mode": "pipe",
    },
]

def get_total_size(path):
    if os.path.isfile(path):
        return os.path.getsize(path)
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            total += os.path.getsize(os.path.join(root, f))
    return total

def normalize_err(err):
    err = re.sub(rb"^(?:[^\s:]*/)?(?:grep|grep-baseline):", b"grep:", err)
    err = re.sub(rb"Try '(?:[^\s']*/)?(?:grep|grep-baseline) --help'", b"Try 'grep --help'", err)
    err = re.sub(rb"Usage: \S*(?:grep-baseline|grep)", b"Usage: grep", err)
    return err

def run_once(binary, args, env_vars=None, pipe_mode=None, preexec_fn=None):
    env = os.environ.copy()
    if env_vars:
        env.update(env_vars)

    cmd = [binary] + args

    t0 = time.perf_counter()
    if pipe_mode == "redirect":
        with open(SINGLE_FILE, "rb") as pipe_in:
            proc = subprocess.run(cmd, stdin=pipe_in, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, preexec_fn=preexec_fn)
    elif pipe_mode == "pipe":
        cat_proc = subprocess.Popen(["cat", SINGLE_FILE], stdout=subprocess.PIPE)
        proc = subprocess.run(cmd, stdin=cat_proc.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, preexec_fn=preexec_fn)
        cat_proc.stdout.close()
        cat_proc.wait()
    else:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, preexec_fn=preexec_fn)
    t1 = time.perf_counter()

    duration_ms = (t1 - t0) * 1000.0
    stdout_sha = hashlib.sha256(proc.stdout).hexdigest()
    stderr_sha = hashlib.sha256(normalize_err(proc.stderr)).hexdigest()
    return duration_ms, proc.returncode, stdout_sha, stderr_sha, len(proc.stdout)

def run_edge_case_conformance():
    print("================================================================================")
    print("               AUTOMATED EDGE-CASE CONFORMANCE VERIFICATION                    ")
    print("================================================================================")
    test_file = os.path.join(TREE_DIR, "dir_000/file_000_000.log")

    def limit_vmem():
        resource.setrlimit(resource.RLIMIT_AS, (512 * 1024 * 1024, 512 * 1024 * 1024))

    def limit_vmem_256():
        resource.setrlimit(resource.RLIMIT_AS, (256 * 1024 * 1024, 256 * 1024 * 1024))

    def limit_nofile_32():
        resource.setrlimit(resource.RLIMIT_NOFILE, (32, 32))

    vmem_ok = True
    try:
        p = subprocess.run([CURRENT_BIN, "--version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           preexec_fn=limit_vmem)
        if p.returncode != 0:
            vmem_ok = False
    except Exception:
        vmem_ok = False

    checks = []

    # 1. Thread sweep: 1, 2, 4, 8, 16, 32, 64
    for t in [1, 2, 4, 8, 16, 32, 64]:
        checks.append((f"Thread sweep single file (t={t})", ["-F", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE], {"GREP_THREADS": str(t)}, None))
        checks.append((f"Thread sweep tree search (t={t})", ["-r", "-l", "FATAL", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": str(t)}, None))

    # 2. Context overlap & group separators
    checks.append(("Context overlap & group sep", ["-n", "-A", "2", "-B", "3", "--group-separator=---", "status=500", SINGLE_FILE], None, None))
    checks.append(("Context overlap C=2 group sep", ["-n", "-C", "2", "--group-separator====", "status=500", SINGLE_FILE], None, None))

    # 3. Invert match
    checks.append(("Invert match small", ["-v", "2026-10-10", test_file], None, None))
    checks.append(("Invert match count 2GB", ["-v", "-c", "INFO", SINGLE_FILE], None, None))
    checks.append(("Invert match lineno small", ["-v", "-n", "INFO", test_file], None, None))

    # 4. Only-matching & color
    checks.append(("Only-matching & color DFA", ["-o", "-n", "--color=always", "ERROR_[0-9]+", SINGLE_FILE], None, None))
    checks.append(("Only-matching & color literal", ["-o", "-n", "--color=always", "-F", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE], None, None))

    # 5. Max count
    checks.append(("Max count 1", ["-m", "1", "INFO", SINGLE_FILE], None, None))
    checks.append(("Max count 5", ["-m", "5", "INFO", SINGLE_FILE], None, None))
    checks.append(("Max count 5 with context", ["-m", "5", "-C", "2", "status=500", SINGLE_FILE], None, None))
    checks.append(("Max count 1 recursive", ["-m", "1", "-r", "INFO", f"{TREE_DIR}/dir_000"], None, None))

    # 6. Null data
    checks.append(("Null data -z", ["-z", "INFO", test_file], None, None))
    checks.append(("Null file -Z -l", ["-Z", "-l", "-r", "FATAL", f"{TREE_DIR}/dir_000"], None, None))

    # 7. Binary file modes
    checks.append(("Binary binary-files", ["--binary-files=binary", "ELF", "/bin/sh"], None, None))
    checks.append(("Binary -a", ["-a", "ELF", "/bin/sh"], None, None))
    checks.append(("Binary -I", ["-I", "ELF", "/bin/sh"], None, None))

    # 8. Virtual memory limit (RLIMIT_AS = 512MB)
    if vmem_ok:
        checks.append(("RLIMIT_AS 512MB slab fallback", ["-F", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE], None, limit_vmem))
        checks.append(("RLIMIT_AS 512MB recursive tree", ["-r", "-n", "FATAL", f"{TREE_DIR}/dir_000"], None, limit_vmem))

    # 9. Invert match with line numbers on 2GB file (Item 4)
    checks.append(("Invert match lineno 2GB -m 1000", ["-v", "-n", "-m", "1000", "INFO", SINGLE_FILE], None, None))

    # 10. PCRE case-insensitive and quantifiers (Item 3)
    checks.append(("PCRE -Pi case-insensitive", ["-P", "-i", "user=admin_[0-9]+", test_file], None, None))
    checks.append(("PCRE (?i) inline flag", ["-P", "(?i)user=admin_[0-9]+", test_file], None, None))
    checks.append(("PCRE quantifier {0,3}", ["-P", "admin_[0-9]{1,3}", test_file], None, None))
    checks.append(("PCRE group quantifier", ["-P", "(?:foo|bar){1,2}_[0-9]+", test_file], None, None))

    # 11. Initial tab alignment (Item 6)
    checks.append(("Initial tab align -T -n -b", ["-T", "-n", "-b", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE], None, None))
    checks.append(("Initial tab align tree", ["-T", "-n", "-b", "-r", "FATAL", f"{TREE_DIR}/dir_000"], None, None))

    # 12. Dense context matches > 70,000 lines (Item 5)
    checks.append(("Dense context match -C 1 75k", ["-C", "1", "-m", "75000", "INFO", SINGLE_FILE], None, None))
    checks.append(("Dense context match -C 1 15k", ["-C", "1", "ERROR_[0-9]{4}_HANDLER", SINGLE_FILE], None, None))

    # 13. Keyword hit on a line that starts just past EGexecute's DFA
    #     lookahead window, so safe_dfaexec gets an END before BEGIN.
    #     The large copy (> 2 MiB) exercises the chunked path.
    dfa_end_text = (b"\nvoid msrs_free(struct msr __percpu *msrs)\n{\n\tfree_percpu(msrs);\n}\n"
                    b"EXPORT_SYMBOL(msrs_free);\n")
    dfa_end_small = os.path.join(BENCH_DIR, "regress_dfa_end.c")
    dfa_end_large = os.path.join(BENCH_DIR, "regress_dfa_end_large.c")
    with open(dfa_end_small, "wb") as f:
        f.write(dfa_end_text)
    with open(dfa_end_large, "wb") as f:
        f.write(dfa_end_text * 50000)
    dfa_end_pat = r"EXPORT_SYMBOL(_GPL)?\([a-z_0-9]+\)"
    checks.append(("DFA end before line start", ["-E", dfa_end_pat, dfa_end_small], None, None))
    checks.append(("DFA end before line start -o", ["-o", "-E", dfa_end_pat, dfa_end_small], {"GREP_THREADS": "1"}, None))
    checks.append(("DFA end before line start large -c", ["-c", "-E", dfa_end_pat, dfa_end_large], None, None))
    checks.append(("DFA end before line start large -n", ["-n", "-E", dfa_end_pat, dfa_end_large], {"GREP_THREADS": "16"}, None))

    # 14. Batch-splitting and timed split behavior
    checks.append(("Batch split regex match t=1", ["-r", "-P", "(?<=user=)regular_user_[0-9]+", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": "1"}, None))
    checks.append(("Batch split regex match t=32", ["-r", "-P", "(?<=user=)regular_user_[0-9]+", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": "32"}, None))
    checks.append(("Batch split regex zero-match t=1", ["-r", "-P", "(?<=file_)[0-9]{3}_000_NO_MATCH", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": "1"}, None))
    checks.append(("Batch split regex zero-match t=32", ["-r", "-P", "(?<=file_)[0-9]{3}_000_NO_MATCH", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": "32"}, None))

    # 15. Virtual memory limit (RLIMIT_AS 512MB with explicit t=32, and 256MB)
    if vmem_ok:
        checks.append(("RLIMIT_AS 512MB tree search t=32", ["-r", "-n", "FATAL", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": "32"}, limit_vmem))
        checks.append(("RLIMIT_AS 256MB tree search default", ["-r", "-n", "FATAL", f"{TREE_DIR}/dir_000"], None, limit_vmem_256))

    # 16. Low file descriptor limits (RLIMIT_NOFILE = 32)
    checks.append(("RLIMIT_NOFILE 32 tree search default", ["-r", "-l", "FATAL", f"{TREE_DIR}/dir_000"], None, limit_nofile_32))
    checks.append(("RLIMIT_NOFILE 32 tree search t=16", ["-r", "-l", "FATAL", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": "16"}, limit_nofile_32))

    # 17. Regex pre-filter parser edge cases (POSIX classes, anchors, UTF-8 quantifiers, PCRE bracket parens)
    regress_prefilter = os.path.join(BENCH_DIR, "regress_prefilter.txt")
    with open(regress_prefilter, "wb") as f:
        f.write(b"abab\nbar\n[(]foo\n")
    checks.append(("Backref POSIX class ([[:alpha:]])\\1", ["-E", r"([[:alpha:]][[:alpha:]])\1", regress_prefilter], None, None))
    checks.append(("Backref anchor (^ab)\\1", ["-E", r"(^ab)\1", regress_prefilter], None, None))
    checks.append(("Backref UTF-8 quantifier", ["-E", "(é?ab)\\1", regress_prefilter], {"LC_ALL": "en_US.UTF-8"}, None))
    checks.append(("PCRE bracketed paren in opt group", ["-P", r"([)]foo)?bar", regress_prefilter], None, None))
    checks.append(("PCRE bracketed open paren", ["-P", r"([(]foo)?bar", regress_prefilter], None, None))

    # 18. PCRE lookaheads, lookbehinds, backreferences, and bracket expressions
    checks.append(("PCRE lookbehind (?<=role=)", ["-P", r"(?<=role=)regular", test_file], None, None))
    checks.append(("PCRE lookahead role=(?=regular)", ["-P", r"role=(?=regular)", test_file], None, None))
    checks.append(("PCRE neg lookahead role=(?!admin)", ["-P", r"role=(?!admin)", test_file], None, None))
    checks.append(("PCRE neg lookbehind (?<!role=)admin", ["-P", r"(?<!role=)admin", test_file], None, None))
    checks.append((r"PCRE named backref \k<name>", ["-P", r"(?P<tok>[0-9]{3})_\k<tok>", test_file], None, None))
    checks.append((r"PCRE numbered backref \g{1}", ["-P", r"([0-9]{3})_\g{1}", test_file], None, None))
    checks.append(("PCRE POSIX class [[:alpha:]]", ["-P", r"[[:alpha:]]+_[[:digit:]]+", test_file], None, None))
    checks.append((r"PCRE hex escape \x{6c}", ["-P", r"\x{6c}\x{6f}\x{67}", test_file], {"LC_ALL": "en_US.UTF-8"}, None))
    checks.append((r"PCRE quote \Q...\E", ["-P", r"\Qlog_entry\E", test_file], None, None))
    checks.append(("PCRE multi-pattern rejection", ["-P", "-e", "foo\nbar", test_file], None, None))

    # 19. ERE & BRE must-literal alternations with classes and quantifiers
    checks.append(("ERE alternation char classes", ["-E", "(a[[:alpha:]]+|b[[:digit:]]+)", test_file], None, None))
    checks.append(("ERE alternation zero quantifier", ["-E", "(foo{0}bar|baz)", test_file], None, None))
    checks.append(("ERE alternation bounded quant", ["-E", "(foo{1,3}bar|test)", test_file], None, None))
    checks.append(("BRE alternation group quantifier", ["-G", r"\(foo\)\?bar\|baz", test_file], None, None))
    checks.append(("BRE alternation multi-group", ["-G", r"\(abc\)\|\(def\)", test_file], None, None))

    # 20. CLI options: -T, -Z, --color, --binary-files, -D
    checks.append(("Initial tab -T -H -n", ["-T", "-H", "-n", "INFO", test_file], None, None))
    checks.append(("Initial tab -T -b", ["-T", "-b", "INFO", test_file], None, None))
    checks.append(("Null separator -Z -c", ["-Z", "-c", "INFO", test_file], None, None))
    checks.append(("Null separator -Z -n", ["-Z", "-n", "INFO", test_file], None, None))
    checks.append(("Color always literal -F", ["--color=always", "-F", "INFO", test_file], None, None))
    checks.append(("Binary without-match", ["--binary-files=without-match", "INFO", test_file], None, None))
    checks.append(("Binary text mode", ["--binary-files=text", "INFO", test_file], None, None))
    checks.append(("Device skip -D skip", ["-D", "skip", "INFO", test_file], None, None))

    # 21. Error diagnostics: invalid -C, -m
    checks.append(("Context -C negative", ["-C", "-1", "INFO", test_file], None, None))
    checks.append(("Context -C invalid", ["-C", "invalid", "INFO", test_file], None, None))
    checks.append(("Max count -m invalid", ["-m", "invalid", "INFO", test_file], None, None))

    # 22. Boundary cases: zero-byte, single-byte, slab/buffer thresholds, missing newline, patterns, limits
    b_empty = os.path.join(BENCH_DIR, "bound_empty.txt")
    open(b_empty, "wb").close()

    b_single_char = os.path.join(BENCH_DIR, "bound_single_char.txt")
    with open(b_single_char, "wb") as f:
        f.write(b"a")

    b_single_nl = os.path.join(BENCH_DIR, "bound_single_nl.txt")
    with open(b_single_nl, "wb") as f:
        f.write(b"\n")

    b_nonl = os.path.join(BENCH_DIR, "bound_nonl.txt")
    with open(b_nonl, "wb") as f:
        f.write(b"line 1: start\nline 2: target_mid\nline 3: target_eof_nonl")

    b_giant = os.path.join(BENCH_DIR, "bound_giant.txt")
    with open(b_giant, "wb") as f:
        f.write(b"A" * 100000 + b"_GIANT_TARGET_" + b"B" * 100000 + b"\n")

    b_slab_64k_m1 = os.path.join(BENCH_DIR, "bound_slab_64k_m1.txt")
    with open(b_slab_64k_m1, "wb") as f:
        f.write(b"x" * (65535 - 7) + b"target\n")

    b_slab_64k = os.path.join(BENCH_DIR, "bound_slab_64k.txt")
    with open(b_slab_64k, "wb") as f:
        f.write(b"x" * (65536 - 7) + b"target\n")

    b_slab_64k_p1 = os.path.join(BENCH_DIR, "bound_slab_64k_p1.txt")
    with open(b_slab_64k_p1, "wb") as f:
        f.write(b"x" * (65537 - 7) + b"target\n")

    b_read_96k_m1 = os.path.join(BENCH_DIR, "bound_read_96k_m1.txt")
    with open(b_read_96k_m1, "wb") as f:
        f.write(b"x" * (98304 - 1 - 7) + b"target\n")

    b_read_96k = os.path.join(BENCH_DIR, "bound_read_96k.txt")
    with open(b_read_96k, "wb") as f:
        f.write(b"x" * (98304 - 7) + b"target\n")

    b_read_96k_p1 = os.path.join(BENCH_DIR, "bound_read_96k_p1.txt")
    with open(b_read_96k_p1, "wb") as f:
        f.write(b"x" * (98304 + 1 - 7) + b"target\n")

    b_bin_first = os.path.join(BENCH_DIR, "bound_bin_first.txt")
    with open(b_bin_first, "wb") as f:
        f.write(b"\x00target_bin\n")

    b_bin_last = os.path.join(BENCH_DIR, "bound_bin_last.txt")
    with open(b_bin_last, "wb") as f:
        f.write(b"target_bin\x00")

    b_bin_mid = os.path.join(BENCH_DIR, "bound_bin_mid.txt")
    with open(b_bin_mid, "wb") as f:
        f.write(b"hello\x00target_bin\n")

    b_pat_0 = os.path.join(BENCH_DIR, "bound_pat_0.txt")
    open(b_pat_0, "wb").close()

    b_pat_32 = os.path.join(BENCH_DIR, "bound_pat_32.txt")
    with open(b_pat_32, "w") as f:
        for i in range(32):
            f.write(f"kw_{i:04d}\n")

    b_pat_33 = os.path.join(BENCH_DIR, "bound_pat_33.txt")
    with open(b_pat_33, "w") as f:
        for i in range(33):
            f.write(f"kw_{i:04d}\n")

    b_pat_64 = os.path.join(BENCH_DIR, "bound_pat_64.txt")
    with open(b_pat_64, "w") as f:
        for i in range(64):
            f.write(f"kw_{i:04d}\n")

    b_pat_65 = os.path.join(BENCH_DIR, "bound_pat_65.txt")
    with open(b_pat_65, "w") as f:
        for i in range(65):
            f.write(f"kw_{i:04d}\n")

    checks.append(("Boundary empty plain", ["target", b_empty], None, None))
    checks.append(("Boundary empty -c", ["-c", "target", b_empty], None, None))
    checks.append(("Boundary empty -c -v", ["-c", "-v", "target", b_empty], None, None))
    checks.append(("Boundary empty -l", ["-l", "target", b_empty], None, None))
    checks.append(("Boundary empty -L", ["-L", "target", b_empty], None, None))
    checks.append(("Boundary empty pat empty", ["", b_empty], None, None))
    checks.append(("Boundary empty -m 0", ["-m", "0", "target", b_empty], None, None))

    checks.append(("Boundary single char match", ["a", b_single_char], None, None))
    checks.append(("Boundary single char nomatch", ["b", b_single_char], None, None))
    checks.append(("Boundary single char -c", ["-c", "a", b_single_char], None, None))
    checks.append(("Boundary single char -v", ["-v", "a", b_single_char], None, None))
    checks.append(("Boundary single char -n", ["-n", "a", b_single_char], None, None))
    checks.append(("Boundary single char -b", ["-b", "a", b_single_char], None, None))

    checks.append(("Boundary single nl empty pat", ["", b_single_nl], None, None))
    checks.append(("Boundary single nl -c", ["-c", "", b_single_nl], None, None))
    checks.append(("Boundary single nl anchor", ["^$", b_single_nl], None, None))
    checks.append(("Boundary single nl -v anchor", ["-v", "^$", b_single_nl], None, None))

    checks.append(("Boundary nonl match mid", ["target_mid", b_nonl], None, None))
    checks.append(("Boundary nonl match eof", ["target_eof_nonl", b_nonl], None, None))
    checks.append(("Boundary nonl -c eof", ["-c", "target_eof_nonl", b_nonl], None, None))
    checks.append(("Boundary nonl -n eof", ["-n", "target_eof_nonl", b_nonl], None, None))
    checks.append(("Boundary nonl -b eof", ["-b", "target_eof_nonl", b_nonl], None, None))
    checks.append(("Boundary nonl -C 1 eof", ["-C", "1", "target_eof_nonl", b_nonl], None, None))
    checks.append(("Boundary nonl -v mid", ["-v", "line 1", b_nonl], None, None))

    checks.append(("Boundary giant line -o", ["-o", "_GIANT_TARGET_", b_giant], None, None))
    checks.append(("Boundary giant line -c", ["-c", "_GIANT_TARGET_", b_giant], None, None))

    checks.append(("Boundary slab 64k - 1", ["target", b_slab_64k_m1], None, None))
    checks.append(("Boundary slab 64k", ["target", b_slab_64k], None, None))
    checks.append(("Boundary slab 64k + 1", ["target", b_slab_64k_p1], None, None))
    checks.append(("Boundary readbuf 96k - 1", ["target", b_read_96k_m1], None, None))
    checks.append(("Boundary readbuf 96k", ["target", b_read_96k], None, None))
    checks.append(("Boundary readbuf 96k + 1", ["target", b_read_96k_p1], None, None))

    checks.append(("Boundary bin first byte default", ["target_bin", b_bin_first], None, None))
    checks.append(("Boundary bin first byte -a", ["-a", "target_bin", b_bin_first], None, None))
    checks.append(("Boundary bin first byte -I", ["-I", "target_bin", b_bin_first], None, None))
    checks.append(("Boundary bin last byte -a", ["-a", "target_bin", b_bin_last], None, None))
    checks.append(("Boundary bin mid byte -a", ["-a", "target_bin", b_bin_mid], None, None))
    checks.append(("Boundary bin mid byte -z", ["-z", "target_bin", b_bin_mid], None, None))

    checks.append(("Boundary pat 0 file", ["-F", "-f", b_pat_0, b_nonl], None, None))
    checks.append(("Boundary pat 32 file", ["-F", "-f", b_pat_32, b_nonl], None, None))
    checks.append(("Boundary pat 33 file", ["-F", "-f", b_pat_33, b_nonl], None, None))
    checks.append(("Boundary pat 64 file", ["-F", "-f", b_pat_64, b_nonl], None, None))
    checks.append(("Boundary pat 65 file", ["-F", "-f", b_pat_65, b_nonl], None, None))

    checks.append(("Boundary regex exact empty", ["-x", "", b_single_nl], None, None))
    checks.append(("Boundary regex empty group star", ["-E", "()*target", b_slab_64k], None, None))
    checks.append(("Boundary regex empty alt start", ["-E", "|target", b_slab_64k], None, None))
    checks.append(("Boundary regex empty alt end", ["-E", "target|", b_slab_64k], None, None))
    checks.append(("Boundary regex empty alt mid", ["-E", "a||b", b_nonl], None, None))
    checks.append(("Boundary regex word empty", ["-w", "", b_nonl], None, None))

    checks.append(("Boundary max count -m 0", ["-m", "0", "target", b_nonl], None, None))
    checks.append(("Boundary max count -m 1 -c", ["-m", "1", "-c", "line", b_nonl], None, None))
    checks.append(("Boundary max count -m 1 -o", ["-m", "1", "-o", "target", b_nonl], None, None))
    checks.append(("Boundary context -C 0", ["-C", "0", "target_mid", b_nonl], None, None))
    checks.append(("Boundary context -B 10 line1", ["-B", "10", "line 1", b_nonl], None, None))
    checks.append(("Boundary context -A 10 eof", ["-A", "10", "target_eof_nonl", b_nonl], None, None))

    # 23. Multithreaded cases: prime threads, oversubscribed threads, multi-chunk concurrent files, ROB head-of-line
    for t in [3, 5, 7, 31, 128]:
        checks.append((f"Thread sweep single file (t={t})", ["-F", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE], {"GREP_THREADS": str(t)}, None))
        checks.append((f"Thread sweep tree search (t={t})", ["-r", "-l", "FATAL", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": str(t)}, None))

    # Multiple concurrent chunked files (> 2 MiB each)
    mt_chunk1 = os.path.join(BENCH_DIR, "mt_chunk1.txt")
    mt_chunk2 = os.path.join(BENCH_DIR, "mt_chunk2.txt")
    with open(mt_chunk1, "wb") as f:
        f.write((b"x" * 60 + b" MT_CHUNK_1 filler data\n") * 35000)
    with open(mt_chunk2, "wb") as f:
        f.write((b"y" * 60 + b" MT_CHUNK_2 filler data\n") * 35000)

    for t in [1, 4, 16, 32]:
        checks.append((f"Multi-chunk concurrent files (t={t})", ["-n", "-b", "MT_CHUNK", mt_chunk1, mt_chunk2], {"GREP_THREADS": str(t)}, None))

    # Tree search with -m 1, -C 1, and -q across high thread counts
    for t in [16, 32]:
        checks.append((f"Max count -m 1 tree search (t={t})", ["-m", "1", "-r", "FATAL", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": str(t)}, None))
        checks.append((f"Context -C 1 tree search (t={t})", ["-C", "1", "-r", "FATAL", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": str(t)}, None))
        checks.append((f"Early exit -q tree search (t={t})", ["-q", "-r", "FATAL", f"{TREE_DIR}/dir_000"], {"GREP_THREADS": str(t)}, None))

    # 24. CLI flags & mixed-operand conformance (Bug 1-5 & flag coverage)
    excl_from_path = os.path.join(BENCH_DIR, "regress_exclude.list")
    with open(excl_from_path, "wb") as f:
        f.write(b"*_001.log\n*_002.log\n")
    cmd_files_20 = [os.path.join(TREE_DIR, f"dir_000/file_000_{i:03d}.log") for i in range(20)]

    checks.append(("Mixed small + large chunked file", ["MT_CHUNK", test_file, mt_chunk1], None, None))
    checks.append(("Mixed large + small file", ["FATAL", mt_chunk1, test_file], None, None))
    checks.append(("Quiet -q missing + large file", ["-q", "MT_CHUNK", os.path.join(BENCH_DIR, "no_such_file.log"), mt_chunk1], None, None))
    checks.append(("Exclude * on dir with >=16 files", ["--exclude=*", "FATAL", f"{TREE_DIR}/dir_000"] + cmd_files_20, None, None))
    checks.append(("Exclude * on missing >=16 files", ["--exclude=*", "FATAL", os.path.join(BENCH_DIR, "no_such_file.log")] + cmd_files_20, None, None))
    checks.append(("Directories -d skip >=16 files", ["-d", "skip", "FATAL", f"{TREE_DIR}/dir_000"] + cmd_files_20, None, None))
    checks.append(("Directories -d read >=16 files", ["-d", "read", "FATAL", f"{TREE_DIR}/dir_000"] + cmd_files_20, None, None))
    checks.append(("Exclude-from file tree search", ["-r", f"--exclude-from={excl_from_path}", "-c", "FATAL", f"{TREE_DIR}/dir_000"], None, None))
    checks.append(("Case override -i --no-ignore-case", ["-F", "-i", "--no-ignore-case", "mt_chunk_1", mt_chunk1], None, None))
    checks.append(("Obsolete -y case-insensitive", ["-F", "-y", "mt_chunk_1", mt_chunk1], None, None))
    checks.append(("Undocumented -X awk matcher", ["-X", "awk", "MT_CHUNK_1|MT_CHUNK_2", mt_chunk1, mt_chunk2], None, None))
    checks.append(("Undocumented -X gawk matcher", ["-X", "gawk", "MT_CHUNK_1|MT_CHUNK_2", mt_chunk1, mt_chunk2], None, None))
    checks.append(("Context shorthand -2", ["-2", "-n", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE], None, None))
    checks.append(("Line-buffered --line-buffered", ["--line-buffered", "-n", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE], None, None))
    checks.append(("No-filename -h multi-chunk", ["-h", "-n", "MT_CHUNK", mt_chunk1, mt_chunk2], None, None))
    checks.append(("GREP_COLORS custom palette", ["--color=always", "-H", "-n", "-b", "-C", "1", "ULTRA_RARE_TOKEN_9941", SINGLE_FILE],
                   {"GREP_COLORS": "ms=01;32:mc=01;34:sl=01;33:cx=01;35:fn=36:ln=32:bn=33:se=31"}, None))
    checks.append(("GREP_COLORS rv inverted", ["--color=always", "-v", "-m", "20", "INFO", test_file],
                   {"GREP_COLORS": "rv:sl=01;31:cx=01;32"}, None))

    # 25. Round 2 audit regressions (zero-context chunk boundaries, fast-mmap separator/-q, pre-filters)
    bin_fast_file = os.path.join(BENCH_DIR, "regress_bin_fast.bin")
    with open(bin_fast_file, "wb") as f:
        f.write(b"hdr\x00nul\n" + (b"x" * 70 + b"\n") * 5000 + b"MT_CHUNK_1 hit\n" + (b"y" * 70 + b"\n") * 30000)

    checks.append(("Zero-context -C 0 adjacent chunks", ["-C", "0", "-c", "MT_CHUNK_1", mt_chunk1], None, None))
    checks.append(("Zero-context -A 0 adjacent chunks", ["-A", "0", "MT_CHUNK_1", mt_chunk1], None, None))
    checks.append(("Zero-context -B 0 adjacent chunks", ["-B", "0", "MT_CHUNK_1", mt_chunk1], None, None))
    checks.append(("Zero-context -0 adjacent chunks", ["-0", "MT_CHUNK_1", mt_chunk1], None, None))
    checks.append(("Fast-mmap binary + text -C 1 sep", ["-F", "-C", "1", "MT_CHUNK_1", bin_fast_file, test_file], None, None))
    checks.append(("Fast-mmap binary -q + missing", ["-F", "-q", "MT_CHUNK_1", bin_fast_file, os.path.join(BENCH_DIR, "no_such_after.log")], {"GREP_THREADS": "1"}, None))
    checks.append(("BRE kwsmusts_multi \\+ quantifier", ["-e", r"MT_CHUNK_\+1", "-e", "NEVER_HIT", test_file], None, None))
    checks.append(("ERE kwsmusts_multi {m,n} quant", ["-E", "MT_CHUNK_[0-9]{1,2}|NEVER_HIT", test_file], None, None))
    checks.append(("Backref stacked quant (a{3}?)\\1", ["-E", "-c", r"(a{3}?)\1", test_file], None, None))
    checks.append(("Backref invalid interval (a{10,x})\\1", ["-E", "-c", r"(a{10,x})\1", test_file], None, None))
    checks.append(("PCRE (*ACCEPT) must-literal", ["-P", "-c", "MT_CHUNK_1(*ACCEPT)NEVER_LITERAL", test_file], None, None))
    checks.append(("PCRE .{10,20} must-literal", ["-P", "-c", r".{10,20}MT_CHUNK_1", test_file], None, None))

    passed = 0
    total = len(checks)
    for name, args, env_vars, preexec in checks:
        b_env = dict(env_vars) if env_vars else {}
        if BASELINE_BIN == CURRENT_BIN:
            b_env["GREP_THREADS"] = "1"
        _, b_rc, b_out, b_err, _ = run_once(BASELINE_BIN, args, env_vars=b_env, preexec_fn=None)
        _, c_rc, c_out, c_err, c_len = run_once(CURRENT_BIN, args, env_vars=env_vars, preexec_fn=preexec)

        if b_rc == c_rc and b_out == c_out and b_err == c_err:
            passed += 1
            print(f"  [PASS] {name:<36} (rc={c_rc}, out_len={c_len:,} B)")
        else:
            print(f"  [FAIL] {name:<36} (rc: {b_rc} vs {c_rc}, stdout_match: {b_out == c_out}, stderr_match: {b_err == c_err})")
            sys.exit(1)

    print(f"\nAll {passed}/{total} edge-case conformance checks PASSED 100% byte-for-byte!\n")

def benchmark(num_runs=5):
    if not os.path.exists(SINGLE_FILE) or not os.path.exists(TREE_DIR):
        print("Corpus not found! Running benchmarks/gen_corpus.py first...")
        subprocess.run([sys.executable, "benchmarks/gen_corpus.py"], check=True)

    run_edge_case_conformance()

    print("================================================================================")
    print(f"        BENCHMARK RUN: Baseline ({BASELINE_BIN}) vs Current ({CURRENT_BIN})")
    print(f"                       Iterations per workload: {num_runs}")
    print("================================================================================\n")

    sizes = {w["id"]: get_total_size(w["size_source"]) for w in WORKLOADS}

    results = []

    for w in WORKLOADS:
        wid = w["id"]
        wname = w["name"]
        args = w["cmd"]
        pipe_mode = w["pipe_mode"]
        size_bytes = sizes[wid]
        size_gb = size_bytes / (1024 ** 3)

        # Baseline runs
        base_times = []
        base_rc, base_stdout_sha, base_stderr_sha = None, None, None
        base_env = {"GREP_THREADS": "1"} if BASELINE_BIN == CURRENT_BIN else None
        for _ in range(num_runs):
            dur, rc, out_sha, err_sha, out_len = run_once(BASELINE_BIN, args, env_vars=base_env, pipe_mode=pipe_mode)
            base_times.append(dur)
            base_rc, base_stdout_sha, base_stderr_sha = rc, out_sha, err_sha
        base_med = sorted(base_times)[len(base_times) // 2]
        base_gbps = size_gb / (base_med / 1000.0)

        # Configs to test for Current
        configs = [
            ("t=1", {"GREP_THREADS": "1"}),
            ("t=4", {"GREP_THREADS": "4"}),
            ("default", {}),
        ]

        cfg_results = {}
        for cname, cenv in configs:
            cur_times = []
            verified = True
            for _ in range(num_runs):
                dur, rc, out_sha, err_sha, out_len = run_once(CURRENT_BIN, args, env_vars=cenv, pipe_mode=pipe_mode)
                cur_times.append(dur)
                if (rc != base_rc) or (out_sha != base_stdout_sha) or (err_sha != base_stderr_sha):
                    verified = False
            cur_med = sorted(cur_times)[len(cur_times) // 2]
            cur_gbps = size_gb / (cur_med / 1000.0)
            speedup = base_med / cur_med if cur_med > 0 else 1.0
            cfg_results[cname] = {
                "med_ms": cur_med,
                "gbps": cur_gbps,
                "speedup": speedup,
                "verified": verified,
            }

        results.append({
            "id": wid,
            "name": wname,
            "size_mb": size_bytes / (1024 ** 2),
            "base_ms": base_med,
            "base_gbps": base_gbps,
            "cfg": cfg_results,
        })

    # Print Table
    header = (
        "| ID | Workload | Size | Baseline (ms) | Baseline (GB/s) | "
        "Cur t=1 (ms) | Cur t=4 (ms) | Cur Def (ms) | Speedup (Def) | Equiv? |"
    )
    sep = (
        "| :--- | :--- | :---: | :---: | :---: | "
        ":---: | :---: | :---: | :---: | :---: |"
    )
    print(header)
    print(sep)
    for r in results:
        equiv = "PASS" if all(r["cfg"][c]["verified"] for c in ["t=1", "t=4", "default"]) else "FAIL"
        print(
            f"| {r['id']} | {r['name']} | {r['size_mb']:.0f} MB | "
            f"{r['base_ms']:.1f} | {r['base_gbps']:.2f} | "
            f"{r['cfg']['t=1']['med_ms']:.1f} | {r['cfg']['t=4']['med_ms']:.1f} | "
            f"{r['cfg']['default']['med_ms']:.1f} | {r['cfg']['default']['speedup']:.2f}x | {equiv} |"
        )

if __name__ == "__main__":
    if "--conformance-only" in sys.argv:
        if not os.path.exists(SINGLE_FILE) or not os.path.exists(TREE_DIR):
            print("Corpus not found! Running benchmarks/gen_corpus.py first...")
            subprocess.run([sys.executable, "benchmarks/gen_corpus.py"], check=True)
        run_edge_case_conformance()
        sys.exit(0)
    runs = 5
    if len(sys.argv) > 1:
        runs = int(sys.argv[1])
    benchmark(runs)
