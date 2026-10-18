#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Merge one benchmark session's runs of some tools into a results directory.

A contestant added to an existing results directory is measured in a
session directory of its own, because kernel_bench.py rewrites env.json in
its output directory.  This script appends that session's records for the
selected tools to the destination's raw.jsonl, tagging each with the
session name, copies only those tools' entries of env.json and
diff_samples.json, and regenerates the reports.  The other tools' records
and metadata are left alone.
"""

import argparse
import json
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOL_ORDER = ("G0", "G1", "R0", "U0", "G2", "X1")


def read_records(path):
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def ordered(d):
    """Return D with the known tools first, in TOOL_ORDER."""
    keys = [k for k in TOOL_ORDER if k in d] + [k for k in d if k not in TOOL_ORDER]
    return {k: d[k] for k in keys}


def load_json(path, default):
    if not os.path.exists(path):
        return default
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2)
    os.replace(tmp, path)


def main():
    parser = argparse.ArgumentParser(description="Merge a session's runs of some tools into a results directory.")
    parser.add_argument("--src", required=True, help="Session results directory")
    parser.add_argument("--dst", required=True, help="Results directory to merge into")
    parser.add_argument("--tools", required=True, help="Comma-separated tools whose runs are merged, e.g. U0")
    parser.add_argument("--equiv", default=None, help="Directory whose diff_samples.json holds the tools' Gate B diff samples (default: --src)")
    parser.add_argument("--note", default="", help="Note recorded with the merged session in env.json")
    parser.add_argument("--replace", action="store_true", help="Drop the destination's existing runs of these tools first")
    parser.add_argument("--no-report", action="store_true", help="Do not regenerate the reports")
    args = parser.parse_args()

    tools = {t.strip() for t in args.tools.split(",") if t.strip()}
    session = os.path.basename(os.path.normpath(args.src))
    dst_raw = os.path.join(args.dst, "raw.jsonl")

    existing = read_records(dst_raw)
    present = sorted({r["tool"] for r in existing} & tools)
    if present and not args.replace:
        sys.exit(f"error: {args.dst} already has runs of {', '.join(present)}; use --replace to drop them")
    kept = [r for r in existing if r["tool"] not in tools]

    added = []
    for r in read_records(os.path.join(args.src, "raw.jsonl")):
        if r["tool"] in tools:
            r["session"] = session
            added.append(r)
    if not added:
        sys.exit(f"error: {args.src} has no runs of {', '.join(sorted(tools))}")

    tmp = dst_raw + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        for r in kept + added:
            f.write(json.dumps(r) + "\n")
    os.replace(tmp, dst_raw)

    src_env = load_json(os.path.join(args.src, "env.json"), {})
    dst_env_path = os.path.join(args.dst, "env.json")
    dst_env = load_json(dst_env_path, {})
    for section in ("tools", "empirical_default_threads"):
        merged = dict(dst_env.get(section, {}))
        for t in tools:
            if t in src_env.get(section, {}):
                merged[t] = src_env[section][t]
        dst_env[section] = ordered(merged)
    dst_env.setdefault("merged_sessions", []).append({
        "session": session,
        "timestamp_utc": src_env.get("timestamp_utc"),
        "kernel_release": src_env.get("kernel_release"),
        "affinity_cpus": src_env.get("affinity_cpus"),
        "tools": sorted(tools),
        "records": len(added),
        "note": args.note,
    })
    write_json(dst_env_path, dst_env)

    equiv_dir = args.equiv or args.src
    src_diffs = load_json(os.path.join(equiv_dir, "diff_samples.json"), {})
    dst_diffs_path = os.path.join(args.dst, "diff_samples.json")
    dst_diffs = load_json(dst_diffs_path, {})
    n_diffs = 0
    for sid, by_tool in src_diffs.items():
        for t, sample in by_tool.items():
            if t in tools:
                dst_diffs.setdefault(sid, {})[t] = sample
                n_diffs += 1
    if args.replace:
        for sid in list(dst_diffs):
            for t in list(dst_diffs[sid]):
                if t in tools and src_diffs.get(sid, {}).get(t) is None:
                    del dst_diffs[sid][t]
            if not dst_diffs[sid]:
                del dst_diffs[sid]
    write_json(dst_diffs_path, dst_diffs)

    print(f"Merged {len(added)} runs of {', '.join(sorted(tools))} from {session} "
          f"({n_diffs} Gate B diff samples) into {args.dst}")
    if not args.no_report:
        subprocess.run([sys.executable, os.path.join(SCRIPT_DIR, "report.py"), "--results-dir", args.dst], check=True)


if __name__ == "__main__":
    main()
