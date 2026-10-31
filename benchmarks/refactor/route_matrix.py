#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Check that two test builds route and answer every case alike.

usage: route_matrix.py --a A/src/grep-shrunk --b B/src/grep-shrunk
                       [--jobs N] [--threads 1,2,32] [--keep]

Both programs must be test builds (grep-shrunk or grep-hooks, compiled
with -DGREP_TEST_HOOKS), because the matrix reads GREP_ROUTE_LOG: the
search path that each run takes for each operand.  The default input
sizes suit grep-shrunk, which chunks files from 128 KiB; give
grep-hooks, which uses the release sizes, --large-size 4M.

The matrix runs each program on every combination of:

  standard input  closed, /dev/null, a small file, a large file at
                  offset 0 and at offset 1, a pipe, and a pipe that
                  carries a NUL byte (binary data)
  operands        none, "-", a small file and "-", a small file, a
                  large file, a large file with a NUL byte, a
                  directory with -r, and 16 files
  options         none, -c, -l, -L, -q, -m1, -v, -v -C1, -v -n, -C0,
                  -n -C1, -A1, -o, -n, --line-buffered and
                  --binary-files=without-match
  GREP_THREADS    1, 2 and 32

plus runs whose standard output is the file being searched, and runs
aimed at the routes that the matrix does not reach: early and late NUL
bytes, a large binary file, a binary file in a directory, many matches
with line numbers, and an encoding error in a UTF-8 locale.  For each
run it compares the sorted route log, standard output, standard error
and exit status of A and B.  It also requires every route that A lists
to be taken, as tests/parallel-route-trace does, unless
--no-route-coverage is given.

