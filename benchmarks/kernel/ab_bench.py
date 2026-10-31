#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Interleaved A/B timing of two grep builds on the kernel scenarios.

usage: ab_bench.py --a A/src/grep --b B/src/grep --out DIR
                   [--phase PHASE] [--scenarios IDS] [--rounds N]
                   [--precise] [--summarize]
       ab_bench.py --summarize DIR...

Every run goes through kernel_bench.execute_run, so it is measured
like the published harness: same command line and environment, output
to a tmpfs file, rusage from wait4.  Builds A and B alternate run by
run (A then B in even rounds, B then A in odd rounds), so slow drift on
the host affects both alike, and each round yields one B/A pair.  Both
programs are first copied, one right after the other, into the scratch
directory, so that their pages come from the same memory (on a NUMA
host, two identical builds built on different nodes can differ by a
few percent in millisecond-long runs).

Phases: warm (en_US.UTF-8, warm page cache, the default), c (LC_ALL=C),
sweep (--threads 1,2,4,...), fixed (--threads N), and cold (page cache
evicted before each run).  Each scenario runs --rounds pairs after
--warmups warm-up pairs; by default 20 pairs (latency scenarios L-xx:
30 to 100), 5 per sweep point, and 3 when cold.

--precise times each run with a blocking wait4.  The harness polls
wait4 every millisecond, which rounds wall times up to steps of about
1.06 ms; use --precise for scenarios that take a few milliseconds.

Each record of DIR/raw.jsonl holds the harness fields plus "build"
(A or B), "phase", "regime" (--regime), "precise", "sha_a" and "sha_b"
(SHA-256 prefixes of the two programs), and "session" (the start time
of the ab_bench run in nanoseconds, which tells apart the rounds of
runs that append to the same file).

--summarize applies the decision rule of benchmarks/refactor/README.md
to the records in DIR (and in --out after a run).  It keeps apart the
records of different programs, phases, scenarios, thread counts,
locales, regimes and timing methods, and leaves out warm-up and
timed-out runs.  For each group it reports the median of the
per-round ratios B/A and its 95% bootstrap confidence interval
(10,000 resamples), and one verdict:

  slower        the interval lies entirely above 1.015
  faster        the interval lies entirely below 1/1.015
  same          the interval lies within [1/1.015, 1.015]
  wide          the interval is wider than +-3%: run more pairs
  inconclusive  none of the above: run more pairs
  few           fewer than 20 pairs: run more pairs

It also checks that A and B printed the same output and exit status.

