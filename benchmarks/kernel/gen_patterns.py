#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Generate deterministic benchmark pattern files from Linux kernel symbols.

Scans the Linux kernel source tree for EXPORT_SYMBOL* declarations,
sorts unique symbols, and deterministically samples pattern sets (seed 42).
"""

import argparse
import hashlib
import os
import random
import re
import subprocess
import sys

EXPORT_SYMBOL_RE = re.compile(
    rb"EXPORT_SYMBOL(?:_GPL|_NS|_NS_GPL)?\s*\(\s*([a-zA-Z0-9_]+)\s*\)"
)


def extract_symbols(ksrc):
    symbols = set()
    for root, dirs, files in os.walk(ksrc):
        if ".git" in dirs:
            dirs.remove(".git")
        for f in files:
            if f.endswith((".c", ".h", ".S")):
                path = os.path.join(root, f)
                try:
                    with open(path, "rb") as fp:
                        content = fp.read()
                        for m in EXPORT_SYMBOL_RE.finditer(content):
                            sym = m.group(1).decode("ascii", errors="ignore")
                            if len(sym) >= 12 and re.match(r"^[A-Za-z0-9_]+$", sym):
                                symbols.add(sym)
                except (OSError, UnicodeDecodeError):
                    pass
    return sorted(symbols)


def calibrate_pattern_file(pat_path: str, ksrc: str, is_regex: bool = False) -> tuple:
    """Runs rg -uu -g '!.git' -c [-F] -f pat_path ksrc to count matching lines and files."""
    args = ["/usr/bin/rg", "-uu", "-g", "!.git", "--color=never", "-c"]
    if not is_regex:
        args.append("-F")
    args.extend(["-f", pat_path, ksrc])
    res = subprocess.run(args, capture_output=True, text=True, errors="replace")
    m_lines = 0
    m_files = 0
    for line in res.stdout.splitlines():
        if ":" in line:
            try:
                cnt = int(line.rsplit(":", 1)[1])
                m_lines += cnt
                m_files += 1
            except ValueError:
                pass
    # Total lines in tree is approx 43.8M
    tree_lines = 43821626
    pct = (m_lines / tree_lines) * 100.0 if tree_lines else 0.0
    return m_lines, m_files, pct


def write_patterns(out_dir, symbols, ksrc=""):
    os.makedirs(out_dir, exist_ok=True)
    rnd = random.Random(42)
    shuffled = list(symbols)
    rnd.shuffle(shuffled)

    sets = {
        "sym_100.txt": sorted(shuffled[:100]),
        "sym_1000.txt": sorted(shuffled[:1000]),
        "sym_10000.txt": sorted(shuffled[:10000]),
    }

    manifest = {}
    for filename, sym_list in sets.items():
        path = os.path.join(out_dir, filename)
        # First write raw patterns to run calibration
        raw_content = "\n".join(sym_list) + "\n"
        with open(path, "w", encoding="utf-8") as f:
            f.write(raw_content)

        m_lines, m_files, pct = 0, 0, 0.0
        if ksrc and os.path.isdir(ksrc):
            m_lines, m_files, pct = calibrate_pattern_file(path, ksrc, is_regex=False)
            print(f"Calibration for {filename}: {m_lines} lines across {m_files} files ({pct:.3f}% of tree)")
            if pct >= 3.0:
                print(f"WARNING: {filename} matches {pct:.2f}% >= 3.0% of tree!", file=sys.stderr)

        header = f"# Calibration: {m_lines} lines across {m_files} files ({pct:.3f}% of tree)\n"
        final_content = header + raw_content
        with open(path, "w", encoding="utf-8") as f:
            f.write(final_content)

        sha256 = hashlib.sha256(final_content.encode("utf-8")).hexdigest()
        manifest[filename] = {
            "count": len(sym_list),
            "sha256": sha256,
            "matching_lines": m_lines,
            "matching_files": m_files,
            "pct_tree": pct,
        }
        print(f"Generated {filename}: {len(sym_list)} patterns (SHA256: {sha256})")

    # Generate regex alternation variant for sym_1000
    regex_filename = "sym_1000_regex.txt"
    regex_list = [f"{s}\\(" for s in sets["sym_1000.txt"]]
    regex_path = os.path.join(out_dir, regex_filename)
    raw_regex_content = "\n".join(regex_list) + "\n"
    with open(regex_path, "w", encoding="utf-8") as f:
        f.write(raw_regex_content)

    m_lines, m_files, pct = 0, 0, 0.0
    if ksrc and os.path.isdir(ksrc):
        m_lines, m_files, pct = calibrate_pattern_file(regex_path, ksrc, is_regex=True)
        print(f"Calibration for {regex_filename}: {m_lines} lines across {m_files} files ({pct:.3f}% of tree)")

    regex_header = f"# Calibration: {m_lines} lines across {m_files} files ({pct:.3f}% of tree)\n"
    final_regex_content = regex_header + raw_regex_content
    with open(regex_path, "w", encoding="utf-8") as f:
        f.write(final_regex_content)

    regex_sha256 = hashlib.sha256(final_regex_content.encode("utf-8")).hexdigest()
    manifest[regex_filename] = {
        "count": len(regex_list),
        "sha256": regex_sha256,
        "matching_lines": m_lines,
        "matching_files": m_files,
        "pct_tree": pct,
    }
    print(f"Generated {regex_filename}: {len(regex_list)} patterns (SHA256: {regex_sha256})")

    return manifest


def main():
    parser = argparse.ArgumentParser(description="Generate kernel benchmark pattern files.")
    parser.add_argument(
        "--ksrc",
        default=os.environ.get("K_SRC", os.path.expanduser("~/p/s/linux-stable")),
        help="Path to Linux kernel source tree",
    )
    parser.add_argument(
        "--out-dir",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "patterns"),
        help="Output directory for pattern files",
    )
    args = parser.parse_args()

    if not os.path.isdir(args.ksrc):
        print(f"Error: Linux source tree not found at {args.ksrc}", file=sys.stderr)
        sys.exit(1)

    print(f"Scanning {args.ksrc} for EXPORT_SYMBOL* declarations...")
    symbols = extract_symbols(args.ksrc)
    print(f"Extracted {len(symbols)} unique symbols.")
    if len(symbols) < 10000:
        print(f"Error: Expected >= 10000 symbols, found {len(symbols)}", file=sys.stderr)
        sys.exit(1)

    write_patterns(args.out_dir, symbols, ksrc=args.ksrc)
    print("Pattern generation complete.")


if __name__ == "__main__":
    main()