Exit status: 0 if A and B agree on every run and every route is taken,
1 otherwise (or if a run hangs), 2 on error.
"""

import argparse
import collections
import concurrent.futures
import hashlib
import locale
import os
import shutil
import signal
import sys
import tempfile
import threading
import time
import traceback

PATTERN = "77"
STDINS = ("closed", "devnull", "small", "large", "large+1", "pipe",
          "pipe-nul")
OPERANDS = (
    (),
    ("-",),
    ("small.txt", "-"),
    ("small.txt",),
    ("large.txt",),
    ("nul-late.txt",),
    ("-r", "dir"),
    tuple(f"f{i:02d}" for i in range(1, 17)),
)
OPTIONS = (
    (), ("-c",), ("-l",), ("-L",), ("-q",), ("-m1",), ("-v",),
    ("-v", "-C1"), ("-v", "-n"), ("-C0",), ("-n", "-C1"), ("-A1",),
    ("-o",), ("-n",), ("--line-buffered",),
    ("--binary-files=without-match",),
)
# (arguments, locale) of the runs aimed at particular routes; the
# same inputs as tests/parallel-route-trace, scaled to grep-shrunk.
UTF8 = "en_US.UTF-8"
TARGETED = (
    (("-n", "-e", "777", "nul-early.txt"), "C"),
    (("-n", "-e", "777", "nul-late.txt"), "C"),
    (("-c", "-e", "777", "nul-late.txt"), "C"),
    (("-n", "-e", "text", "large.txt"), "C"),
    (("-q", "-F", "-e", "needle", "bin.dat"), "C"),
    (("-r", "-q", "-F", "-e", "needle", "tree", "small.txt"), "C"),
    (("-C1", "-e", "needle", "enc.txt"), UTF8),
)
ENC_ROUTE = "chunk-enc-sequential"
TIMEOUT = 60


def parse_size(text):
    units = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    if text and text[-1].upper() in units:
        return int(text[:-1]) * units[text[-1].upper()]
    return int(text)


def have_locale(name):
    """Whether the C library has locale NAME."""
    saved = locale.setlocale(locale.LC_CTYPE)
    try:
        locale.setlocale(locale.LC_CTYPE, name)
        return True
    except locale.Error:
        return False
    finally:
        locale.setlocale(locale.LC_CTYPE, saved)


def make_inputs(root, large_size):
    """Create the input files in ROOT; return the data of the pipes."""
    def put(name, data):
        with open(os.path.join(root, name), "wb") as f:
            f.write(data)

    lines = []
    size = n = 0
    while size < large_size:
        n += 1
        line = f"line {n} text\n".encode()
        lines.append(line)
        size += len(line)
    large = b"".join(lines)
    put("large.txt", large)
    nul = b"x\0y\n"
    put("nul-late.txt", large + nul + large)
    put("nul-early.txt", large[:50000] + nul + large)
    put("enc.txt", large[:300000] + b"ctx \377 byte\nneedle\n" + large)
    put("small.txt", b"small 77\nsmall 8\n")
    for i in range(1, 17):
        put(f"f{i:02d}", f"op {i}\n{'op 77' if i % 4 == 0 else 'x'}\n"
            .encode())
    os.mkdir(os.path.join(root, "dir"))
    put("dir/a.txt", b"dir 77\n")
    put("dir/b.txt", b"dir 8\n")
    put("dir/large.txt", large)
    # A -q search of a binary file uses one mmap scan from 2 MiB, and a
    # walk scans binary files in slices from 256 KiB in grep-shrunk.
    binary = (b"z" * 1023 + b"\n") * 2100 + b"\0needle\n"
    put("bin.dat", binary)
    os.mkdir(os.path.join(root, "tree"))
    put("tree/a", b"small 7\n")
    put("tree/bin.dat", binary)
    half = len(lines) // 2
    return {"pipe": large,
            "pipe-nul": b"".join(lines[:half]) + nul
            + b"".join(lines[half:])}


class Case:
    """One run: standard input, arguments, threads and locale."""

    def __init__(self, stdin, args, threads, out_is=None, lc=None):
        self.stdin = stdin
        self.args = args
        self.threads = threads
        self.out_is = out_is     # "stdin" or "operand": output = input
        self.locale = lc

    def describe(self):
        inp = {
            "closed": " 0<&-", "devnull": " < /dev/null",
            "small": " < small.txt", "large": " < large.txt",
            "large+1": " < large.txt (read from offset 1)",
            "pipe": " (stdin: cat large.txt |)",
            "pipe-nul": " (stdin: a pipe with a NUL byte)",
        }.get(self.stdin, "")
        out = ""
        if self.out_is:
            out = f" >> copy.txt (the {self.out_is} is copy.txt)"
        lc = f"LC_ALL={self.locale} " if self.locale else ""
        return (f"{lc}GREP_THREADS={self.threads} grep "
                f"{' '.join(self.args)}{inp}{out}")


def cases(threads, utf8=True):
    result = []
    for t in threads:
        for stdin in STDINS:
            for operands in OPERANDS:
                for options in OPTIONS:
                    args = (*options, "-e", PATTERN, *operands)
                    result.append(Case(stdin, args, t))
        for options in ((), ("-c",)):
            args = (*options, "-e", PATTERN)
            result.append(Case("copy", (*args, "-"), t, "stdin"))
            result.append(Case("devnull", (*args, "copy.txt"), t,
                               "operand"))
        for args, lc in TARGETED:
            if lc == UTF8 and not utf8:
                continue
            for stdin in ("devnull", "closed"):
                result.append(Case(stdin, args, t, lc=lc))
    return result


def run(prog, case, root, pipes, default_locale, tag):
    """Run PROG on CASE in ROOT; return (routes, out, err, status)."""
    run_dir = tempfile.mkdtemp(prefix=f"{tag}-",
                               dir=os.path.join(root, "runs"))
    log = os.path.join(run_dir, "route.log")
    out_path = os.path.join(run_dir, "out")
    args = list(case.args)
    if case.out_is:
        out_path = os.path.join(run_dir, "copy.txt")
        shutil.copyfile(os.path.join(root, "large.txt"), out_path)
        if case.out_is == "operand":
            args[-1] = out_path
        out_fd = os.open(out_path, os.O_WRONLY | os.O_APPEND)
    else:
        out_fd = os.open(out_path, os.O_WRONLY | os.O_CREAT, 0o644)
    fds = [out_fd, os.open(os.path.join(run_dir, "err"),
                           os.O_WRONLY | os.O_CREAT, 0o644)]
    actions = [(os.POSIX_SPAWN_DUP2, fds[0], 1),
               (os.POSIX_SPAWN_DUP2, fds[1], 2)]
    w_fd = None
    if case.stdin == "closed":
        actions.append((os.POSIX_SPAWN_CLOSE, 0))
    else:
        if case.stdin in pipes:
            in_fd, w_fd = os.pipe()
        else:
            name = {"devnull": os.devnull, "small": "small.txt",
                    "large": "large.txt", "large+1": "large.txt",
                    "copy": out_path}[case.stdin]
            in_fd = os.open(os.path.join(root, name), os.O_RDONLY)
            if case.stdin == "large+1":
                os.lseek(in_fd, 1, os.SEEK_SET)
        fds.append(in_fd)
        actions.append((os.POSIX_SPAWN_DUP2, in_fd, 0))
    env = {"PATH": "/usr/bin:/bin",
           "LC_ALL": case.locale or default_locale,
           "GREP_THREADS": str(case.threads), "GREP_ROUTE_LOG": log}
    try:
        pid = os.posix_spawn(prog, ["grep", *args], env,
                             file_actions=actions,
                             setsigdef=(signal.SIGPIPE,))
    except BaseException:
        if w_fd is not None:
            os.close(w_fd)
        raise
    finally:
        for fd in fds:
            os.close(fd)
    writer = None
    if w_fd is not None:
        def feed():
            try:
                with os.fdopen(w_fd, "wb") as w:
                    w.write(pipes[case.stdin])
            except BrokenPipeError:
                pass
        writer = threading.Thread(target=feed)
        writer.start()
    killed = threading.Event()

    def kill():
        killed.set()
        os.kill(pid, signal.SIGKILL)

    timer = threading.Timer(TIMEOUT, kill)
    timer.start()
    # Wait without reaping, so that the timer cannot hit a reused pid.
    os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT)
    timer.cancel()
    timer.join()
    _, wstatus = os.waitpid(pid, 0)
    if writer:
        writer.join()
    status = "hang" if killed.is_set() else (
        os.waitstatus_to_exitcode(wstatus))

    def digest(data):
        return hashlib.sha256(data).hexdigest()[:16]

    routes = ()
    if os.path.exists(log):
        with open(log, encoding="utf-8", errors="replace") as f:
            text = f.read().replace(run_dir, "RUN")
        routes = tuple(sorted(text.splitlines()))
    with open(out_path, "rb") as f:
        out = digest(f.read())
    with open(os.path.join(run_dir, "err"), "rb") as f:
        err = digest(f.read().replace(run_dir.encode(), b"RUN"))
    shutil.rmtree(run_dir)
    return routes, out, err, status


def route_names(prog):
    """Return the routes that PROG lists, or None if it lists none."""
    r_fd, w_fd = os.pipe()
    actions = [(os.POSIX_SPAWN_DUP2, w_fd, 1),
               (os.POSIX_SPAWN_OPEN, 2, os.devnull, os.O_WRONLY, 0)]
    try:
        pid = os.posix_spawn(prog, ["grep"], {"GREP_ROUTE_LOG": "list"},
                             file_actions=actions)
    finally:
        os.close(w_fd)
    with os.fdopen(r_fd, "rb") as r:
        out = r.read().decode(errors="replace")
    os.waitpid(pid, 0)
    names = out.split()
    return names if "serial" in names else None


def describe_difference(case, a, b):
    print(f"DIFFERENT: {case.describe()}")
    ra, rb = collections.Counter(a[0]), collections.Counter(b[0])
    if ra != rb:
        print(f"  routes only in A: {sorted((ra - rb).elements())}")
        print(f"  routes only in B: {sorted((rb - ra).elements())}")
    for name, x, y in zip(("stdout", "stderr", "exit status"), a[1:],
                          b[1:]):
        if x != y or x == "hang":
            print(f"  {name}: A {x}  B {y}")


def run_all(progs, todo, root, pipes, args):
    """Run every case on both programs; return {(build, index): result}."""
    pool = concurrent.futures.ThreadPoolExecutor(args.jobs)
    results = {}
    try:
        futures = {
            pool.submit(run, progs[key], case, root, pipes, args.locale,
                        f"{key}{i}"): (key, i)
            for i, case in enumerate(todo) for key in progs}
        for future in concurrent.futures.as_completed(futures):
            results[futures[future]] = future.result()
    except BaseException:
        pool.shutdown(wait=True, cancel_futures=True)
        raise
    pool.shutdown()
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Check that two test builds route and answer every "
        "case alike.")
    ap.add_argument("--a", required=True, help="test build A")
    ap.add_argument("--b", required=True, help="test build B")
    ap.add_argument("--jobs", type=int,
                    default=min(32, os.cpu_count() or 1),
                    help="runs at a time (default: up to 32)")
    ap.add_argument("--threads", default="1,2,32",
                    help="GREP_THREADS values (default: 1,2,32)")
    ap.add_argument("--large-size", default="1M",
                    help="size of the large file (default: 1M)")
    ap.add_argument("--locale", default="C",
                    help="LC_ALL for the runs (default: C)")
    ap.add_argument("--show", type=int, default=20, metavar="N",
                    help="describe at most N differing runs")
    ap.add_argument("--no-route-coverage", action="store_true",
                    help="do not require every route to be taken")
    ap.add_argument("--keep", action="store_true",
                    help="keep the input directory")
    args = ap.parse_args(argv)
    progs = {"A": os.path.abspath(args.a), "B": os.path.abspath(args.b)}
    root = None
    try:
        names = None
        for prog in progs.values():
            if not os.access(prog, os.X_OK):
                raise OSError(f"{prog}: not executable")
            names = route_names(prog)
            if names is None:
                raise OSError(f"{prog}: not a test build (it ignores "
                              "GREP_ROUTE_LOG)")
        threads = [int(t) for t in args.threads.split(",")]
        utf8 = have_locale(UTF8)
        root = tempfile.mkdtemp(prefix="route-matrix-")
        os.mkdir(os.path.join(root, "runs"))
        os.chdir(root)
        pipes = make_inputs(root, parse_size(args.large_size))
        todo = cases(threads, utf8)
        start = time.monotonic()
        results = run_all(progs, todo, root, pipes, args)
        elapsed = time.monotonic() - start
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError) as e:
        print(f"route_matrix: {e}", file=sys.stderr)
        return 2

    bad = []
    for i, case in enumerate(todo):
        a, b = results[("A", i)], results[("B", i)]
        if a != b or "hang" in (a[3], b[3]):
            bad.append((case, a, b))
    for case, a, b in bad[:args.show]:
        describe_difference(case, a, b)
    if len(bad) > args.show:
        print(f"... and {len(bad) - args.show} more")
    seen = {key: {line.split("\t", 1)[0] for k, r in results.items()
                  if k[0] == key for line in r[0]} for key in progs}
    exempt = set() if utf8 else {ENC_ROUTE}
    unseen = [n for n in names if n not in exempt
              and not (n in seen["A"] and n in seen["B"])]
    print(f"route_matrix: {len(todo)} cases per build in {elapsed:.1f} s; "
          f"{len(bad)} differ; {len(names) - len(unseen)} of "
          f"{len(names)} routes taken"
          + (f" (not taken: {', '.join(unseen)})" if unseen else "")
          + ("" if utf8 else f"; no {UTF8} locale, {ENC_ROUTE} exempt"))
    failed = bad or (unseen and not args.no_route_coverage)
    if failed or args.keep:
        print(f"route_matrix: inputs kept in {root}")
    else:
        os.chdir("/")
        shutil.rmtree(root)
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:          # a bug, not a difference
        traceback.print_exc()
        sys.exit(2)
