#!/usr/bin/env python3
"""
Benchmark runner for GNU Grep multi-threaded vs baseline.
Verifies byte-for-byte stdout + stderr + exit code equality and measures wall-clock speedup.
Also executes automated edge-case conformance validation.
"""

import hashlib
import os
import re
import resource
import subprocess
import sys
import time

BENCH_DIR = "/dev/shm/grep_bench"
SINGLE_FILE = os.path.join(BENCH_DIR, "single_2GB.log")
TREE_DIR = os.path.join(BENCH_DIR, "tree_10k")

BASELINE_BIN = os.path.abspath("benchmarks/grep-baseline")
CURRENT_BIN = os.path.abspath(os.environ.get("GREP_BIN", "src/grep"))

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
    return re.sub(rb"^[^:]+:", b"grep:", err)

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

    passed = 0
    total = len(checks)
    for name, args, env_vars, preexec in checks:
        _, b_rc, b_out, b_err, _ = run_once(BASELINE_BIN, args, env_vars=env_vars, preexec_fn=None)
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
        for _ in range(num_runs):
            dur, rc, out_sha, err_sha, out_len = run_once(BASELINE_BIN, args, pipe_mode=pipe_mode)
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
    runs = 5
    if len(sys.argv) > 1:
        runs = int(sys.argv[1])
    benchmark(runs)
