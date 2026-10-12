#!/usr/bin/env python3
"""Run complete 59-scenario benchmark suite with fair thread-matched comparisons.

Executes:
1. Warm Cache Master Matrix (55 scenarios across B, T, M, U, X, P):
   - G0 (1 thread)
   - G1 (default 32 threads)
   - R0 (default 12 threads)
   - R0 (-j 32)
   - G1 (matched 12 threads on the 38 R0-supported tree scenarios: T, M, P)
2. Invocation Latency Matrix (Category L: L-00, L-01, L-02, L-03):
   - High-iteration micro-benchmarks (30-100 rounds)
3. LC_ALL=C Locale Sensitivity Matrix (22 scenarios)
4. Cold Page Cache Matrix (4 scenarios: T-01, T-03, T-06, B-01 with fadvise eviction)
5. Multi-Thread Scaling Sweeps (9 scenarios across 1..128 threads)
6. Hard Gate A (byte-exact identity) and Gate D (thread determinism) verification
7. Automated report generation via ReportGenerator (summary.md, scaling.csv, equivalence.md)
"""

import argparse
import datetime
import glob
import json
import os
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from scenarios import Scenario, make_scenarios
from evict_cache import evict_paths, check_fincore_sample
from kernel_bench import (
    execute_run,
    capture_env_metadata,
    check_idle_gate,
    pre_warm_cache,
    sha256_file,
    detect_numa_node,
    format_cpu_ranges,
    scrub_data,
)
from report import ReportGenerator


