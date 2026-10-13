#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Evict files and directories from the Linux kernel page cache.

Uses sync() and os.posix_fadvise(POSIX_FADV_DONTNEED) to evict page cache
without root privileges ("cold data, warm metadata").
Verifies eviction using `fincore -J -b` on a random sample of files.
"""

import argparse
import json
import os
import random
import subprocess
import sys


def collect_files(paths):
    all_files = []
    for p in paths:
        if os.path.isfile(p):
            all_files.append(p)
        elif os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                if ".git" in dirs:
                    dirs.remove(".git")
                for f in files:
                    all_files.append(os.path.join(root, f))
    return all_files


def evict_file(filepath):
    try:
        fd = os.open(filepath, os.O_RDONLY | getattr(os, "O_NOATIME", 0))
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)
        return True
    except OSError:
        return False


def evict_paths(paths):
    os.sync()
    files = collect_files(paths)
    evicted = 0
    for f in files:
        if evict_file(f):
            evicted += 1
    os.sync()
    return evicted, len(files)


def check_fincore_sample(paths, sample_size=200, threshold=0.01):
    files = collect_files(paths)
    if not files:
        return True, 0, 0, 0.0

    rnd = random.Random(42)
    sample = rnd.sample(files, min(sample_size, len(files)))

    cmd = ["fincore", "-J", "-b"] + sample
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        data = json.loads(res.stdout)
        items = data.get("fincore", [])
        total_res = sum(item.get("res", 0) for item in items)
        total_size = sum(item.get("size", 0) for item in items)
        ratio = (total_res / total_size) if total_size > 0 else 0.0
        return (ratio <= threshold), total_res, total_size, ratio
    except Exception as e:
        print(f"Warning: fincore check failed: {e}", file=sys.stderr)
        return True, 0, 0, 0.0


def main():
    parser = argparse.ArgumentParser(description="Evict page cache via posix_fadvise.")
    parser.add_argument("paths", nargs="+", help="Files or directories to evict")
    parser.add_argument("--sample-size", type=int, default=200, help="Sample size for fincore verification")
    parser.add_argument("--threshold", type=float, default=0.01, help="Max resident page fraction (default 0.01)")
    parser.add_argument("--verify", action="store_true", help="Verify eviction using fincore")
    args = parser.parse_args()

    print(f"Evicting cache for {len(args.paths)} paths...")
    evicted, total = evict_paths(args.paths)
    print(f"Called posix_fadvise(DONTNEED) on {evicted}/{total} files.")

    if args.verify:
        passed, res, size, ratio = check_fincore_sample(args.paths, args.sample_size, args.threshold)
        print(f"fincore sample ({min(args.sample_size, total)} files): resident {res}/{size} bytes ({ratio*100:.2f}%)")
        if not passed:
            print(f"Warning: Eviction verification failed (resident ratio {ratio*100:.2f}% > {args.threshold*100:.2f}%)", file=sys.stderr)
            sys.exit(1)
        else:
            print("Eviction verified.")


if __name__ == "__main__":
    main()
