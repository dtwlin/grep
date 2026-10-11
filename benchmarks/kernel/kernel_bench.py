#!/usr/bin/env python3
"""Linux kernel-tree 3-way benchmark runner.

Compares:
  G0: Stock upstream GNU grep baseline
  G1: Multi-threaded GNU grep fork
  R0: Stock ripgrep
  G2: Distro GNU grep
  X1: git grep

Outputs:
  - raw.jsonl: Per-run JSON records with rusage, timings, and SHA-256 hashes
  - env.json: Scrubbed system and binary environment metadata
  - Automatically triggers report.py to produce summary.md, scaling.csv, equivalence.md
"""

import argparse
import datetime
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

# Add parent directory for module imports
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from scenarios import Scenario, make_scenarios


def scrub_string(text: str, ksrc: str = "", kbench_dir: str = "", repo_dir: str = "") -> str:
    if not isinstance(text, str):
        return text

    # Specific corpus files
    if kbench_dir:
        big = os.path.join(kbench_dir, "linux-all.c")
        text = text.replace(big, "$BIG")
        try:
            text = text.replace(os.path.realpath(big), "$BIG")
        except Exception:
            pass

    # Runtime directory paths: ksrc -> $K, kbench_dir -> $KBENCH, repo_dir -> $REPO
    for d_path, tag in [(ksrc, "$K"), (kbench_dir, "$KBENCH"), (repo_dir, "$REPO")]:
        if d_path:
            text = text.replace(d_path, tag)
            try:
                text = text.replace(os.path.realpath(d_path), tag)
            except Exception:
                pass

    # User home directory: os.path.expanduser("~") -> ~
    home = os.path.expanduser("~")
    if home:
        text = text.replace(home, "~")
        try:
            text = text.replace(os.path.realpath(home), "~")
        except Exception:
            pass
    env_home = os.environ.get("HOME", "")
    if env_home:
        text = text.replace(env_home, "~")
        try:
            text = text.replace(os.path.realpath(env_home), "~")
        except Exception:
            pass

    # Hostname and domain suffixes: longest match first -> $HOST
    host_cands = set()
    try:
        import socket
        for name in (socket.gethostname(), socket.getfqdn()):
            if name:
                host_cands.add(name)
                if "." in name:
                    parts = name.split(".")
                    for i in range(1, len(parts)):
                        suffix = ".".join(parts[i:])
                        if "." in suffix:
                            host_cands.add(suffix)
    except Exception:
        pass

    for hc in sorted(host_cands, key=len, reverse=True):
        text = text.replace(hc, "$HOST")

    # User name: getpass.getuser() -> $USER
    try:
        import getpass
        user = getpass.getuser()
        if user:
            text = re.sub(rf"\b{re.escape(user)}\d*\b", "$USER", text)
            text = text.replace(user, "$USER")
    except Exception:
        pass
    env_user = os.environ.get("USER", "")
    if env_user:
        text = re.sub(rf"\b{re.escape(env_user)}\d*\b", "$USER", text)
        text = text.replace(env_user, "$USER")

    return text


def scrub_data(obj: Any, ksrc: str = "", kbench_dir: str = "", repo_dir: str = "") -> Any:
    if isinstance(obj, str):
        return scrub_string(obj, ksrc, kbench_dir, repo_dir)
    elif isinstance(obj, list):
        return [scrub_data(x, ksrc, kbench_dir, repo_dir) for x in obj]
    elif isinstance(obj, dict):
        return {scrub_string(k, ksrc, kbench_dir, repo_dir): scrub_data(v, ksrc, kbench_dir, repo_dir) for k, v in obj.items()}
    return obj


def sha256_file(path: str, chunk_size: int = 4 * 1024 * 1024) -> str:
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return ""