Exit status: 1 if A and B printed different output or a group is
slower; otherwise 3 if a group needs more pairs; otherwise 0.
"""

import argparse
import collections
import hashlib
import json
import os
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import traceback

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

import kernel_bench as kb  # noqa: E402
from evict_cache import check_fincore_sample, evict_paths  # noqa: E402
from scenarios import make_scenarios  # noqa: E402

L_ROUNDS = {"L-00": 100, "L-01": 100, "L-02": 50, "L-03": 30}
MIN_PAIRS = 20
SLOWER = 1.015
WIDE = 0.03
RESAMPLES = 10000


def precise_run_process_timed(argv, cwd, env, stdin_file, pipe_cmd,
                              tmp_out, tmp_err, timeout_sec):
    """kernel_bench.run_process_timed with a blocking wait4.

    It reads the clock as soon as the child is reaped, and a timer
    thread enforces the timeout.  The environment and redirections are
    the same as the harness's.
    """
    in_fp = None
    pipe_proc = None
    killed = []
    full_env = {"PATH": "/usr/local/bin:/usr/bin:/bin",
                "HOME": os.environ.get("HOME", "/tmp")}
    full_env.update(env)
    with open(tmp_out, "wb") as out_f, open(tmp_err, "wb") as err_f:
        try:
            if pipe_cmd:
                pipe_proc = subprocess.Popen(
                    pipe_cmd, stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL, close_fds=True)
                stdin_src = pipe_proc.stdout
            elif stdin_file:
                in_fp = open(stdin_file, "rb")
                stdin_src = in_fp
            else:
                stdin_src = subprocess.DEVNULL
            t0 = time.perf_counter_ns()
            proc = subprocess.Popen(argv, stdin=stdin_src, stdout=out_f,
                                    stderr=err_f, cwd=cwd, env=full_env,
                                    close_fds=True)
            if pipe_proc and pipe_proc.stdout:
                pipe_proc.stdout.close()

            def kill():
                killed.append(1)
                proc.kill()

            timer = threading.Timer(timeout_sec, kill)
            timer.start()
            _, status, ru = os.wait4(proc.pid, 0)
            t1 = time.perf_counter_ns()
            timer.cancel()
            proc.returncode = os.waitstatus_to_exitcode(status)
        finally:
            if in_fp:
                in_fp.close()
            if pipe_proc:
                try:
                    pipe_proc.kill()
                    pipe_proc.wait(timeout=1.0)
                except (OSError, subprocess.SubprocessError):
                    pass
    return {
        "wall_ns": t1 - t0,
        "wall_ms": (t1 - t0) / 1e6,
        "exit_code": proc.returncode,
        "timed_out": bool(killed),
        "stdout_bytes": (os.path.getsize(tmp_out)
                         if os.path.exists(tmp_out) else 0),
        "rusage": ru,
    }


def default_schedule(scen, phase):
    """Return the (warm-up pairs, measured pairs) of SCEN in PHASE."""
    if phase == "cold":
        return 0, 3
    if phase == "sweep":
        return 1, 5
    if scen.category == "L":
        return 5, max(MIN_PAIRS, L_ROUNDS.get(scen.sid, 100))
    return 2, MIN_PAIRS


def bootstrap_ci(ratios, rng):
    """Return the 95% bootstrap confidence interval of the median."""
    k = len(ratios)
    meds = sorted(statistics.median(rng.choices(ratios, k=k))
                  for _ in range(RESAMPLES))
    return meds[int(RESAMPLES * 0.025)], meds[int(RESAMPLES * 0.975) - 1]


def verdict(lo, hi, pairs):
    """Apply the decision rule to the interval [LO, HI] of PAIRS pairs."""
    if pairs < MIN_PAIRS:
        return "few"
    if lo > SLOWER:
        return "slower"
    if hi < 1 / SLOWER:
        return "faster"
    if (hi - lo) / 2 > WIDE:
        return "wide"
    if lo >= 1 / SLOWER and hi <= SLOWER:
        return "same"
    return "inconclusive"


def load_groups(paths):
    """Group the measured records of the raw.jsonl files in PATHS.
    Return ({group: {pair: {build: record}}}, timed-out run count)."""
    groups = collections.defaultdict(dict)
    timed_out = 0
    for path in paths:
        if os.path.isdir(path):
            path = os.path.join(path, "raw.jsonl")
        with open(path, encoding="utf-8") as f:
            for line in f:
                rec = json.loads(line)
                if rec.get("is_warmup") or "build" not in rec:
                    continue
                if rec.get("timed_out"):
                    timed_out += 1
                    continue
                key = (rec.get("phase", "?"), rec["scenario"],
                       rec.get("threads"), rec.get("locale", "?"),
                       rec.get("regime", ""), bool(rec.get("precise")),
                       rec.get("sha_a", "?"), rec.get("sha_b", "?"))
                pair = (rec.get("session", path), rec["round"])
                groups[key].setdefault(pair, {})[rec["build"]] = rec
    return groups, timed_out


def output_key(rec):
    if rec.get("equiv_mode") == "exit_only":
        return rec["exit_code"]
    return (rec.get("stdout_sha256"), rec.get("stderr_sha256"),
            rec["exit_code"])


def summarize(paths):
    """Print the decision-rule table.  Return 1 if a group is slower or
    printed different output, 3 if a group needs more pairs, 0 if all
    are settled, and -1 if there is nothing to summarize."""
    groups, timed_out = load_groups(paths)
    if not groups:
        print("ab_bench: no measured A/B records to summarize")
        return -1
    counts = collections.Counter()
    print(f"{'phase':6} {'scenario':8} {'threads':>7} {'locale':11} "
          f"{'pairs':>5} {'B/A':>6}  {'95% CI':15}  verdict")
    for key in sorted(groups, key=str):
        pairs = [p for p in groups[key].values() if "A" in p and "B" in p]
        ratios = [p["B"]["wall_ms"] / p["A"]["wall_ms"] for p in pairs
                  if p["A"]["wall_ms"] > 0]
        if not ratios:
            continue
        seed = int(hashlib.sha256(repr(key).encode()).hexdigest()[:8], 16)
        lo, hi = bootstrap_ci(ratios, random.Random(seed))
        v = verdict(lo, hi, len(ratios))
        outputs = {b: {output_key(p[b]) for p in pairs} for b in "AB"}
        if outputs["A"] != outputs["B"]:
            v += ", OUTPUT DIFFERS"
            counts["differs"] += 1
        counts[v.split(",")[0]] += 1
        if lo > 1 and not v.startswith("slower"):
            v += ", B/A > 1"
        phase, sid, threads, locale, regime, precise, sha_a, sha_b = key
        th = "default" if threads is None else str(threads)
        print(f"{phase:6} {sid:8} {th:>7} {locale[:11]:11} "
              f"{len(ratios):5d} {statistics.median(ratios):6.3f}  "
              f"[{lo:.3f}, {hi:.3f}]  {v}")
    shas = {(k[6], k[7], k[5], k[4]) for k in groups}
    if len(shas) > 1:
        print("ab_bench: the records come from more than one pair of "
              "programs, regime or timing method; each is summarized "
              "on its own")
    if timed_out:
        print(f"ab_bench: left out {timed_out} timed-out run(s)")
    if counts["slower"]:
        print(f"ab_bench: {counts['slower']} group(s) slower; before "
              "treating that as a regression, rebuild B with "
              "-falign-functions=64 and run them again")
    more = counts["wide"] + counts["inconclusive"] + counts["few"]
    if more:
        print(f"ab_bench: {more} group(s) need more pairs; run again "
              "into the same directory")
    if counts["slower"] or counts["differs"]:
        return 1
    return 3 if more else 0


def run_benchmark(args):
    """Run the A/B benchmark; return the number of output mismatches."""
    if args.precise:
        kb.run_process_timed = precise_run_process_timed
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.tmp_dir, exist_ok=True)
    bin_dir = tempfile.mkdtemp(prefix="ab-bin-", dir=args.tmp_dir)
    try:
        builds = {}
        shas = {}
        for key, path in (("A", args.a), ("B", args.b)):
            os.mkdir(os.path.join(bin_dir, key))
            builds[key] = os.path.join(bin_dir, key, "grep")
            shutil.copy2(os.path.realpath(path), builds[key])
            with open(builds[key], "rb") as f:
                shas[key] = hashlib.sha256(f.read()).hexdigest()[:16]
        return run_scenarios(args, builds, shas)
    finally:
        shutil.rmtree(bin_dir)


def run_scenarios(args, builds, shas):
    """Run the scenarios with the programs in BUILDS (by "A" and "B"),
    whose SHA-256 prefixes are SHAS; return the number of output
    mismatches."""
    allsc = make_scenarios()
    if args.scenarios:
        sids = [s.strip() for s in args.scenarios.split(",") if s.strip()]
        unknown = [s for s in sids if s not in allsc]
        if unknown:
            raise ValueError(f"unknown scenarios: {', '.join(unknown)}")
        scens = [allsc[s] for s in sids]
    else:
        scens = sorted(allsc.values(),
                       key=lambda s: (s.category == "P", s.sid))
    locale = "C" if args.phase == "c" else "en_US.UTF-8"
    if args.phase == "sweep":
        thread_list = [int(t) for t in args.threads.split(",")]
    elif args.phase == "fixed":
        thread_list = [int(args.threads)]
    else:
        thread_list = [None]
    cache_state = "cold" if args.phase == "cold" else "warm"
    affinity = sorted(os.sched_getaffinity(0))
    big = os.path.join(args.kbench_dir, "linux-all.c")
    session = time.time_ns()
    mismatches = []

    raw = open(os.path.join(args.out, "raw.jsonl"), "a", encoding="utf-8")

    def run(scen, build, threads, r, warm, gate_wait):
        rec = kb.execute_run(
            scenario=scen, tool="G1", bin_path=builds[build],
            ksrc=args.ksrc, kbench_dir=args.kbench_dir,
            patterns_dir=args.patterns_dir, repo_dir=args.repo,
            locale=locale, threads=threads, round_idx=r, is_warmup=warm,
            tmp_dir=args.tmp_dir, timeout_sec=scen.timeout_sec or 600,
            cache_state=cache_state, idle_gate_wait_s=gate_wait)
        if rec is None:
            return None
        rec.update(build=build, phase=args.phase, regime=args.regime,
                   precise=args.precise, session=session,
                   sha_a=shas["A"], sha_b=shas["B"])
        raw.write(json.dumps(rec) + "\n")
        raw.flush()
        return rec

    if args.phase != "cold":
        print("pre-warming", big)
        kb.pre_warm_cache([big])

    t_start = time.time()
    for i, scen in enumerate(scens, 1):
        w_def, r_def = default_schedule(scen, args.phase)
        warmups = w_def if args.warmups is None else args.warmups
        rounds = r_def if args.rounds is None else args.rounds
        for threads in thread_list:
            _, gate_wait = kb.check_idle_gate(args.idle_gate, affinity)
            outputs = {"A": set(), "B": set()}
            walls = {"A": [], "B": []}
            for w in range(warmups):
                for build in ("AB" if w % 2 == 0 else "BA"):
                    run(scen, build, threads, -1 - w, True, gate_wait)
            for r in range(rounds):
                for build in ("AB" if r % 2 == 0 else "BA"):
                    if args.phase == "cold":
                        target = [big] if scen.category == "B" else [
                            args.ksrc]
                        kb.check_idle_gate(args.idle_gate, affinity)
                        evict_paths(target)
                        check_fincore_sample(target, sample_size=200,
                                             threshold=0.02)
                    rec = run(scen, build, threads, r, False, gate_wait)
                    if rec is None:
                        continue
                    if scen.equiv_mode == "exit_only":
                        key = rec["exit_code"]
                    else:
                        key = (rec["stdout_sha256"], rec["exit_code"])
                    outputs[build].add(key)
                    walls[build].append(rec["wall_ms"])
            same = outputs["A"] == outputs["B"] and len(outputs["A"]) == 1
            if not same:
                mismatches.append((scen.sid, threads, locale))
            med = {b: statistics.median(v) if v else float("nan")
                   for b, v in walls.items()}
            th = "default" if threads is None else str(threads)
            ratio = med["B"] / med["A"] if med["A"] else float("nan")
            print(f"[{args.phase} {i}/{len(scens)}] {scen.sid:6s} "
                  f"threads={th:>7s} A {med['A']:9.2f} ms  "
                  f"B {med['B']:9.2f} ms  B/A {ratio:.3f}  output "
                  f"{'same' if same else 'DIFFERENT'}", flush=True)
    raw.close()
    print(f"done in {time.time() - t_start:.0f} s; output mismatches: "
          f"{mismatches or 'none'}")
    return len(mismatches)


def main(argv=None):
    shm = "/dev/shm" if os.path.isdir("/dev/shm") else (
        tempfile.gettempdir())
    ap = argparse.ArgumentParser(
        description="Interleaved A/B timing of two grep builds on the "
        "kernel scenarios.")
    ap.add_argument("dirs", nargs="*", metavar="DIR",
                    help="result directories to summarize")
    ap.add_argument("--a", help="build A binary (baseline)")
    ap.add_argument("--b", help="build B binary (candidate)")
    ap.add_argument("--out", help="result directory (raw.jsonl is "
                    "appended to)")
    ap.add_argument("--phase", default="warm",
                    choices=["warm", "c", "sweep", "fixed", "cold"])
    ap.add_argument("--scenarios", default="",
                    help="comma-separated scenario ids (default: all)")
    ap.add_argument("--threads", default="",
                    help="sweep: comma-separated counts; fixed: one count")
    ap.add_argument("--warmups", type=int, default=None)
    ap.add_argument("--rounds", type=int, default=None,
                    help="measured A/B pairs per scenario")
    ap.add_argument("--regime", default="",
                    help="free-form label stored in each record")
    ap.add_argument("--idle-gate", default="cpuset",
                    choices=["cpuset", "loadavg", "off"])
    ap.add_argument("--precise", action="store_true",
                    help="time with a blocking wait4 instead of the "
                    "harness's 1 ms polling")
    ap.add_argument("--summarize", action="store_true",
                    help="apply the A/B decision rule to the results")
    ap.add_argument("--repo",
                    default=os.path.abspath(os.path.join(SCRIPT_DIR,
                                                         "..", "..")),
                    help="repository root, scrubbed from recorded "
                    "commands")
    ap.add_argument("--ksrc", default=os.environ.get(
        "K_SRC", os.path.expanduser("~/p/s/linux-stable")),
        help="Linux kernel source tree")
    ap.add_argument("--kbench-dir", default=os.environ.get(
        "KBENCH_DIR", os.path.expanduser("~/p/s/kbench")),
        help="generated corpus directory")
    ap.add_argument("--patterns-dir",
                    default=os.path.join(SCRIPT_DIR, "patterns"),
                    help="pattern files directory")
    ap.add_argument("--tmp-dir", default=os.path.join(shm, "kbench_out"),
                    help="scratch directory for run output")
    args = ap.parse_args(argv)

    running = args.a or args.b
    problem = None
    if running and not (args.a and args.b and args.out):
        problem = "a benchmark run needs --a, --b and --out"
    elif running and args.phase in ("sweep", "fixed") and not args.threads:
        problem = f"--phase {args.phase} needs --threads"
    elif not running and not (args.summarize and (args.dirs or args.out)):
        problem = "give --a, --b and --out to run, or --summarize DIR..."
    if problem:
        ap.error(problem)
    status = 0
    try:
        if running and run_benchmark(args):
            status = 1
        if args.summarize:
            paths = list(args.dirs) + ([args.out] if args.out else [])
            result = summarize(paths)
            if result < 0:
                status = 2
            elif status == 0 or result == 1:
                status = result
    except (OSError, ValueError) as e:
        print(f"ab_bench: {e}", file=sys.stderr)
        return 2
    return status


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:          # a bug, not a difference
        traceback.print_exc()
        sys.exit(2)
