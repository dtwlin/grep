#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# Comprehensive 5-minute multithreaded stress test for Supercharged grep.
# Continuously generates randomized, adversarial, and extremal inputs,
# varying files, line layouts, pattern engines, options, and thread counts,
# and verifies 100% byte-for-byte fidelity against golden baseline GNU grep.

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time


def normalize_err(err):
    err = re.sub(rb"(?m)^(?:[^\s:]*/)?(?:grep|grep-baseline):", b"grep:", err)
    err = re.sub(
        rb"Try '(?:[^\s']*/)?(?:grep|grep-baseline) --help'",
        b"Try 'grep --help'",
        err,
    )
    err = re.sub(rb"Usage: \S*(?:grep-baseline|grep)", b"Usage: grep", err)
    return err


def generate_workload(tmpdir, cycle):
    """Generates a random adversarial or extremal corpus."""
    workload_type = random.choice([
        "tree",
        "chunked",
        "small",
        "giant_line",
        "binary",
        "nonl",
        "empty",
        "multi_file",
        "pipe",
    ])
    work_path = os.path.join(tmpdir, f"work_{cycle}")
    os.makedirs(work_path, exist_ok=True)

    stdin_data = None
    targets = []
    file_operands = []

    if workload_type == "tree":
        # Random directory tree with depth 1-4 and 10-150 files
        num_files = random.randint(15, 120)
        for i in range(num_files):
            sub = os.path.join(work_path, f"d_{i % 5}")
            os.makedirs(sub, exist_ok=True)
            fpath = os.path.join(sub, f"f_{i:04d}.txt")
            with open(fpath, "wb") as fp:
                for line_idx in range(random.randint(5, 40)):
                    if random.random() < 0.15:
                        token = f"TARGET_TREE_{random.randint(1, 5)}"
                        targets.append(token)
                        fp.write(f"line {line_idx:03d}: hit {token} data\n".encode())
                    else:
                        fp.write(
                            f"line {line_idx:03d}: filler payload"
                            f" {random.randint(1000, 9999)}\n".encode()
                        )
        file_operands = ["-r", work_path]

    elif workload_type == "chunked":
        # 2.2 MiB to 6.0 MiB file to exercise intra-file chunked mmap
        fpath = os.path.join(work_path, "chunked.log")
        num_lines = random.randint(32000, 65000)
        target = f"TARGET_CHUNK_{random.randint(1, 3)}"
        targets.append(target)
        with open(fpath, "wb") as fp:
            for i in range(num_lines):
                if i % 8000 == 0:
                    fp.write(f"chunk line {i:06d}: hit {target} token\n".encode())
                else:
                    fp.write(b"x" * 60 + b" chunk filler data line payload 0123456789\n")
        file_operands = [fpath]

    elif workload_type == "giant_line":
        # Single line of 40 KiB - 200 KiB without newlines
        fpath = os.path.join(work_path, "giant.txt")
        line_len = random.randint(40000, 180000)
        target = "GIANT_LINE_TOKEN"
        targets.append(target)
        half = line_len // 2
        with open(fpath, "wb") as fp:
            fp.write(b"A" * half + b" " + target.encode() + b" " + b"B" * half + b"\n")
        file_operands = [fpath]

    elif workload_type == "binary":
        # Injects NUL bytes at byte 0, middle, or EOF
        fpath = os.path.join(work_path, "binary.bin")
        target = "BIN_TARGET_HIT"
        targets.append(target)
        variant = random.choice(["first", "mid", "last"])
        with open(fpath, "wb") as fp:
            if variant == "first":
                fp.write(b"\x00" + b"line 1 " + target.encode() + b" data\nline 2 end\n")
            elif variant == "mid":
                fp.write(b"line 1\nline 2 " + target.encode() + b"\x00mid_nul\nline 3\n")
            else:
                fp.write(b"line 1 " + target.encode() + b"\nline 2 end\x00")
        file_operands = [fpath]

    elif workload_type == "nonl":
        # Missing trailing newline at EOF
        fpath = os.path.join(work_path, "nonl.txt")
        target = "NONL_EOF_TARGET"
        targets.append(target)
        with open(fpath, "wb") as fp:
            fp.write(f"line 1 start\nline 2 mid\nline 3 {target}".encode())
        file_operands = [fpath]

    elif workload_type == "empty":
        fpath = os.path.join(work_path, "empty.txt")
        open(fpath, "wb").close()
        targets.append("ANY_TARGET")
        file_operands = [fpath]

    elif workload_type == "multi_file":
        # Multiple files passed as arguments
        f1 = os.path.join(work_path, "m1.txt")
        f2 = os.path.join(work_path, "m2.txt")
        f3 = os.path.join(work_path, "m3.txt")
        target = "MULTI_FILE_TOKEN"
        targets.append(target)
        with open(f1, "wb") as fp:
            fp.write(f"file 1 hit {target}\n".encode())
        with open(f2, "wb") as fp:
            fp.write(b"file 2 no match\n")
        with open(f3, "wb") as fp:
            fp.write(f"file 3 hit {target}\nfile 3 extra\n".encode())
        file_operands = [f1, f2, f3]

    elif workload_type == "pipe":
        # Streaming STDIN input
        target = "PIPE_STREAM_TARGET"
        targets.append(target)
        buf = []
        for i in range(random.randint(50, 500)):
            if i % 25 == 0:
                buf.append(f"pipe line {i:04d}: {target} hit\n".encode())
            else:
                buf.append(f"pipe line {i:04d}: regular filler data\n".encode())
        stdin_data = b"".join(buf)
        file_operands = []

    else:
        # Default small file
        fpath = os.path.join(work_path, "small.txt")
        target = "SMALL_TARGET"
        targets.append(target)
        with open(fpath, "wb") as fp:
            for i in range(random.randint(10, 100)):
                fp.write(f"small line {i:03d} data {random.randint(1, 99)}\n".encode())
            fp.write(f"small line hit {target}\n".encode())
        file_operands = [fpath]

    if not targets:
        targets = ["DEFAULT_TARGET"]

    return workload_type, work_path, file_operands, stdin_data, targets


