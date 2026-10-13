#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Generate single-file and operand-list corpora from the Linux kernel tree.

Builds:
  - linux-all.c ($BIG): deterministic concatenation of all tracked *.c then *.h
  - big-24M.h: largest real source header (dcn_3_2_0_sh_mask.h, 24MB)
  - files.z: null-delimited list of all tracked files (git ls-files -z)
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

BIG_HEADER_REL = "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_2_0_sh_mask.h"


def sha256_file(path, chunk_size=4 * 1024 * 1024):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def generate_corpus(ksrc, out_dir, force=False):
    os.makedirs(out_dir, exist_ok=True)
    manifest = {}

    # 1. files.z
    files_z_path = os.path.join(out_dir, "files.z")
    if force or not os.path.exists(files_z_path):
        print("Generating files.z via git ls-files -z...")
        t0 = time.time()
        res = subprocess.run(
            ["git", "ls-files", "-z"],
            cwd=ksrc,
            capture_output=True,
            check=True,
        )
        with open(files_z_path, "wb") as f:
            f.write(res.stdout)
        t1 = time.time()
        print(f"Generated files.z in {t1 - t0:.2f}s ({len(res.stdout)} bytes)")
    size_z = os.path.getsize(files_z_path)
    sha_z = sha256_file(files_z_path)
    manifest["files.z"] = {"path": files_z_path, "size": size_z, "sha256": sha_z}

    # 2. big-24M.h
    big_header_src = os.path.join(ksrc, BIG_HEADER_REL)
    big_header_dst = os.path.join(out_dir, "big-24M.h")
    if not os.path.exists(big_header_src):
        print(f"Error: Could not find {big_header_src}", file=sys.stderr)
        sys.exit(1)
    if force or not os.path.exists(big_header_dst):
        print(f"Copying {BIG_HEADER_REL} -> big-24M.h...")
        shutil.copy2(big_header_src, big_header_dst)
    size_h = os.path.getsize(big_header_dst)
    sha_h = sha256_file(big_header_dst)
    manifest["big-24M.h"] = {"path": big_header_dst, "size": size_h, "sha256": sha_h}
    print(f"big-24M.h: {size_h} bytes (SHA256: {sha_h})")

    # 3. linux-all.c ($BIG)
    big_c_path = os.path.join(out_dir, "linux-all.c")
    if force or not os.path.exists(big_c_path):
        print("Finding all tracked *.c and *.h files...")
        t0 = time.time()
        res_c = subprocess.run(
            ["git", "ls-files", "*.c"],
            cwd=ksrc,
            capture_output=True,
            text=True,
            check=True,
        )
        res_h = subprocess.run(
            ["git", "ls-files", "*.h"],
            cwd=ksrc,
            capture_output=True,
            text=True,
            check=True,
        )
        c_files = res_c.stdout.splitlines()
        h_files = res_h.stdout.splitlines()
        total_files = len(c_files) + len(h_files)
        print(f"Concatenating {len(c_files)} .c and {len(h_files)} .h files into {big_c_path}...")

        tmp_out = big_c_path + ".tmp"
        written_bytes = 0
        buf_size = 4 * 1024 * 1024
        with open(tmp_out, "wb") as out_fp:
            for rel in c_files + h_files:
                fpath = os.path.join(ksrc, rel)
                try:
                    with open(fpath, "rb") as in_fp:
                        while True:
                            chunk = in_fp.read(buf_size)
                            if not chunk:
                                break
                            out_fp.write(chunk)
                            written_bytes += len(chunk)
                except OSError as e:
                    print(f"Warning: Failed to read {fpath}: {e}", file=sys.stderr)
        os.replace(tmp_out, big_c_path)
        t1 = time.time()
        print(f"Generated linux-all.c ({written_bytes / (1024*1024):.1f} MB) in {t1 - t0:.2f}s")

    size_big = os.path.getsize(big_c_path)
    sha_big = sha256_file(big_c_path)
    manifest["linux-all.c"] = {"path": big_c_path, "size": size_big, "sha256": sha_big}
    print(f"linux-all.c: {size_big} bytes (SHA256: {sha_big})")

    # Save manifest
    manifest_path = os.path.join(out_dir, "corpus_manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print(f"Corpus manifest saved to {manifest_path}")
    return manifest


def main():
    parser = argparse.ArgumentParser(description="Generate Linux kernel benchmark corpora.")
    parser.add_argument(
        "--ksrc",
        default=os.environ.get("K_SRC", os.path.expanduser("~/p/s/linux-stable")),
        help="Path to Linux kernel source tree",
    )
    parser.add_argument(
        "--out-dir",
        default=os.environ.get("KBENCH_DIR", os.path.expanduser("~/p/s/kbench")),
        help="Destination directory for generated corpora",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force regeneration of corpus files even if they already exist",
    )
    args = parser.parse_args()

    if not os.path.isdir(args.ksrc):
        print(f"Error: Linux source tree not found at {args.ksrc}", file=sys.stderr)
        sys.exit(1)

    generate_corpus(args.ksrc, args.out_dir, force=args.force)


if __name__ == "__main__":
    main()
