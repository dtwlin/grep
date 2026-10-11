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
from evict_cache import evict_paths, check_fincore_sample


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
                norm = re.sub(rb"^[^:\s]+:", b"grep:", line)
                h.update(norm)
        return h.hexdigest()
    except OSError:
        return ""


def read_proc_stat_cpus(target_cpus: Optional[List[int]] = None) -> Dict[int, Tuple[int, int]]:
    """Reads /proc/stat and returns dict: cpu_id -> (busy_ticks, total_ticks)."""
    stats: Dict[int, Tuple[int, int]] = {}
    try:
        with open("/proc/stat", "r") as f:
            for line in f:
                if not line.startswith("cpu"):
                    continue
                parts = line.split()
                name = parts[0]
                if name == "cpu":
                    continue
                try:
                    cpu_num = int(name[3:])
                except ValueError:
                    continue
                if target_cpus is not None and cpu_num not in target_cpus:
                    continue
                vals = [int(x) for x in parts[1:]]
                total = sum(vals)
                idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
                busy = total - idle
                stats[cpu_num] = (busy, total)
    except Exception:
        pass
    return stats


def check_idle_gate(
    mode: str = "cpuset",
    affinity_cpus: Optional[List[int]] = None,
    max_busy_frac: float = 0.10,
    max_load: float = 5.0,
    max_wait_sec: int = 600,
) -> Tuple[bool, float]:
    """Waits for system/affinity CPU set to become idle.
    Returns (passed, total_wait_seconds).
    """
    if mode == "off":
        return True, 0.0

    if mode == "loadavg":
        start = time.time()
        while True:
            try:
                load1, _, _ = os.getloadavg()
                if load1 <= max_load:
                    return True, time.time() - start
            except Exception:
                return True, time.time() - start
            if time.time() - start > max_wait_sec:
                print(f"Notice: loadavg ({load1:.2f}) > {max_load}, continuing after {max_wait_sec}s wait.")
                return False, time.time() - start
            time.sleep(1.0)

    # mode == "cpuset"
    start = time.time()
    if affinity_cpus is None:
        try:
            affinity_cpus = sorted(os.sched_getaffinity(0))
        except Exception:
            affinity_cpus = list(range(os.cpu_count() or 1))

    last_busy_frac = 0.0
    while True:
        s0 = read_proc_stat_cpus(affinity_cpus)
        time.sleep(1.0)
        s1 = read_proc_stat_cpus(affinity_cpus)

        delta_busy = sum(s1[c][0] - s0[c][0] for c in affinity_cpus if c in s0 and c in s1)
        delta_total = sum(s1[c][1] - s0[c][1] for c in affinity_cpus if c in s0 and c in s1)
        busy_frac = (delta_busy / delta_total) if delta_total > 0 else 0.0
        last_busy_frac = busy_frac

        elapsed = time.time() - start
        if busy_frac <= max_busy_frac:
            return True, elapsed

        if elapsed >= max_wait_sec:
            print(f"Notice: CPU busy fraction ({last_busy_frac*100:.1f}%) > {max_busy_frac*100:.0f}%, continuing after {elapsed:.1f}s wait.")
            return False, elapsed


def format_cpu_ranges(cpus: List[int]) -> str:
    if not cpus:
        return ""
    sorted_cpus = sorted(set(cpus))
    ranges = []
    start = sorted_cpus[0]
    end = start
    for c in sorted_cpus[1:]:
        if c == end + 1:
            end = c
        else:
            if start == end:
                ranges.append(str(start))
            else:
                ranges.append(f"{start}-{end}")
            start = c
            end = c
    if start == end:
        ranges.append(str(start))
    else:
        ranges.append(f"{start}-{end}")
    return ",".join(ranges)