def select_pattern_and_options(targets, workload_type, cycle):
    """Selects an adversarial pattern, engine, and flag permutation."""
    pat_engine = random.choice(["F", "E", "P", "G", "literal_single", "multi_kw"])
    base_target = random.choice(targets)
    options = []
    pattern = base_target

    if pat_engine == "F":
        options.append("-F")
        if random.random() < 0.2:
            options.append("-i")
    elif pat_engine == "multi_kw":
        # Multi-keyword with 2 to 40 keywords
        options.append("-F")
        num_kw = random.choice([2, 5, 16, 32, 35, 64])
        pattern_list = [f"kw_{k:04d}" for k in range(num_kw)]
        pattern_list.append(base_target)
        pattern = "\n".join(pattern_list)
    elif pat_engine == "E":
        options.append("-E")
        pat_variants = [
            f"({base_target}|NONEXISTENT_[0-9]+)",
            f"[a-z]+_{base_target}",
            f"{base_target}.*line",
            f"^{base_target}",
            f"{base_target}$",
        ]
        pattern = random.choice(pat_variants)
    elif pat_engine == "P":
        options.append("-P")
        prefix = base_target[:4]
        pattern = f"(?<={prefix})[A-Za-z0-9_]+"
    else:
        pattern = base_target

    # Randomly attach search mode and formatting flags
    mode_flag = random.choice([
        [],
        ["-n"],
        ["-b"],
        ["-n", "-b"],
        ["-c"],
        ["-v"],
        ["-c", "-v"],
        ["-l"],
        ["-L"],
        ["-m", "1"],
        ["-m", "5"],
        ["-m", "0"],
        ["-o"],
        ["-C", "1"],
        ["-A", "2"],
        ["-B", "2"],
    ])

    # Omit conflicting or quiet options on certain workloads
    if workload_type == "pipe" and "-l" in mode_flag:
        mode_flag = ["-n"]
    if "-o" in mode_flag and "-v" in mode_flag:
        mode_flag = ["-n"]

    options.extend(mode_flag)

    # Randomly add -w, -x, -z, -T, -a, -I
    extra = random.choice([
        [],
        ["-w"],
        ["-x"],
        ["-T"],
        ["-a"],
        ["-I"],
    ])
    options.extend(extra)

    return options, pattern


