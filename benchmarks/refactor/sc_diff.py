#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Report the "make syntax-check" violations that a change adds.

usage: sc_diff.py A.log B.log

A.log and B.log hold the output of "make -k syntax-check > LOG 2>&1"
in trees A (before the change) and B (after it).  Run make with -k:
without it make stops at the first failing rule, and the later rules
never run.

The check fails at the fork point already (long lines in benchmarks/
and src/, among others), so a refactor is gated on adding no new
violation rather than on a clean run.  Each violation becomes a key
of rule name plus offending text, ignoring the file name and line
number, so moving an existing violation to another line or file does
not count as new.  Keys are counted, so a second copy of an existing
violation does count.

Exit status: 0 if B adds no violation, 1 if it adds some or if a rule
that ran in A did not run in B, 2 on error.
"""

import argparse
import collections
import re
import sys
import traceback

# "make: *** [maint.mk:375: sc_rule] Error 1" (make 3.81 prints just
# "[sc_rule]"), sometimes appended to the rule's halt message because
# stdout and stderr interleave.
FAILED = re.compile(r"make(?:\[\d+\])?: \*\*\* \[(?:[^\]]*?: )?"
                    r"sc_([^\]\s]+)\] Error \d+")
PASSED = re.compile(r"\d+\.\d+ (\S+)")
RULE = re.compile(r"[A-Za-z0-9_.+-]+")
FILE_LINE = re.compile(r"[^:\s]+:\d+:(.*)")
FILE_COLON = re.compile(r"[^:\s]+\.[A-Za-z0-9]+: (.*)")
HALT = re.compile(r"[^:\s]+\.mk: ")
MAKE_NOISE = re.compile(r"make(?:\[\d+\])?: (Entering|Leaving|Target) ")


def violation_key(line):
    """Return the text of LINE without its file name and line number."""
    m = FILE_LINE.fullmatch(line)
    if m:
        return m.group(1).rstrip()
    m = FILE_COLON.fullmatch(line)
    if m:
        return m.group(1).rstrip()
    return re.sub(r":\d+:", ":N:", line).rstrip()


def parse(path):
    """Return (rules that ran, Counter of (rule, key), examples)."""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    ran = set()
    keys = collections.Counter()
    examples = {}
    current = None

    def record(rule, text):
        if not text.strip() or HALT.match(text) or MAKE_NOISE.match(text):
            return
        key = (rule, violation_key(text))
        keys[key] += 1
        examples.setdefault(key, text)

    for line in lines:
        m = FAILED.search(line)
        if m:
            ran.add(m.group(1))
            record(current or m.group(1), line[:m.start()])
            current = None
            continue
        m = PASSED.fullmatch(line)
        if m and m.group(1) == current:
            ran.add(current)
            current = None
            continue
        if current is None and RULE.fullmatch(line):
            current = line
            continue
        record(current or "?", line)
    return ran, keys, examples


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Report the "make syntax-check" violations that B '
        "adds to A.")
    ap.add_argument("a_log", metavar="A.log")
    ap.add_argument("b_log", metavar="B.log")
    args = ap.parse_args(argv)
    try:
        ran_a, keys_a, _ = parse(args.a_log)
        ran_b, keys_b, examples_b = parse(args.b_log)
    except OSError as e:
        print(f"sc_diff: {e}", file=sys.stderr)
        return 2
    if not ran_a:
        print(f"sc_diff: {args.a_log}: no syntax-check rule ran",
              file=sys.stderr)
        return 2
    status = 0
    missing = sorted(ran_a - ran_b)
    if missing:
        status = 1
        print(f"sc_diff: rules that ran in A but not in B (was make run "
              f"with -k?): {', '.join(missing)}")
    added = keys_b - keys_a
    removed = keys_a - keys_b
    if added:
        status = 1
        print(f"sc_diff: B adds {sum(added.values())} violation(s):")
        for (rule, key), n in sorted(added.items()):
            count = f" (x{n})" if n > 1 else ""
            print(f"  sc_{rule}{count}: {examples_b[(rule, key)]}")
    print(f"sc_diff: {len(ran_b)} rules ran; B adds "
          f"{sum(added.values())} and removes {sum(removed.values())} "
          "violation(s)")
    return status


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:          # a bug, not a difference
        traceback.print_exc()
        sys.exit(2)