def normalize_and_sort_stdout(
    raw_path: str,
    sorted_path: str,
    tool: str,
    scenario: Scenario,
    cwd: Optional[str],
    ksrc: str,
    tmp_dir: str,
) -> Tuple[str, str, int]:
    """Normalizes and sorts stdout lines for Gate B equivalence checking.

    1. For exit_only or none scenarios: returns raw hash.
    2. For sum_counts scenarios: sums the counts, writes SUM:<total>, and returns hash of that.
    3. For sorted_lines (all other scenarios):
       - Drops lines matching ^(?:binary file .* matches|.*: binary file matches)$ (case-insensitive).
       - For X1 tree scenarios: prefixes cwd + '/' to lines.
       - Runs streaming external sort: LC_ALL=C sort -T <tmp_dir> -o <sorted_path> <norm_tmp>.
       - Computes sha256 of sorted_path.
    Returns (raw_sha256, sorted_normalized_sha256, sum_count).
    """
    raw_sha256 = sha256_file(raw_path)
    if not os.path.exists(raw_path):
        return raw_sha256, raw_sha256, 0

    mode = scenario.equiv_mode
    if mode in ("exit_only", "none"):
        return raw_sha256, raw_sha256, 0

    sum_count = 0
    if mode == "sum_counts":
        try:
            with open(raw_path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    m = re.search(r":?(\d+)$", line.strip())
                    if m:
                        sum_count += int(m.group(1))
        except Exception:
            pass
        with open(sorted_path, "w", encoding="utf-8") as f:
            f.write(f"SUM:{sum_count}\n")
        sorted_sha = sha256_file(sorted_path)
        return raw_sha256, sorted_sha, sum_count

    norm_tmp = os.path.join(tmp_dir, f"norm_{os.path.basename(raw_path)}")
    binary_drop_re = re.compile(rb"^(?:binary file .* matches|.*: binary file matches)\r?$", re.IGNORECASE)

    is_x1_tree = (
        tool == "X1"
        and cwd
        and scenario.category in ("T", "M", "U", "P", "X")
        and scenario.sid not in ("T-25b",)
    )
    prefix_bytes = (cwd.rstrip("/").encode("utf-8") + b"/") if is_x1_tree else None

    with open(raw_path, "rb") as fin, open(norm_tmp, "wb") as fout:
        for line in fin:
            if binary_drop_re.match(line):
                continue
            if prefix_bytes:
                fout.write(prefix_bytes + line)
            else:
                fout.write(line)

    sort_env = os.environ.copy()
    sort_env["LC_ALL"] = "C"
    subprocess.run(["sort", "-T", tmp_dir, "-o", sorted_path, norm_tmp], env=sort_env, check=True)

    try:
        os.remove(norm_tmp)
    except OSError:
        pass

    sorted_sha = sha256_file(sorted_path)
    return raw_sha256, sorted_sha, 0


def normalize_stderr(path: str) -> str:
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for line in f:
                # Normalize argv[0] prefix: e.g. "^[^:]+:" -> "grep:"
                norm = re.sub(rb"^[^:\s]+:", b"grep:", line)
                h.update(norm)
        return h.hexdigest()
    except OSError:
        return ""


def capture_env_metadata(
    ksrc: str,
    kbench_dir: str,
    repo_dir: str,
    binaries: Dict[str, str],
) -> Dict[str, Any]:
    k_rel = platform.release()
    m_k = re.search(r"\b(\d+\.\d+\.\d+)\b", k_rel)
    kernel_release = m_k.group(1) if m_k else k_rel

    meta: Dict[str, Any] = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": f"Linux-{kernel_release}-{platform.machine()}",
        "processor": platform.processor(),
        "cpu_count_logical": os.cpu_count(),
        "kernel_release": kernel_release,
    }

    # CPU info summary
    try:
        res = subprocess.run(["lscpu"], capture_output=True, text=True)
        cpu_dict = {}
        for line in res.stdout.splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                cpu_dict[k.strip()] = v.strip()
        meta["cpu_model"] = cpu_dict.get("Model name", "Unknown")
        meta["numa_nodes"] = cpu_dict.get("NUMA node(s)", "Unknown")
        meta["l3_cache"] = cpu_dict.get("L3 cache", "Unknown")
    except Exception:
        pass

    # Memory info
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    meta["mem_total"] = line.split(":", 1)[1].strip()
    except Exception:
        pass

    # THP status
    thp_path = "/sys/kernel/mm/transparent_hugepage/enabled"
    if os.path.exists(thp_path):
        try:
            with open(thp_path) as f:
                meta["thp_enabled"] = f.read().strip()
        except Exception:
            pass

    # Git revisions
    try:
        res = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo_dir, capture_output=True, text=True)
        meta["grep_git_sha"] = res.stdout.strip()
    except Exception:
        meta["grep_git_sha"] = "unknown"

    try:
        res = subprocess.run(["git", "describe", "--tags", "--always"], cwd=ksrc, capture_output=True, text=True)
        meta["linux_version"] = res.stdout.strip()
    except Exception:
        meta["linux_version"] = "unknown"

    # Binary metadata
    tools_meta = {}
    for tool_name, bin_path in binaries.items():
        if not bin_path or not os.path.exists(bin_path):
            continue
        t_info: Dict[str, Any] = {
            "path": bin_path,
            "sha256": sha256_file(bin_path),
        }
        # version: keep only leading \d+(\.\d+){1,2}
        try:
            res = subprocess.run([bin_path, "--version"], capture_output=True, text=True)
            v_line = res.stdout.splitlines()[0] if res.stdout else res.stderr.splitlines()[0]
            m = re.search(r"\b(\d+(?:\.\d+){1,2})\b", v_line)
            t_info["version"] = m.group(1) if m else "unknown"
        except Exception:
            t_info["version"] = "unknown"
        tools_meta[tool_name] = t_info
    meta["tools"] = tools_meta

    # Scrub paths and internal identifiers
    return scrub_data(meta, ksrc, kbench_dir, repo_dir)