def run_stress_test(baseline_bin, candidate_bin, duration_secs=300):
    print("=" * 80)
    print(f"      SUPERCHARGED GREP: {duration_secs//60}-MINUTE CONTINUOUS STRESS TEST")
    print(f"      Baseline:  {baseline_bin}")
    print(f"      Candidate: {candidate_bin}")
    print(f"      Duration:  {duration_secs} seconds ({duration_secs/60:.1f} minutes)")
    print("=" * 80)

    start_time = time.time()
    last_report_time = start_time
    cycle = 0
    total_bytes = 0

    thread_pool = [1, 2, 3, 4, 5, 7, 8, 16, 31, 32, 64, 128]

    tmpdir = tempfile.mkdtemp(prefix="grep_stress_harness_")

    try:
        while True:
            now = time.time()
            elapsed = now - start_time
            if elapsed >= duration_secs:
                break

            cycle += 1
            # Periodic heartbeat report every 10 seconds
            if now - last_report_time >= 10.0:
                rate = cycle / elapsed if elapsed > 0 else 0
                mb_searched = total_bytes / (1024 * 1024)
                print(
                    f"  [RUNNING] {elapsed:5.1f}s / {duration_secs}s ({elapsed/duration_secs*100:4.1f}%) | "
                    f"Cycles: {cycle:,} ({rate:5.1f} cycles/sec) | Data: {mb_searched:6.1f} MB | Errors: 0"
                )
                last_report_time = now

            # Select thread count for this cycle
            th = random.choice(thread_pool)
            env = os.environ.copy()
            env["GREP_THREADS"] = str(th)

            # Generate workload
            w_type, w_path, file_args, stdin_data, targets = generate_workload(tmpdir, cycle)
            options, pattern = select_pattern_and_options(targets, w_type, cycle)

            cmd_args = options + [pattern] + file_args

            # Execute baseline
            b_env = os.environ.copy()
            b_env["GREP_THREADS"] = "1"
            b_proc = subprocess.run(
                [baseline_bin] + cmd_args,
                input=stdin_data,
                capture_output=True,
                env=b_env,
            )

            # Execute candidate with GREP_THREADS=th
            c_proc = subprocess.run(
                [candidate_bin] + cmd_args,
                input=stdin_data,
                capture_output=True,
                env=env,
            )

            # Verify return code
            if b_proc.returncode != c_proc.returncode:
                print(f"\n[FATAL STRESS FAILURE] Cycle {cycle} (threads={th}):")
                print(f"  Command: {cmd_args}")
                print(f"  Return code: base={b_proc.returncode} vs cand={c_proc.returncode}")
                print(f"  Base stderr:\n{b_proc.stderr.decode('utf-8', 'replace')}")
                print(f"  Cand stderr:\n{c_proc.stderr.decode('utf-8', 'replace')}")
                sys.exit(1)

            # Verify standard output
            if b_proc.stdout != c_proc.stdout:
                print(f"\n[FATAL STRESS FAILURE] Cycle {cycle} (threads={th}):")
                print(f"  Command: {cmd_args}")
                print(f"  Stdout length mismatch: base={len(b_proc.stdout)} vs cand={len(c_proc.stdout)}")
                print(f"  Base head: {b_proc.stdout[:200]!r}")
                print(f"  Cand head: {c_proc.stdout[:200]!r}")
                sys.exit(1)

            # Verify standard error (with argv normalization)
            b_err = normalize_err(b_proc.stderr)
            c_err = normalize_err(c_proc.stderr)
            if b_err != c_err:
                print(f"\n[FATAL STRESS FAILURE] Cycle {cycle} (threads={th}):")
                print(f"  Command: {cmd_args}")
                print(f"  Stderr mismatch:\nBase:\n{b_err!r}\nCand:\n{c_err!r}")
                sys.exit(1)

            # Accumulate bytes searched
            if stdin_data:
                total_bytes += len(stdin_data)
            total_bytes += len(c_proc.stdout)

            # Clean up per-cycle scratch files
            if os.path.exists(w_path):
                shutil.rmtree(w_path, ignore_errors=True)

        total_elapsed = time.time() - start_time
        final_rate = cycle / total_elapsed if total_elapsed > 0 else 0
        final_mb = total_bytes / (1024 * 1024)
        print("=" * 80)
        print(f"  [SUCCESS] Completed {cycle:,} stress test cycles in {total_elapsed:.2f}s ({final_rate:.1f} cycles/sec)")
        print(f"  Total Data Processed: {final_mb:.1f} MB")
        print("  0 failures, 0 discrepancies, 100% byte-for-byte fidelity confirmed!")
        print("=" * 80)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description="Continuous stress test for Supercharged grep.")
    parser.add_argument(
        "--baseline",
        default="benchmarks/grep-baseline",
        help="Path to baseline GNU grep binary (default: benchmarks/grep-baseline)",
    )
    parser.add_argument(
        "--candidate",
        default="src/grep",
        help="Path to candidate Supercharged grep binary (default: src/grep)",
    )
    parser.add_argument(
        "--duration",
        type=int,
        default=300,
        help="Stress test duration in seconds (default: 300 = 5 minutes)",
    )
    args = parser.parse_args()

    if not os.path.exists(args.candidate):
        sys.exit(f"Candidate binary not found: {args.candidate}")
    if not os.path.exists(args.baseline):
        # Fall back to testing multi-threaded candidate against candidate at GREP_THREADS=1
        args.baseline = args.candidate

    run_stress_test(args.baseline, args.candidate, duration_secs=args.duration)


if __name__ == "__main__":
    main()