def detect_numa_node(affinity_cpus: List[int]) -> str:
    aff_set = set(affinity_cpus)
    nodes = []
    for node_dir in sorted(glob.glob("/sys/devices/system/node/node*")):
        node_name = os.path.basename(node_dir)
        cpulist_path = os.path.join(node_dir, "cpulist")
        if os.path.exists(cpulist_path):
            try:
                with open(cpulist_path) as f:
                    clist = f.read().strip()
                node_cpus = set()
                for part in clist.split(","):
                    if "-" in part:
                        lo, hi = map(int, part.split("-"))
                        node_cpus.update(range(lo, hi + 1))
                    elif part.isdigit():
                        node_cpus.add(int(part))
                if aff_set.issubset(node_cpus):
                    m = re.search(r"\d+", node_name)
                    return m.group(0) if m else node_name
                elif aff_set & node_cpus:
                    m = re.search(r"\d+", node_name)
                    nodes.append(m.group(0) if m else node_name)
            except Exception:
                pass
    return ",".join(nodes) if nodes else "unknown"


def capture_env_metadata(
    ksrc: str,
    kbench_dir: str,
    repo_dir: str,
    binaries: Dict[str, str],
    affinity_cpus: Optional[List[int]] = None,
    note: str = "",
) -> Dict[str, Any]:
    k_rel = platform.release()
    m_k = re.search(r"\b(\d+\.\d+\.\d+)\b", k_rel)
    kernel_release = m_k.group(1) if m_k else k_rel

    if affinity_cpus is None:
        try:
            affinity_cpus = sorted(os.sched_getaffinity(0))
        except Exception:
            affinity_cpus = list(range(os.cpu_count() or 1))

    aff_count = len(affinity_cpus)

    meta: Dict[str, Any] = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": f"Linux-{kernel_release}-{platform.machine()}",
        "processor": platform.processor(),
        "cpu_count_logical": os.cpu_count(),
        "affinity_cpus": format_cpu_ranges(affinity_cpus),
        "affinity_count": aff_count,
        "numa_node": detect_numa_node(affinity_cpus),
        "kernel_release": kernel_release,
    }
    if note:
        meta["note"] = note

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

    empirical_threads = {
        "G0": 1,
        "G1": min(aff_count, 16) + 1 if "G1" in binaries else 17,
        "R0": min(aff_count, 12) + 1 if "R0" in binaries else 13,
        "G2": 1,
        "X1": 129,
    }
    meta["empirical_default_threads"] = empirical_threads

    # Binary metadata
    tools_meta = {}
    for tool_name, bin_path in binaries.items():
        if not bin_path or not os.path.exists(bin_path):
            continue
        t_info: Dict[str, Any] = {
            "path": bin_path,
            "sha256": sha256_file(bin_path),
        }
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
                pipe_proc.stdout.close()

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
    cache_state: str = "warm",
    idle_gate_wait_s: float = 0.0,
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

    if not is_warmup and round_idx == 0:
        ref_sorted = os.path.join(tmp_dir, f"ref_{scenario.sid}_{tool}.sorted")
        if os.path.exists(tmp_sorted):
            shutil.copyfile(tmp_sorted, ref_sorted)

    for p in (tmp_out, tmp_err, tmp_sorted):
        try:
            if os.path.exists(p):
                os.remove(p)
        except OSError:
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
        "cache_state": cache_state,
        "round": round_idx,
        "is_warmup": is_warmup,
        "idle_gate_wait_s": round(idle_gate_wait_s, 3),
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
    parser.add_argument("--locales", default="en_US.UTF-8,C", help="Comma-separated locales (e.g. en_US.UTF-8,C)")
    parser.add_argument("--threads-sweep", default="1,2,4,8,12,16,24,32,48,64", help="Thread counts for sweep scenarios")
    parser.add_argument("--rounds", type=int, default=None, help="Number of measured rounds per scenario")
    parser.add_argument("--warmups", type=int, default=None, help="Number of warmup rounds per scenario")
    parser.add_argument("--timeout", type=int, default=600, help="Per-run timeout in seconds")
    parser.add_argument("--quick", action="store_true", help="Quick run mode (core scenarios, 3 rounds, sweeps)")
    parser.add_argument("--dry-run", action="store_true", help="Dry run mode: 1 round per scenario to test invocation")
    parser.add_argument("--cold", choices=["none", "fadvise", "drop-caches"], default="none", help="Cache eviction mode")
    parser.add_argument("--idle-gate", choices=["cpuset", "loadavg", "off"], default="cpuset", help="Idle gate mode before scenarios")
    parser.add_argument("--note", default="", help="Experimental note or environment context to record in env.json")
    parser.add_argument("--c-scenarios", default=None, help="Comma-separated scenarios for LC_ALL=C matrix")
    parser.add_argument("--cold-scenarios", default="T-01,T-03,T-06,B-01", help="Comma-separated scenarios for cold cache matrix")
    parser.add_argument("--sweep-scenarios", default=None, help="Comma-separated scenarios for thread sweep")
    parser.add_argument("--sweep-rounds", type=int, default=5, help="Number of measured rounds per thread sweep point")
    parser.add_argument("--sweep-warmups", type=int, default=1, help="Number of warmup rounds per thread sweep point")
    parser.add_argument("--cold-rounds", type=int, default=3, help="Number of cold cache measured rounds")
    parser.add_argument("--phase", choices=["all", "warm", "c", "cold", "sweep"], default="all", help="Which benchmark phase to run")
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

    # Detect affinity
    try:
        affinity_cpus = sorted(os.sched_getaffinity(0))
    except Exception:
        affinity_cpus = list(range(os.cpu_count() or 1))

    # Cap thread sweeps to affinity count
    raw_sweeps = [int(th.strip()) for th in args.threads_sweep.split(",") if th.strip()]
    sweep_threads = [th for th in raw_sweeps if th <= len(affinity_cpus)]
    if not sweep_threads and raw_sweeps:
        sweep_threads = [min(raw_sweeps)]

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
    env_meta = capture_env_metadata(args.ksrc, args.kbench_dir, repo_dir, binaries, affinity_cpus, args.note)
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

    # Sort scenarios so category P runs last
    selected_scenarios.sort(key=lambda s: (1 if s.category == "P" else 0, s.sid))

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
    primary_locale = locales[0] if locales else "en_US.UTF-8"

    raw_jsonl_path = os.path.join(out_dir, "raw.jsonl")
    jsonl_fp = open(raw_jsonl_path, "a", encoding="utf-8")

    print("=" * 70)
    print("Linux Kernel-Tree 3-Way Benchmark Harness")
    print(f"Output directory : {out_dir}")
    print(f"Kernel source    : {args.ksrc}")
    print(f"Corpus dir       : {args.kbench_dir}")
    print(f"Contestants      : {', '.join(tools)}")
    print(f"Affinity CPUs    : {format_cpu_ranges(affinity_cpus)} ({len(affinity_cpus)} logical CPUs, NUMA node {detect_numa_node(affinity_cpus)})")
    print(f"Idle gate mode   : {args.idle_gate}")
    print(f"Scenarios ({len(selected_scenarios)}): {', '.join(s.sid for s in selected_scenarios)}")
    print(f"Primary locale   : {primary_locale}")
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

    # =========================================================================
    # Phase 1: Warm Cache Matrix (Primary Locale, e.g. en_US.UTF-8)
    # =========================================================================
    if args.phase not in ("all", "warm"):
        selected_scenarios = []

    total_scenarios = len(selected_scenarios)
    for s_idx, scenario in enumerate(selected_scenarios, 1):
        print(f"\n[Phase 1 {s_idx}/{total_scenarios}] Scenario {scenario.sid}: {scenario.description} ({scenario.category})")
        gate_passed, gate_wait = check_idle_gate(args.idle_gate, affinity_cpus)

        # Determine warmups & rounds for this scenario
        if scenario.category == "L":
            if scenario.sid in ("L-00", "L-01"):
                scen_warmup = 5 if args.warmups is None else args.warmups
                scen_rounds = 100 if args.rounds is None else args.rounds
            elif scenario.sid == "L-02":
                scen_warmup = 5 if args.warmups is None else args.warmups
                scen_rounds = 50 if args.rounds is None else args.rounds
            elif scenario.sid == "L-03":
                scen_warmup = 5 if args.warmups is None else args.warmups
                scen_rounds = 30 if args.rounds is None else args.rounds
            else:
                scen_warmup = 5 if args.warmups is None else args.warmups
                scen_rounds = 100 if args.rounds is None else args.rounds
        elif scenario.category == "P":
            scen_warmup = 1 if args.warmups is None else args.warmups
            scen_rounds = 3 if args.rounds is None else args.rounds
        else:
            scen_warmup = warmup_count
            scen_rounds = measured_rounds

        timeout_for_scenario = scenario.timeout_sec or args.timeout
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
                    locale=primary_locale,
                    threads=None,
                    round_idx=-1 - w,
                    is_warmup=True,
                    tmp_dir=tmp_scratch,
                    timeout_sec=timeout_for_scenario,
                    cache_state="warm",
                    idle_gate_wait_s=gate_wait,
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
                    locale=primary_locale,
                    threads=None,
                    round_idx=r,
                    is_warmup=False,
                    tmp_dir=tmp_scratch,
                    timeout_sec=timeout_for_scenario,
                    cache_state="warm",
                    idle_gate_wait_s=gate_wait,
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
                        gate_a_failures.append((scenario.sid, tool, r, primary_locale))

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
        for f in glob.glob(os.path.join(tmp_scratch, f"ref_{scenario.sid}_*.sorted")):
            try:
                os.remove(f)
            except OSError:
                pass

        # Quick mode inline sweep (1 round)
        if args.quick and scenario.is_sweep and sweep_threads and not args.dry_run:
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
                        locale=primary_locale,
                        threads=th,
                        round_idx=0,
                        is_warmup=False,
                        tmp_dir=tmp_scratch,
                        timeout_sec=timeout_for_scenario,
                        cache_state="warm",
                        idle_gate_wait_s=gate_wait,
                    )
                    if rec:
                        jsonl_fp.write(json.dumps(rec) + "\n")
                        jsonl_fp.flush()
                        wall_ms = rec["wall_ms"]
                        print(f"    {s_tool} (threads={th:3d}): {wall_ms:7.1f} ms | RSS {rec['max_rss_kib']/1024:5.1f} MB")

    # =========================================================================
    # Phase 2: LC_ALL=C Matrix (22 Scenarios: Core + T-12, T-16, B-05, M-04)
    # =========================================================================
    run_c_matrix = ("C" in locales) and not args.quick and not args.dry_run and not args.scenarios and (args.phase in ("all", "c"))
    if run_c_matrix:
        if args.c_scenarios:
            c_sids = [s.strip() for s in args.c_scenarios.split(",") if s.strip()]
        else:
            core_sids = [s.sid for s in all_scenarios.values() if s.is_core]
            c_sids = sorted(set(core_sids) | {"T-12", "T-16", "B-05", "M-04"})
        c_scenarios = [all_scenarios[sid] for sid in c_sids if sid in all_scenarios]
        c_scenarios.sort(key=lambda s: (1 if s.category == "P" else 0, s.sid))

        print("\n" + "=" * 70)
        print(f"Phase 2: LC_ALL=C Matrix ({len(c_scenarios)} scenarios)")
        print("=" * 70)

        for s_idx, scenario in enumerate(c_scenarios, 1):
            print(f"\n[Phase 2 {s_idx}/{len(c_scenarios)}] Scenario {scenario.sid} (LC_ALL=C): {scenario.description}")
            gate_passed, gate_wait = check_idle_gate(args.idle_gate, affinity_cpus)

            scen_warmup = 2 if args.warmups is None else args.warmups
            scen_rounds = 5 if args.rounds is None else args.rounds
            timeout_for_scenario = scenario.timeout_sec or args.timeout
            g0_stdout_sha = None
            g0_exit = None

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
                        locale="C",
                        threads=None,
                        round_idx=-1 - w,
                        is_warmup=True,
                        tmp_dir=tmp_scratch,
                        timeout_sec=timeout_for_scenario,
                        cache_state="warm",
                        idle_gate_wait_s=gate_wait,
                    )
                    if rec:
                        jsonl_fp.write(json.dumps(rec) + "\n")
                        jsonl_fp.flush()

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
                        locale="C",
                        threads=None,
                        round_idx=r,
                        is_warmup=False,
                        tmp_dir=tmp_scratch,
                        timeout_sec=timeout_for_scenario,
                        cache_state="warm",
                        idle_gate_wait_s=gate_wait,
                    )
                    if not rec:
                        continue

                    jsonl_fp.write(json.dumps(rec) + "\n")
                    jsonl_fp.flush()

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
                            gate_a_failures.append((scenario.sid, tool, r, "C"))

                    wall_ms = rec["wall_ms"]
                    rss_mb = rec["max_rss_kib"] / 1024
                    cpu_u = rec["cpu_util"]
                    print(f"  {tool:2s} r{r}: {wall_ms:7.1f} ms | exit {rec['exit_code']} | RSS {rss_mb:5.1f} MB | CPU {cpu_u*100:5.1f}%{gate_str}")

    # =========================================================================
    # Phase 3: Cold Cache Matrix (T-01, T-03, T-06, B-01; fadvise eviction)
    # =========================================================================
    run_cold_matrix = bool(args.cold_scenarios) and not args.quick and not args.dry_run and not args.scenarios and (args.phase in ("all", "cold"))
    if run_cold_matrix:
        cold_sids = [s.strip() for s in args.cold_scenarios.split(",") if s.strip()]
        cold_scenarios = [all_scenarios[sid] for sid in cold_sids if sid in all_scenarios]

        print("\n" + "=" * 70)
        print(f"Phase 3: Cold Cache Matrix ({len(cold_scenarios)} scenarios, {args.cold_rounds} rounds, fadvise eviction)")
        print("=" * 70)

        for s_idx, scenario in enumerate(cold_scenarios, 1):
            print(f"\n[Phase 3 {s_idx}/{len(cold_scenarios)}] Scenario {scenario.sid} (Cold Cache): {scenario.description}")
            timeout_for_scenario = scenario.timeout_sec or args.timeout
            evict_target = [os.path.join(args.kbench_dir, "linux-all.c")] if scenario.sid == "B-01" else [args.ksrc]

            for r in range(args.cold_rounds):
                tools_rotated = tools[r % len(tools):] + tools[:r % len(tools)]
                for tool in tools_rotated:
                    gate_passed, gate_wait = check_idle_gate(args.idle_gate, affinity_cpus)

                    # Evict page cache before every run
                    evict_paths(evict_target)
                    check_fincore_sample(evict_target, sample_size=200, threshold=0.02)

                    rec = execute_run(
                        scenario=scenario,
                        tool=tool,
                        bin_path=binaries[tool],
                        ksrc=args.ksrc,
                        kbench_dir=args.kbench_dir,
                        patterns_dir=args.patterns_dir,
                        repo_dir=repo_dir,
                        locale=primary_locale,
                        threads=None,
                        round_idx=r,
                        is_warmup=False,
                        tmp_dir=tmp_scratch,
                        timeout_sec=timeout_for_scenario,
                        cache_state="cold",
                        idle_gate_wait_s=gate_wait,
                    )
                    if not rec:
                        continue

                    jsonl_fp.write(json.dumps(rec) + "\n")
                    jsonl_fp.flush()

                    wall_ms = rec["wall_ms"]
                    rss_mb = rec["max_rss_kib"] / 1024
                    cpu_u = rec["cpu_util"]
                    print(f"  {tool:2s} cold r{r}: {wall_ms:7.1f} ms | exit {rec['exit_code']} | RSS {rss_mb:5.1f} MB | CPU {cpu_u*100:5.1f}%")

    # =========================================================================
    # Phase 4: Multi-Thread Scaling Sweeps (8 Scenarios, 1..64 Threads)
    # =========================================================================
    run_sweep_matrix = bool(sweep_threads) and not args.dry_run and (args.phase in ("all", "sweep")) and (not args.scenarios or args.phase == "sweep")
    if run_sweep_matrix:
        if args.sweep_scenarios:
            sw_sids = [s.strip() for s in args.sweep_scenarios.split(",") if s.strip()]
        else:
            sw_sids = [s.sid for s in all_scenarios.values() if s.is_sweep]
        sw_scenarios = [all_scenarios[sid] for sid in sw_sids if sid in all_scenarios]

        print("\n" + "=" * 70)
        print(f"Phase 4: Multi-Thread Scaling Sweeps ({len(sw_scenarios)} scenarios, threads: {sweep_threads})")
        print(f"Iteration policy: {args.sweep_warmups} warmup + {args.sweep_rounds} measured rounds per point")
        print("=" * 70)

        for s_idx, scenario in enumerate(sw_scenarios, 1):
            print(f"\n[Phase 4 {s_idx}/{len(sw_scenarios)}] Thread Sweep Scenario {scenario.sid}: {scenario.description}")
            sweep_tools = [t for t in ("G1", "R0", "X1") if t in tools and t not in scenario.na_tools]
            timeout_for_scenario = scenario.timeout_sec or args.timeout
            g1_shas = {}

            for th in sweep_threads:
                for s_tool in sweep_tools:
                    gate_passed, gate_wait = check_idle_gate(args.idle_gate, affinity_cpus)

                    # Warmups for this thread point
                    for w in range(args.sweep_warmups):
                        rec = execute_run(
                            scenario=scenario,
                            tool=s_tool,
                            bin_path=binaries[s_tool],
                            ksrc=args.ksrc,
                            kbench_dir=args.kbench_dir,
                            patterns_dir=args.patterns_dir,
                            repo_dir=repo_dir,
                            locale=primary_locale,
                            threads=th,
                            round_idx=-1 - w,
                            is_warmup=True,
                            tmp_dir=tmp_scratch,
                            timeout_sec=timeout_for_scenario,
                            cache_state="warm",
                            idle_gate_wait_s=gate_wait,
                        )
                        if rec:
                            jsonl_fp.write(json.dumps(rec) + "\n")
                            jsonl_fp.flush()

                    # Measured rounds for this thread point
                    measured_ms = []
                    for r in range(args.sweep_rounds):
                        rec = execute_run(
                            scenario=scenario,
                            tool=s_tool,
                            bin_path=binaries[s_tool],
                            ksrc=args.ksrc,
                            kbench_dir=args.kbench_dir,
                            patterns_dir=args.patterns_dir,
                            repo_dir=repo_dir,
                            locale=primary_locale,
                            threads=th,
                            round_idx=r,
                            is_warmup=False,
                            tmp_dir=tmp_scratch,
                            timeout_sec=timeout_for_scenario,
                            cache_state="warm",
                            idle_gate_wait_s=gate_wait,
                        )
                        if rec:
                            jsonl_fp.write(json.dumps(rec) + "\n")
                            jsonl_fp.flush()
                            measured_ms.append(rec["wall_ms"])
                            if s_tool == "G1":
                                g1_shas.setdefault(th, []).append(rec["stdout_sha256"])

                    if measured_ms:
                        med = sorted(measured_ms)[len(measured_ms) // 2]
                        print(f"    {s_tool:2s} (threads={th:3d}): median {med:7.1f} ms | min {min(measured_ms):7.1f} ms | max {max(measured_ms):7.1f} ms")

            if "G1" in sweep_tools and g1_shas:
                all_g1_shas = {sha for shas in g1_shas.values() for sha in shas}
                if len(all_g1_shas) == 1:
                    print(f"  --> [Gate D for {scenario.sid}: PASS - 100% deterministic stdout SHA-256 across all {len(g1_shas)} thread counts]")
                else:
                    print(f"  --> [Gate D for {scenario.sid}: FAIL - divergent stdout SHA-256 detected: {len(all_g1_shas)} distinct hashes]")

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