def check_load_average_gate(max_load: float = 5.0, max_wait_sec: int = 15):
    start = time.time()
    while True:
        try:
            load1, _, _ = os.getloadavg()
            if load1 <= max_load:
                return True
        except Exception:
            return True
        if time.time() - start > max_wait_sec:
            print(f"Notice: loadavg ({load1:.2f}) > {max_load}, continuing after {max_wait_sec}s wait.")
            return False
        time.sleep(1.0)


def pre_warm_cache(paths: List[str]):
    """Ensure relevant files are cached in page cache before warm runs."""
    for p in paths:
        if os.path.isfile(p):
            try:
                with open(p, "rb") as f:
                    while f.read(16 * 1024 * 1024):
                        pass
            except OSError:
                pass


def run_process_timed(
    argv: List[str],
    cwd: Optional[str],
    env: Dict[str, str],
    stdin_file: Optional[str],
    pipe_cmd: Optional[List[str]],
    tmp_out: str,
    tmp_err: str,
    timeout_sec: int,
) -> Dict[str, Any]:
    """Spawns process, captures rusage and timing using wait4."""
    timed_out = False
    in_fp = None
    pipe_proc = None

    full_env = {
        "PATH": "/usr/local/bin:/usr/bin:/bin",
        "HOME": os.environ.get("HOME", "/tmp"),
    }
    full_env.update(env)

    with open(tmp_out, "wb") as out_f, open(tmp_err, "wb") as err_f:
        try:
            if pipe_cmd:
                pipe_proc = subprocess.Popen(
                    pipe_cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    close_fds=True,
                )
                stdin_src = pipe_proc.stdout
            elif stdin_file:
                in_fp = open(stdin_file, "rb")
                stdin_src = in_fp
            else:
                stdin_src = subprocess.DEVNULL

            t0 = time.perf_counter_ns()
            proc = subprocess.Popen(
                argv,
                stdin=stdin_src,
                stdout=out_f,
                stderr=err_f,
                cwd=cwd,
                env=full_env,
                close_fds=True,
            )

            if pipe_proc and pipe_proc.stdout:
                # Close in parent so child gets SIGPIPE if it finishes early
                pipe_proc.stdout.close()

            # Poll with os.wait4(pid, WNOHANG) to observe rusage and respect timeout
            start_wall = time.time()
            rusage = None
            exit_code = -1

            while True:
                pid, status, ru = os.wait4(proc.pid, os.WNOHANG)
                if pid != 0:
                    exit_code = os.waitstatus_to_exitcode(status)
                    rusage = ru
                    break

                if time.time() - start_wall > timeout_sec:
                    timed_out = True
                    proc.kill()
                    pid, status, ru = os.wait4(proc.pid, 0)
                    exit_code = os.waitstatus_to_exitcode(status)
                    rusage = ru
                    break
                time.sleep(0.001)

            t1 = time.perf_counter_ns()

        finally:
            if in_fp:
                in_fp.close()
            if pipe_proc:
                try:
                    pipe_proc.kill()
                    pipe_proc.wait(timeout=1.0)
                except Exception:
                    pass

    wall_ns = t1 - t0
    stdout_bytes = os.path.getsize(tmp_out) if os.path.exists(tmp_out) else 0

    return {
        "wall_ns": wall_ns,
        "wall_ms": wall_ns / 1e6,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "stdout_bytes": stdout_bytes,
        "rusage": rusage,
    }


