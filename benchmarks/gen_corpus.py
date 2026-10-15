#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""
Generate deterministic benchmark corpus for GNU Grep multi-threaded benchmarks.
Target directory: /dev/shm/grep_bench
Uses only generic synthetic data (example.com, etc.).
"""

import os
import shutil
import sys
import tempfile
import time


def _default_bench_dir():
    if os.environ.get("GREP_BENCH_DIR"):
        return os.environ["GREP_BENCH_DIR"]
    if os.path.exists("/dev/shm"):
        try:
            if shutil.disk_usage("/dev/shm").free >= int(2.5 * 1024 ** 3):
                return "/dev/shm/grep_bench"
        except OSError:
            pass
    return os.path.join(tempfile.gettempdir(), "grep_bench")


BENCH_DIR = _default_bench_dir()

def generate_corpus():
    start_time = time.time()
    if os.path.exists(BENCH_DIR):
        shutil.rmtree(BENCH_DIR)
    os.makedirs(BENCH_DIR, exist_ok=True)

    single_file = os.path.join(BENCH_DIR, "single_2GB.log")
    tree_dir = os.path.join(BENCH_DIR, "tree_10k")
    os.makedirs(tree_dir, exist_ok=True)

    print("Generating single_2GB.log (~1.2 GiB)...")
    # Base synthetic lines (~100-120 bytes each)
    base_lines = [
        "2026-10-10T08:00:01.100Z server-node-01.example.com app[1001]: INFO status=ok method=GET uri=/api/v2/items?id=10291 ip=192.0.2.1 latency=1.2ms\n",
        "2026-10-10T08:00:01.105Z server-node-02.example.com app[1002]: DEBUG connection established client=client-pool-A id=48102 pool_size=64\n",
        "2026-10-10T08:00:01.110Z server-node-03.example.com worker[1003]: INFO background sync completed batch=9941 duration=14ms processed=500\n",
        "2026-10-10T08:00:01.115Z server-node-01.example.com app[1001]: INFO status=ok method=POST uri=/api/v2/update user=regular_user_102 code=200\n",
        "2026-10-10T08:00:01.120Z server-node-04.example.com router[1004]: WARN queue pressure high queue=ingress_events length=128 threshold=100\n",
        "2026-10-10T08:00:01.125Z server-node-02.example.com app[1002]: INFO user session authenticated session=sess_982144 user=regular_user_103\n",
        "2026-10-10T08:00:01.130Z server-node-05.example.com db[1005]: INFO query executed sql=\"SELECT id, name FROM items WHERE valid = 1\" rows=42\n",
        "2026-10-10T08:00:01.135Z server-node-03.example.com app[1003]: DEBUG cache lookup hit key=config:tenant:primary ttl=299s size=1024b\n",
    ]
    error_line = "2026-10-10T08:00:01.140Z server-node-01.example.com app[1001]: ERROR ERROR_1042_HANDLER failed processing request err_code=500\n"

    # Build a 16 MiB block with periodic errors (~200 errors per block)
    block_lines = []
    for i in range(160000):
        if i % 800 == 0:
            block_lines.append(error_line)
        else:
            block_lines.append(base_lines[i % len(base_lines)])
    block_bytes = "".join(block_lines).encode("utf-8")
    block_size = len(block_bytes)

    # We write 75 blocks -> ~1.25 GiB total (~12M lines, ~15k error lines)
    total_blocks = 75
    # Deterministic injection of rare tokens across blocks
    rare_token_block_indices = set(range(2, total_blocks, 3)) # 25 hits
    backref_token_indices = set(range(1, total_blocks, 2))    # 37 hits
    pcre_token_indices = set(range(0, total_blocks, 1))       # 75 hits

    rare_line = b"2026-10-10T08:00:01.999Z server-node-99.example.com audit[9999]: ALERT ULTRA_RARE_TOKEN_9941 trigger anomaly score=99.9\n"
    backref_line = b"2026-10-10T08:00:01.998Z server-node-99.example.com audit[9999]: INFO check pair=aaaa_aaaa_9941 match=confirmed\n"
    pcre_line = b"2026-10-10T08:00:01.997Z server-node-99.example.com auth[9999]: NOTICE login granted user=admin_0042 role=superuser ip=192.0.2.99\n"

    with open(single_file, "wb", buffering=16 * 1024 * 1024) as f:
        for b_idx in range(total_blocks):
            f.write(block_bytes)
            if b_idx in rare_token_block_indices:
                f.write(rare_line)
            if b_idx in backref_token_indices:
                f.write(backref_line)
            if b_idx in pcre_token_indices:
                f.write(pcre_line)

    single_size = os.path.getsize(single_file)
    print(f"Generated {single_file}: {single_size / (1024*1024):.1f} MiB")

    print("Generating tree_10k (5,000 files across 100 directories)...")
    # Subdirectories
    num_dirs = 100
    files_per_dir = 50
    # Prepare 4 file content templates: 16 KiB, 64 KiB, 128 KiB, 256 KiB
    content_templates = [
        block_bytes[:16 * 1024],
        block_bytes[:64 * 1024],
        block_bytes[:128 * 1024],
        block_bytes[:256 * 1024],
    ]

    total_tree_files = 0
    total_tree_bytes = 0

    for d in range(num_dirs):
        dir_path = os.path.join(tree_dir, f"dir_{d:03d}")
        os.makedirs(dir_path, exist_ok=True)
        for f_idx in range(files_per_dir):
            file_name = f"file_{d:03d}_{f_idx:03d}.log"
            file_path = os.path.join(dir_path, file_name)
            template_idx = (d + f_idx) % len(content_templates)
            c = content_templates[template_idx]

            # Injected targets
            injections = []
            if (d * files_per_dir + f_idx) % 200 == 0:
                injections.append(rare_line)
            if (d * files_per_dir + f_idx) % 10 == 0:
                injections.append(b"2026-10-10T08:00:00Z server.example.com app[1]: QUERY_PATTERN_ALPHA matched id=1\n")
            if (d * files_per_dir + f_idx) % 15 == 0:
                injections.append(b"2026-10-10T08:00:00Z server.example.com app[2]: QUERY_PATTERN_BETA matched id=2\n")

            if injections:
                payload = c + b"".join(injections)
            else:
                payload = c

            with open(file_path, "wb") as f_out:
                f_out.write(payload)
            total_tree_files += 1
            total_tree_bytes += len(payload)

    elapsed = time.time() - start_time
    print(f"Generated {total_tree_files} files in tree: {total_tree_bytes / (1024*1024):.1f} MiB")
    print(f"Corpus generation completed in {elapsed:.2f} seconds.")

if __name__ == "__main__":
    generate_corpus()