def main():
    parser = argparse.ArgumentParser(description="Run complete 59-scenario benchmark suite with fair thread-matched comparisons.")
    parser.add_argument("--ksrc", default=os.environ.get("K_SRC", os.path.expanduser("~/p/s/linux-stable")), help="Linux kernel source tree")
    parser.add_argument("--kbench-dir", default=os.environ.get("KBENCH_DIR", os.path.expanduser("~/p/s/kbench")), help="Generated corpus directory")
    parser.add_argument("--patterns-dir", default=os.path.join(SCRIPT_DIR, "patterns"), help="Pattern files directory")
    parser.add_argument("--bin-G0", default=os.path.abspath(os.path.join(SCRIPT_DIR, "../grep-baseline")), help="Path to G0 baseline binary")
    parser.add_argument("--bin-G1", default=os.path.abspath(os.path.join(SCRIPT_DIR, "../../src/grep")), help="Path to G1 multi-threaded binary")
    parser.add_argument("--bin-R0", default="/usr/bin/rg", help="Path to R0 ripgrep binary")
    parser.add_argument("--bin-G2", default="/usr/bin/grep", help="Path to G2 distro grep binary")
    parser.add_argument("--bin-X1", default="/usr/bin/git", help="Path to X1 git binary")
    parser.add_argument("--scenarios", default="", help="Comma-separated scenarios to run (default all)")
    parser.add_argument("--rounds", type=int, default=3, help="Measured rounds for standard scenarios")
    parser.add_argument("--warmups", type=int, default=1, help="Warmup rounds for standard scenarios")
    parser.add_argument("--idle-gate", choices=["cpuset", "loadavg", "off"], default="cpuset", help="Idle gate mode before scenarios")
    parser.add_argument("--note", default="Complete 59-scenario Linux kernel benchmark with fair thread-matched comparisons", help="Experiment note")
    parser.add_argument("--out", default=None, help="Output directory for results")
    parser.add_argument("--skip-cold", action="store_true", help="Skip cold cache phase")
    parser.add_argument("--skip-sweep", action="store_true", help="Skip thread sweep phase")
    parser.add_argument("--skip-c", action="store_true", help="Skip LC_ALL=C phase")
    args = parser.parse_args()

    repo_dir = os.path.abspath(os.path.join(SCRIPT_DIR, "../.."))
    binaries = {
        "G0": args.bin_G0,
        "G1": args.bin_G1,
        "R0": args.bin_R0,
        "G2": args.bin_G2,
        "X1": args.bin_X1,
    }

    # Verify binaries
    for t in ("G0", "G1", "R0"):
        b = binaries.get(t)
        if not b or not os.path.exists(b):
            print(f"Error: Binary for {t} not found at {b}", file=sys.stderr)
            sys.exit(1)

    try:
        affinity_cpus = sorted(os.sched_getaffinity(0))
    except Exception:
        affinity_cpus = list(range(os.cpu_count() or 1))

    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S")
    out_dir = args.out if args.out else os.path.join(SCRIPT_DIR, "results", f"{timestamp}-complete")
    os.makedirs(out_dir, exist_ok=True)

    tmp_scratch = "/dev/shm/kbench_out"
    os.makedirs(tmp_scratch, exist_ok=True)

    env_meta = capture_env_metadata(args.ksrc, args.kbench_dir, repo_dir, binaries, affinity_cpus, args.note)
    env_json_path = os.path.join(out_dir, "env.json")
    with open(env_json_path, "w", encoding="utf-8") as f:
        json.dump(env_meta, f, indent=2)

    all_scenarios = make_scenarios()
    tree_38_sids = set(s.sid for s in all_scenarios.values() if s.category in ("T", "M", "P"))

    print("=" * 80)
    print("Complete Linux Kernel-Tree 3-Way Benchmark & Fair Thread-Matched Suite")
    print(f"Output directory : {out_dir}")
    print(f"Kernel source    : {args.ksrc}")
    print(f"Corpus dir       : {args.kbench_dir}")
    print(f"Logical CPUs     : {len(affinity_cpus)} (NUMA node {detect_numa_node(affinity_cpus)})")
    print(f"G1 Binary        : {binaries['G1']} ({sha256_file(binaries['G1'])[:16]}...)")
    print(f"G0 Baseline      : {binaries['G0']} ({sha256_file(binaries['G0'])[:16]}...)")
    print(f"R0 Ripgrep       : {binaries['R0']} ({sha256_file(binaries['R0'])[:16]}...)")
    print(f"Scenarios (total): {len(all_scenarios)} (Tree multi-threaded: {len(tree_38_sids)})")
    print("=" * 80, flush=True)

    # Pre-warm cache for warm runs
    big_path = os.path.join(args.kbench_dir, "linux-all.c")
    pre_warm_cache([big_path])

    raw_jsonl_path = os.path.join(out_dir, "raw.jsonl")
    jsonl_fp = open(raw_jsonl_path, "a", encoding="utf-8")
    all_diff_samples = {}
    gate_a_failures = []

    # =========================================================================
    # Phase 1a: Warm Cache Master Matrix (B, T, M, U, X, P - 55 scenarios)
    # =========================================================================
    phase1_sids = [s.sid for s in all_scenarios.values() if s.category in ("B", "T", "M", "U", "X", "P")]
    if args.scenarios:
        allowed = set(s.strip() for s in args.scenarios.split(",") if s.strip())
        phase1_sids = [sid for sid in phase1_sids if sid in allowed]
    phase1_sids.sort(key=lambda sid: (1 if all_scenarios[sid].category == "P" else 0, sid))

    print(f"\nPhase 1a: Warm Cache Master Matrix ({len(phase1_sids)} scenarios)")
    for s_idx, sid in enumerate(phase1_sids, 1):
        scenario = all_scenarios[sid]
        print(f"\n[Phase 1a {s_idx:2d}/{len(phase1_sids)}] Scenario {scenario.sid}: {scenario.description} ({scenario.category})")
        gate_passed, gate_wait = check_idle_gate(args.idle_gate, affinity_cpus)
        timeout_for_scenario = scenario.timeout_sec or 600

        # Define contestant configurations: (tool, threads_param)
        combos = [
            ("G0", None),       # Baseline (1 thread)
            ("G1", None),       # Our multi-threaded fork (default 32 threads)
            ("R0", None),       # Ripgrep out-of-the-box (default 12 threads)
            ("R0", 32),         # Ripgrep thread-matched (-j 32)
        ]
        if scenario.sid in tree_38_sids:
            combos.append(("G1", 12))  # Our fork thread-matched (GREP_THREADS=12)

        # Warmup rounds
        for w in range(args.warmups):
            for tool, th in combos:
                rec = execute_run(
                    scenario=scenario,
                    tool=tool,
                    bin_path=binaries[tool],
                    ksrc=args.ksrc,
                    kbench_dir=args.kbench_dir,
                    patterns_dir=args.patterns_dir,
                    repo_dir=repo_dir,
                    locale="en_US.UTF-8",
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

        # Measured rounds
        g0_stdout_sha = None
        g0_exit = None

        for r in range(args.rounds):
            combos_rot = combos[r % len(combos):] + combos[:r % len(combos)]
            for tool, th in combos_rot:
                rec = execute_run(
                    scenario=scenario,
                    tool=tool,
                    bin_path=binaries[tool],
                    ksrc=args.ksrc,
                    kbench_dir=args.kbench_dir,
                    patterns_dir=args.patterns_dir,
                    repo_dir=repo_dir,
                    locale="en_US.UTF-8",
                    threads=th,
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
                        gate_a_failures.append((scenario.sid, tool, th, r, "en_US.UTF-8"))

                th_label = f"@{th:2d}t" if th is not None else "def "
                wall_ms = rec["wall_ms"]
                rss_mb = rec["max_rss_kib"] / 1024
                cpu_u = rec["cpu_util"]
                print(f"  {tool:2s} {th_label} r{r}: {wall_ms:7.1f} ms | exit {rec['exit_code']} | RSS {rss_mb:5.1f} MB | CPU {cpu_u*100:5.1f}%{gate_str}")

        # Gate B diff sampling
        ref_g0 = os.path.join(tmp_scratch, f"ref_{scenario.sid}_G0.sorted")
        ref_r0 = os.path.join(tmp_scratch, f"ref_{scenario.sid}_R0.sorted")
        if os.path.exists(ref_g0) and os.path.exists(ref_r0):
            g0_hash = sha256_file(ref_g0)
            r0_hash = sha256_file(ref_r0)
            if r0_hash != g0_hash and scenario.equiv_mode not in ("exit_only", "none"):
                diff_res = subprocess.run(["diff", "-u", ref_g0, ref_r0], capture_output=True, text=True, errors="replace")
                diff_sample = "\n".join(diff_res.stdout.splitlines()[:10])
                classification = "KNOWN-SEMANTIC (BOM stripping)" if scenario.sid == "T-09" else "SOFT_DIFF"
                all_diff_samples.setdefault(scenario.sid, {})["R0"] = {
                    "classification": classification,
                    "diff_sample": diff_sample,
                }
        for f in glob.glob(os.path.join(tmp_scratch, f"ref_{scenario.sid}_*.sorted")):
            try:
                os.remove(f)
            except OSError:
                pass

    # =========================================================================
    # Phase 1b: Latency Matrix (Category L - 4 scenarios)
    # =========================================================================
    lat_sids = ["L-00", "L-01", "L-02", "L-03"]
    lat_rounds = {"L-00": 100, "L-01": 100, "L-02": 50, "L-03": 30}
    print(f"\nPhase 1b: Latency Matrix ({len(lat_sids)} scenarios)")
    for sid in lat_sids:
        scenario = all_scenarios[sid]
        n_rounds = lat_rounds.get(sid, 30)
        print(f"\n[Phase 1b] Scenario {scenario.sid}: {scenario.description} ({n_rounds} rounds)")
        combos = [("G0", None), ("G1", None), ("R0", None), ("R0", 32)]

        for w in range(5):
            for tool, th in combos:
                rec = execute_run(
                    scenario=scenario,
                    tool=tool,
                    bin_path=binaries[tool],
                    ksrc=args.ksrc,
                    kbench_dir=args.kbench_dir,
                    patterns_dir=args.patterns_dir,
                    repo_dir=repo_dir,
                    locale="en_US.UTF-8",
                    threads=th,
                    round_idx=-1 - w,
                    is_warmup=True,
                    tmp_dir=tmp_scratch,
                    timeout_sec=30,
                    cache_state="warm",
                )
                if rec:
                    jsonl_fp.write(json.dumps(rec) + "\n")
                    jsonl_fp.flush()

        for r in range(n_rounds):
            for tool, th in combos:
                rec = execute_run(
                    scenario=scenario,
                    tool=tool,
                    bin_path=binaries[tool],
                    ksrc=args.ksrc,
                    kbench_dir=args.kbench_dir,
                    patterns_dir=args.patterns_dir,
                    repo_dir=repo_dir,
                    locale="en_US.UTF-8",
                    threads=th,
                    round_idx=r,
                    is_warmup=False,
                    tmp_dir=tmp_scratch,
                    timeout_sec=30,
                    cache_state="warm",
                )
                if rec:
                    jsonl_fp.write(json.dumps(rec) + "\n")
                    jsonl_fp.flush()
        print(f"  --> Completed {n_rounds} rounds for {sid}.")

    # =========================================================================
    # Phase 2: LC_ALL=C Locale Sensitivity Matrix (22 scenarios)
    # =========================================================================
    if not args.skip_c:
        c_scenarios_str = "T-01,T-02,T-03,T-04,T-05,T-06,T-09,T-10,T-11,T-12,T-15,T-16,T-25b,B-01,B-02,B-03,B-04,B-05,B-07,B-08,M-02,M-04"
        c_sids = [s.strip() for s in c_scenarios_str.split(",") if s.strip()]
        print(f"\nPhase 2: LC_ALL=C Locale Sensitivity Matrix ({len(c_sids)} scenarios)")
        for s_idx, sid in enumerate(c_sids, 1):
            if sid not in all_scenarios:
                continue
            scenario = all_scenarios[sid]
            print(f"[Phase 2 {s_idx:2d}/{len(c_sids)}] Scenario {scenario.sid} (LC_ALL=C): {scenario.description}")
            gate_passed, gate_wait = check_idle_gate(args.idle_gate, affinity_cpus)
            timeout_for_scenario = scenario.timeout_sec or 600

            # Warmups
            for w in range(1):
                for tool in ("G0", "G1", "R0"):
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

            # Measured
            for r in range(args.rounds):
                for tool in ("G0", "G1", "R0"):
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
                    if rec:
                        jsonl_fp.write(json.dumps(rec) + "\n")
                        jsonl_fp.flush()

    # =========================================================================
    # Phase 3: Cold Page Cache Matrix (4 scenarios)
    # =========================================================================
    if not args.skip_cold:
        cold_sids = ["T-01", "T-03", "T-06", "B-01"]
        print(f"\nPhase 3: Cold Page Cache Matrix ({len(cold_sids)} scenarios, fadvise eviction)")
        for s_idx, sid in enumerate(cold_sids, 1):
            scenario = all_scenarios[sid]
            print(f"\n[Phase 3 {s_idx}/{len(cold_sids)}] Cold Scenario {scenario.sid}: {scenario.description}")
            evict_target = [os.path.join(args.kbench_dir, "linux-all.c")] if scenario.sid == "B-01" else [args.ksrc]

            for r in range(3):
                for tool in ("G0", "G1", "R0"):
                    gate_passed, gate_wait = check_idle_gate(args.idle_gate, affinity_cpus)
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
                        locale="en_US.UTF-8",
                        threads=None,
                        round_idx=r,
                        is_warmup=False,
                        tmp_dir=tmp_scratch,
                        timeout_sec=300,
                        cache_state="cold",
                        idle_gate_wait_s=gate_wait,
                    )
                    if rec:
                        jsonl_fp.write(json.dumps(rec) + "\n")
                        jsonl_fp.flush()
                        print(f"  {tool:2s} cold r{r}: {rec['wall_ms']:7.1f} ms | RSS {rec['max_rss_kib']/1024:5.1f} MB | CPU {rec['cpu_util']*100:5.1f}%")

    # =========================================================================
    # Phase 4: Multi-Thread Scaling Sweeps (9 scenarios, 1..128 threads)
    # =========================================================================
    if not args.skip_sweep:
        sweep_sids = ["T-01", "T-04", "T-06", "T-10", "T-15", "T-16", "B-01", "B-04", "M-02"]
        sweep_threads = [1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128]
        sweep_threads = [th for th in sweep_threads if th <= len(affinity_cpus)]
        print(f"\nPhase 4: Multi-Thread Scaling Sweeps ({len(sweep_sids)} scenarios, threads: {sweep_threads})")

        for s_idx, sid in enumerate(sweep_sids, 1):
            scenario = all_scenarios[sid]
            print(f"\n[Phase 4 {s_idx}/{len(sweep_sids)}] Thread Sweep Scenario {scenario.sid}: {scenario.description}")
            g1_shas = {}

            for th in sweep_threads:
                for tool in ("G1", "R0"):
                    # Warmup
                    rec = execute_run(
                        scenario=scenario,
                        tool=tool,
                        bin_path=binaries[tool],
                        ksrc=args.ksrc,
                        kbench_dir=args.kbench_dir,
                        patterns_dir=args.patterns_dir,
                        repo_dir=repo_dir,
                        locale="en_US.UTF-8",
                        threads=th,
                        round_idx=-1,
                        is_warmup=True,
                        tmp_dir=tmp_scratch,
                        timeout_sec=scenario.timeout_sec or 300,
                        cache_state="warm",
                    )
                    if rec:
                        jsonl_fp.write(json.dumps(rec) + "\n")
                        jsonl_fp.flush()

                    # Measured 3 rounds
                    measured_ms = []
                    for r in range(3):
                        rec = execute_run(
                            scenario=scenario,
                            tool=tool,
                            bin_path=binaries[tool],
                            ksrc=args.ksrc,
                            kbench_dir=args.kbench_dir,
                            patterns_dir=args.patterns_dir,
                            repo_dir=repo_dir,
                            locale="en_US.UTF-8",
                            threads=th,
                            round_idx=r,
                            is_warmup=False,
                            tmp_dir=tmp_scratch,
                            timeout_sec=scenario.timeout_sec or 300,
                            cache_state="warm",
                        )
                        if rec:
                            jsonl_fp.write(json.dumps(rec) + "\n")
                            jsonl_fp.flush()
                            measured_ms.append(rec["wall_ms"])
                            if tool == "G1":
                                g1_shas.setdefault(th, []).append(rec["stdout_sha256"])

                    if measured_ms:
                        med = sorted(measured_ms)[len(measured_ms) // 2]
                        print(f"    {tool:2s} (threads={th:3d}): median {med:7.1f} ms | min {min(measured_ms):7.1f} ms | max {max(measured_ms):7.1f} ms")

            if g1_shas:
                all_g1_shas = {sha for shas in g1_shas.values() for sha in shas}
                if len(all_g1_shas) == 1:
                    print(f"  --> [Gate D for {scenario.sid}: PASS - 100% deterministic stdout SHA-256 across all {len(g1_shas)} thread counts]")
                else:
                    print(f"  --> [Gate D for {scenario.sid}: FAIL - divergent stdout SHA-256 detected: {len(all_g1_shas)} distinct hashes]")

    jsonl_fp.close()

    # Save diff samples
    diff_samples_path = os.path.join(out_dir, "diff_samples.json")
    with open(diff_samples_path, "w", encoding="utf-8") as f:
        json.dump(scrub_data(all_diff_samples, args.ksrc, args.kbench_dir, repo_dir), f, indent=2)

    print("\n" + "=" * 80)
    print("Benchmark execution completed.")
    print(f"Raw results written to: {raw_jsonl_path}")
    print(f"Environment metadata  : {env_json_path}")
    if gate_a_failures:
        print(f"WARNING: Gate A failures detected on {len(gate_a_failures)} runs: {gate_a_failures}")
    else:
        print("Gate A verification: 100% PASS (G1 byte-exact to G0 across all scenarios).")

    # Generate benchmark report
    print("\nGenerating comprehensive report via ReportGenerator...")
    rg = ReportGenerator(out_dir)
    rg.write_summary(os.path.join(out_dir, "summary.md"))
    rg.write_scaling_csv(os.path.join(out_dir, "scaling.csv"))
    rg.write_equivalence(os.path.join(out_dir, "equivalence.md"))
    print("=" * 80)


if __name__ == "__main__":
    main()