def execute_run(
    scenario: Scenario,
    tool: str,
    bin_path: str,
    ksrc: str,
    kbench_dir: str,
    patterns_dir: str,
    repo_dir: str,
    locale: str,
    threads: Optional[int],
    round_idx: int,
    is_warmup: bool,
    tmp_dir: str,
    timeout_sec: int,
) -> Optional[Dict[str, Any]]:
    # Special-case L-00 (/bin/true)
    if scenario.sid == "L-00":
        cmd_spec = {
            "argv": ["/bin/true"],
            "cwd": None,
            "env": {"LC_ALL": locale},
            "stdin_file": None,
            "pipe_cmd": None,
        }
    else:
        cmd_spec = scenario.build_cmd(
            tool=tool,
            bin_path=bin_path,
            ksrc=ksrc,
            kbench_dir=kbench_dir,
            patterns_dir=patterns_dir,
            threads=threads,
        )

    if cmd_spec.get("na"):
        return None

    cmd_spec.setdefault("env", {})
    cmd_spec["env"]["LC_ALL"] = locale

    tag = f"{scenario.sid}_{tool}_r{round_idx}_{int(time.time()*1000)}"
    tmp_out = os.path.join(tmp_dir, f"{tag}.stdout")
    tmp_err = os.path.join(tmp_dir, f"{tag}.stderr")

    res = run_process_timed(
        argv=cmd_spec["argv"],
        cwd=cmd_spec.get("cwd"),
        env=cmd_spec["env"],
        stdin_file=cmd_spec.get("stdin_file"),
        pipe_cmd=cmd_spec.get("pipe_cmd"),
        tmp_out=tmp_out,
        tmp_err=tmp_err,
        timeout_sec=timeout_sec,
    )

    tmp_sorted = os.path.join(tmp_dir, f"{tag}.sorted")

    # Hashes and output metrics
    stdout_sha256, sorted_sha256, sum_count = normalize_and_sort_stdout(
        raw_path=tmp_out,
        sorted_path=tmp_sorted,
        tool=tool,
        scenario=scenario,
        cwd=cmd_spec.get("cwd"),
        ksrc=ksrc,
        tmp_dir=tmp_dir,
    )
    stderr_sha256 = normalize_stderr(tmp_err)

    # Keep sorted output of first measured round (round 0, not warmup) for diff sample capture
    if not is_warmup and round_idx == 0:
        ref_sorted = os.path.join(tmp_dir, f"ref_{scenario.sid}_{tool}.sorted")
        if os.path.exists(tmp_sorted):
            shutil.copyfile(tmp_sorted, ref_sorted)

    # Clean up tmp files
    for p in (tmp_out, tmp_err, tmp_sorted):
        try:
            if os.path.exists(p):
                os.remove(p)
        except OSError:
            pass
        pass

    ru = res["rusage"]
    user_s = ru.ru_utime if ru else 0.0
    sys_s = ru.ru_stime if ru else 0.0
    wall_s = res["wall_ns"] / 1e9
    cpu_util = ((user_s + sys_s) / wall_s) if wall_s > 0 else 0.0

    record = {
        "scenario": scenario.sid,
        "category": scenario.category,
        "description": scenario.description,
        "tool": tool,
        "threads": threads,
        "locale": locale,
        "round": round_idx,
        "is_warmup": is_warmup,
        "wall_ns": res["wall_ns"],
        "wall_ms": res["wall_ms"],
        "user_s": user_s,
        "sys_s": sys_s,
        "cpu_util": cpu_util,
        "max_rss_kib": ru.ru_maxrss if ru else 0,
        "minflt": ru.ru_minflt if ru else 0,
        "majflt": ru.ru_majflt if ru else 0,
        "nvcsw": ru.ru_nvcsw if ru else 0,
        "nivcsw": ru.ru_nivcsw if ru else 0,
        "inblock": ru.ru_inblock if ru else 0,
        "exit_code": res["exit_code"],
        "timed_out": res["timed_out"],
        "stdout_bytes": res["stdout_bytes"],
        "stdout_sha256": stdout_sha256,
        "stdout_sorted_sha256": sorted_sha256,
        "stderr_sha256": stderr_sha256,
        "sum_count": sum_count,
        "equiv_mode": scenario.equiv_mode,
        "cmd": scrub_data(cmd_spec["argv"], ksrc, kbench_dir, repo_dir),
    }
    return record


def main():
    parser = argparse.ArgumentParser(description="Run 3-way kernel benchmark.")
    parser.add_argument("--ksrc", default=os.environ.get("K_SRC", os.path.expanduser("~/p/s/linux-stable")), help="Linux kernel source tree")
    parser.add_argument("--kbench-dir", default=os.environ.get("KBENCH_DIR", os.path.expanduser("~/p/s/kbench")), help="Generated corpus directory")
    parser.add_argument("--patterns-dir", default=os.path.join(SCRIPT_DIR, "patterns"), help="Pattern files directory")
    parser.add_argument("--tools", default="G0,G1,R0,G2,X1", help="Comma-separated tools to run (G0,G1,R0,G2,X1)")
    parser.add_argument("--bin-G0", default=os.path.abspath(os.path.join(SCRIPT_DIR, "../grep-baseline")), help="Path to G0 baseline binary")
    parser.add_argument("--bin-G1", default=os.path.abspath(os.path.join(SCRIPT_DIR, "../../src/grep")), help="Path to G1 multi-threaded binary")
    parser.add_argument("--bin-R0", default="/usr/bin/rg", help="Path to R0 ripgrep binary")
    parser.add_argument("--bin-G2", default="/usr/bin/grep", help="Path to G2 distro grep binary")
    parser.add_argument("--bin-X1", default="/usr/bin/git", help="Path to X1 git binary")
    parser.add_argument("--categories", default="", help="Comma-separated categories to run (e.g. T,B,M,L,X,U,P)")
    parser.add_argument("--scenarios", default="", help="Comma-separated scenario IDs to run (e.g. T-01,T-03)")
    parser.add_argument("--core-only", action="store_true", help="Run only core (★) scenarios")
    parser.add_argument("--locales", default="en_US.UTF-8", help="Comma-separated locales (e.g. en_US.UTF-8,C)")
    parser.add_argument("--threads-sweep", default="1,4,16,32,64,128", help="Thread counts for sweep scenarios")
    parser.add_argument("--rounds", type=int, default=None, help="Number of measured rounds per scenario")
    parser.add_argument("--warmups", type=int, default=None, help="Number of warmup rounds per scenario")
    parser.add_argument("--timeout", type=int, default=600, help="Per-run timeout in seconds")
    parser.add_argument("--quick", action="store_true", help="Quick run mode (core scenarios, 3 rounds, sweeps)")
    parser.add_argument("--dry-run", action="store_true", help="Dry run mode: 1 round per scenario to test invocation")
    parser.add_argument("--cold", choices=["none", "fadvise", "drop-caches"], default="none", help="Cache eviction mode")
    parser.add_argument("--out", default=None, help="Output directory for results")
    parser.add_argument("--resume", default=None, help="Resume from an existing results directory")
    args = parser.parse_args()

    repo_dir = os.path.abspath(os.path.join(SCRIPT_DIR, "../.."))
    tools = [t.strip() for t in args.tools.split(",") if t.strip()]

    binaries = {
        "G0": args.bin_G0,
        "G1": args.bin_G1,
        "R0": args.bin_R0,
        "G2": args.bin_G2,
        "X1": args.bin_X1,
    }

    # Verify binaries
    for t in tools:
        b = binaries.get(t)
        if not b or not os.path.exists(b):
            print(f"Error: Binary for {t} not found at {b}", file=sys.stderr)
            sys.exit(1)

    # Set up output directory
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S")
    mode_name = "quick" if args.quick else ("dry_run" if args.dry_run else "full")
    if args.out:
        out_dir = args.out
    elif args.resume:
        out_dir = args.resume
    else:
        out_dir = os.path.join(SCRIPT_DIR, "results", f"{timestamp}-{mode_name}")
    os.makedirs(out_dir, exist_ok=True)

    # Output tmpfs scratch dir
    tmp_scratch = "/dev/shm/kbench_out"
    os.makedirs(tmp_scratch, exist_ok=True)

    # Save scrubbed env.json
    env_meta = capture_env_metadata(args.ksrc, args.kbench_dir, repo_dir, binaries)
    env_json_path = os.path.join(out_dir, "env.json")
    with open(env_json_path, "w", encoding="utf-8") as f:
        json.dump(env_meta, f, indent=2)

    diff_samples_path = os.path.join(out_dir, "diff_samples.json")
    all_diff_samples = {}
    if os.path.exists(diff_samples_path):
        try:
            with open(diff_samples_path, "r", encoding="utf-8") as f:
                all_diff_samples = json.load(f)
        except Exception:
            pass

    # All scenarios
    all_scenarios = make_scenarios()
    selected_scenarios: List[Scenario] = []

    if args.scenarios:
        sids = set(s.strip() for s in args.scenarios.split(",") if s.strip())
        selected_scenarios = [s for sid, s in all_scenarios.items() if sid in sids]
    elif args.categories:
        cats = set(c.strip().upper() for c in args.categories.split(",") if c.strip())
        selected_scenarios = [s for s in all_scenarios.values() if s.category in cats]
    elif args.core_only or args.quick:
        selected_scenarios = [s for s in all_scenarios.values() if s.is_core]
    else:
        selected_scenarios = list(all_scenarios.values())

    # Sort scenarios by ID
    selected_scenarios.sort(key=lambda s: s.sid)

    # Configure rounds & warmups
    if args.dry_run:
        warmup_count = 0
        measured_rounds = 1
    elif args.quick:
        warmup_count = 1 if args.warmups is None else args.warmups
        measured_rounds = 3 if args.rounds is None else args.rounds
    else:
        warmup_count = 2 if args.warmups is None else args.warmups
        measured_rounds = 5 if args.rounds is None else args.rounds

    locales = [loc.strip() for loc in args.locales.split(",") if loc.strip()]
    sweep_threads = [int(th.strip()) for th in args.threads_sweep.split(",") if th.strip()]

    raw_jsonl_path = os.path.join(out_dir, "raw.jsonl")
    jsonl_fp = open(raw_jsonl_path, "a", encoding="utf-8")

    print("=" * 70)
    print("Linux Kernel-Tree 3-Way Benchmark Harness")
    print(f"Output directory : {out_dir}")
    print(f"Kernel source    : {args.ksrc}")
    print(f"Corpus dir       : {args.kbench_dir}")
    print(f"Contestants      : {', '.join(tools)}")
    print(f"Scenarios ({len(selected_scenarios)}): {', '.join(s.sid for s in selected_scenarios)}")
    print(f"Locales          : {', '.join(locales)}")
    print(f"Rounds           : {warmup_count} warmup + {measured_rounds} measured")
    if sweep_threads:
        print(f"Thread sweeps    : {sweep_threads}")
    print("=" * 70)

    # Pre-warm cache for warm scenarios
    if args.cold == "none":
        print("Pre-warming page cache for kernel tree...")
        big_path = os.path.join(args.kbench_dir, "linux-all.c")
        pre_warm_cache([big_path])

    # Equivalence tracking across runs
    gate_a_failures = []
    gate_b_failures = []

    # Main execution loop
    total_scenarios = len(selected_scenarios)
    for s_idx, scenario in enumerate(selected_scenarios, 1):
        print(f"\n[{s_idx}/{total_scenarios}] Scenario {scenario.sid}: {scenario.description} ({scenario.category})")
        check_load_average_gate()

        # Determine warmups & rounds for this scenario (Category L uses 5 warmups + 100 measured rounds)
        if scenario.category == "L":
            scen_warmup = 5 if args.warmups is None else args.warmups
            scen_rounds = 100 if args.rounds is None else args.rounds
        else:
            scen_warmup = warmup_count
            scen_rounds = measured_rounds

        for locale in locales:
            # Check baseline G0 output for Gate A comparison in this scenario
            g0_stdout_sha = None
            g0_exit = None

            # 1. Warmup rounds
            for w in range(scen_warmup):
                tools_rotated = tools[w % len(tools):] + tools[:w % len(tools)]
                for tool in tools_rotated:
                    rec = execute_run(
                        scenario=scenario,
                        tool=tool,
                        bin_path=binaries[tool],
                        ksrc=args.ksrc,
                        kbench_dir=args.kbench_dir,
                        patterns_dir=args.patterns_dir,
                        repo_dir=repo_dir,
                        locale=locale,
                        threads=None,
                        round_idx=-1 - w,
                        is_warmup=True,
                        tmp_dir=tmp_scratch,
                        timeout_sec=scenario.timeout_sec or args.timeout,
                    )
                    if rec:
                        jsonl_fp.write(json.dumps(rec) + "\n")
                        jsonl_fp.flush()

            # 2. Measured rounds (tool defaults)
            for r in range(scen_rounds):
                tools_rotated = tools[r % len(tools):] + tools[:r % len(tools)]
                for tool in tools_rotated:
                    rec = execute_run(
                        scenario=scenario,
                        tool=tool,
                        bin_path=binaries[tool],
                        ksrc=args.ksrc,
                        kbench_dir=args.kbench_dir,
                        patterns_dir=args.patterns_dir,
                        repo_dir=repo_dir,
                        locale=locale,
                        threads=None,
                        round_idx=r,
                        is_warmup=False,
                        tmp_dir=tmp_scratch,
                        timeout_sec=scenario.timeout_sec or args.timeout,
                    )
                    if not rec:
                        continue

                    jsonl_fp.write(json.dumps(rec) + "\n")
                    jsonl_fp.flush()

                    # Gate A tracking: G1 vs G0 byte-exact
                    if tool == "G0":
                        g0_stdout_sha = rec["stdout_sha256"]
                        g0_exit = rec["exit_code"]

                    gate_str = ""
                    if tool == "G1" and g0_stdout_sha:
                        if scenario.equiv_mode == "exit_only":
                            passed = (rec["exit_code"] == g0_exit)
                        else:
                            passed = (rec["stdout_sha256"] == g0_stdout_sha and rec["exit_code"] == g0_exit)
                        gate_str = " [Gate A: PASS]" if passed else " [Gate A: FAIL]"
                        if not passed:
                            gate_a_failures.append((scenario.sid, tool, r))

                    wall_ms = rec["wall_ms"]
                    rss_mb = rec["max_rss_kib"] / 1024
                    cpu_u = rec["cpu_util"]
                    print(f"  {tool:2s} r{r}: {wall_ms:7.1f} ms | exit {rec['exit_code']} | RSS {rss_mb:5.1f} MB | CPU {cpu_u*100:5.1f}%{gate_str}")

            # Gate B diff sampling on the first run outputs:
            ref_g0 = os.path.join(tmp_scratch, f"ref_{scenario.sid}_G0.sorted")
            if os.path.exists(ref_g0):
                g0_hash = sha256_file(ref_g0)
                for t in ("R0", "G2", "X1"):
                    ref_t = os.path.join(tmp_scratch, f"ref_{scenario.sid}_{t}.sorted")
                    if os.path.exists(ref_t):
                        t_hash = sha256_file(ref_t)
                        if t_hash != g0_hash and scenario.equiv_mode not in ("exit_only", "none"):
                            diff_res = subprocess.run(
                                ["diff", "-u", ref_g0, ref_t],
                                capture_output=True,
                                text=True,
                                errors="replace",
                            )
                            diff_lines = diff_res.stdout.splitlines()[:10]
                            diff_sample = "\n".join(diff_lines)
                            classification = "KNOWN-SEMANTIC (BOM stripping)" if (scenario.sid == "T-09" and t == "R0") else "SOFT_DIFF"
                            all_diff_samples.setdefault(scenario.sid, {})[t] = {
                                "classification": classification,
                                "diff_sample": diff_sample,
                            }
            # Clean up all ref sorted files for this scenario
            for f in glob.glob(os.path.join(tmp_scratch, f"ref_{scenario.sid}_*.sorted")):
                try:
                    os.remove(f)
                except OSError:
                    pass

            # 3. Thread sweeps (for sweep scenarios)
            if scenario.is_sweep and sweep_threads and not args.dry_run:
                print(f"  Thread sweep ({scenario.sid}): {sweep_threads}")
                sweep_tools = [t for t in ("G1", "R0", "X1") if t in tools and t not in scenario.na_tools]
                for th in sweep_threads:
                    for s_tool in sweep_tools:
                        rec = execute_run(
                            scenario=scenario,
                            tool=s_tool,
                            bin_path=binaries[s_tool],
                            ksrc=args.ksrc,
                            kbench_dir=args.kbench_dir,
                            patterns_dir=args.patterns_dir,
                            repo_dir=repo_dir,
                            locale=locale,
                            threads=th,
                            round_idx=0,
                            is_warmup=False,
                            tmp_dir=tmp_scratch,
                            timeout_sec=scenario.timeout_sec or args.timeout,
                        )
                        if rec:
                            jsonl_fp.write(json.dumps(rec) + "\n")
                            jsonl_fp.flush()
                            wall_ms = rec["wall_ms"]
                            print(f"    {s_tool} (threads={th:3d}): {wall_ms:7.1f} ms | RSS {rec['max_rss_kib']/1024:5.1f} MB")

    jsonl_fp.close()

    # Save diff_samples.json
    diff_samples_path = os.path.join(out_dir, "diff_samples.json")
    with open(diff_samples_path, "w", encoding="utf-8") as f:
        json.dump(scrub_data(all_diff_samples, args.ksrc, args.kbench_dir, repo_dir), f, indent=2)
    print("\n" + "=" * 70)
    print("Benchmark execution completed.")
    print(f"Raw results written to: {raw_jsonl_path}")
    print(f"Environment metadata  : {env_json_path}")
    if gate_a_failures:
        print(f"WARNING: Gate A failures detected on {len(gate_a_failures)} runs: {gate_a_failures}")
    else:
        print("Gate A verification: 100% PASS (G1 byte-exact to G0).")

    # Automatically run report.py to generate summary.md, scaling.csv, equivalence.md
    print("Generating benchmark report...")
    report_script = os.path.join(SCRIPT_DIR, "report.py")
    if os.path.exists(report_script):
        subprocess.run([sys.executable, report_script, "--results-dir", out_dir], check=False)
    print("=" * 70)


if __name__ == "__main__":
    main()
