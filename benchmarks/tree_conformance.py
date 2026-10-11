#!/usr/bin/env python3
"""Byte-exact tree-traversal conformance harness.

Compare a candidate grep binary against a baseline grep binary on
directory trees: stdout, stderr and the exit status must be identical
for every option set and every GREP_THREADS value.

The harness builds adversarial trees (symbolic-link loops, unreadable
directories and files, FIFOs, exclusion globs, empty and very deep
directories, a directory with more than 10,000 entries, binary files,
names with spaces and newlines, and the output file inside the
searched tree) under one or more work directories, and can also scan
existing read-only trees given with --tree.

Both binaries are launched with the same long absolute argv[0], so that
diagnostics, which start with argv[0], can be compared unnormalized.
Some runs send stdout and stderr to the same file, because diagnostics
flush standard output first and that interleaving must match too.

Regression entries in the adversarial tree:
  longdiag/   an unreadable file whose diagnostic exceeds 2 KiB
  bin/        binary files, so -C/-A/-B runs see quiet matches
  out*/       hold the output file in the output-file-in-tree runs
A separate tree holds names longer than PATH_MAX, which fts-based grep
handles with -r but not with -R; its comparisons are labeled
"deep_path_max".  Another one is deep and has a leaf directory at every
level; it is searched with low limits on open files.  Comparisons whose
label starts with a --known-fail prefix are reported as expected
failures instead of failures.

Example:
  python3 benchmarks/tree_conformance.py \\
      --work-dir /tmp/tc-tmpfs --work-dir /var/tmp/tc-disk \\
      --tree /path/to/some/source/tree --tree-pattern spin_lock_irqsave
"""

import argparse
import hashlib
import os
import resource
import shutil
import subprocess
import sys
import threading
import time

DEFAULT_THREADS = [1, 2, 3, 7, 16, 32, 64]

# argv[0] for both binaries: long and absolute, like an installed copy.
ARGV0 = "/opt/example/prefix/with/a/rather/long/installation/path/bin/grep"

# Option sets required for every tree.
BASE_OPTION_SETS = [
    [], ["-n"], ["-c"], ["-l"], ["-L"], ["-q"], ["-m1"], ["-o"], ["-C2"],
    ["-Z", "-l"], ["-s"], ["-H"], ["-h"],
]

# Extra option sets exercised on the (small) adversarial trees.
EXTRA_OPTION_SETS = [
    ["-c", "-C2"], ["-n", "-b", "-T"], ["--color=always", "-n"],
    ["-v", "-c"], ["-w", "-n"], ["-i", "-l"], ["-L", "-Z"],
    ["-m2", "-n"], ["-A1", "--group-separator=XX"], ["-B1", "-n"],
    ["--exclude-dir=.git"], ["--exclude-dir=sub*", "-l"],
    ["--include=*.c"], ["--include=*.c", "--include=*.h", "-n"],
    ["--exclude=*.h", "-c"], ["--exclude=*.txt", "--include=x*", "-l"],
    ["--exclude-dir=wide", "--exclude-dir=deep*", "-n"],
    ["-s", "-l"], ["-D", "skip"], ["-a", "-c"], ["-I", "-l"],
    ["--binary-files=without-match", "-n"], ["-z", "-l"],
]

# Option sets run with stdout and stderr sent to the same file.
COMBINED_OPTION_SETS = [[], ["-n"], ["-c"], ["-l"], ["-C2"], ["-s"]]

PATTERN = "needle"


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------- trees

def write_file(path, data, mode=None):
    if isinstance(data, str):
        data = data.encode()
    with open(path, "wb") as f:
        f.write(data)
    if mode is not None:
        os.chmod(path, mode)


def make_chain(base, prefix, levels, namelen, leaf_name, leaf_data,
               leaf_mode=0o644):
    """Create BASE/prefix0/prefix1/... LEVELS deep using directory file
    descriptors, so that the full name may exceed PATH_MAX."""
    fd = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
    try:
        for i in range(levels):
            name = (f"{prefix}{i:04d}_" + "x" * namelen)[:namelen]
            os.mkdir(name, dir_fd=fd)
            nfd = os.open(name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd)
            if i % 25 == 24:
                lf = os.open(f"mid{i}.txt", os.O_WRONLY | os.O_CREAT, 0o644,
                             dir_fd=nfd)
                os.write(lf, b"a needle in the middle\n")
                os.close(lf)
            os.close(fd)
            fd = nfd
        lf = os.open(leaf_name, os.O_WRONLY | os.O_CREAT, 0o644, dir_fd=fd)
        os.write(lf, leaf_data)
        os.close(lf)
        os.chmod(leaf_name, leaf_mode, dir_fd=fd)
    finally:
        os.close(fd)


def build_adversarial(root, wide_count):
    """Build the adversarial tree at ROOT (which must not exist)."""
    os.makedirs(root)
    j = lambda *p: os.path.join(root, *p)

    # Created first and last, so that on file systems that list entries
    # in (reverse) creation order one of them is visited after most of
    # the tree, i.e. after much output.
    os.makedirs(j("out_early"))

    write_file(j("a.txt"), "alpha\nneedle one\nbeta\nneedle two\n")
    write_file(j("needle_name.txt"), "no match in this one\n")
    write_file(j("zeta.txt"), "nothing to see\n")
    write_file(j(".hidden"), "needle hidden\n")
    write_file(j("name with spaces.txt"), "needle spaces\n")
    write_file(j("name\nwith\nnewline.txt"), "needle newline\n")
    write_file(j("crlf.txt"), "needle crlf\r\nfoo\r\n")
    write_file(j("noeol.txt"), "foo\nneedle at end without newline")
    write_file(j("empty.txt"), "")
    ctx = [f"line {i}" for i in range(200)]
    for i in (5, 7, 20, 100, 101, 199):
        ctx[i] = f"needle at {i}"
    write_file(j("context.txt"), "\n".join(ctx) + "\n")

    os.makedirs(j("bin"))
    write_file(j("bin", "binary.dat"), b"needle\x00\x01\x02 needle\n")
    late = bytearray()
    while len(late) < 150 * 1024:
        late += b"text line before the nul byte, needle maybe\n"
    late[120 * 1024] = 0
    write_file(j("bin", "late_nul.dat"), bytes(late))
    big = bytearray()
    n = 0
    while len(big) < 300 * 1024:
        big += (f"line {n} needle\n" if n % 997 == 0 else f"line {n}\n").encode()
        n += 1
    write_file(j("bin", "big_text.txt"), bytes(big))
    with open(j("bin", "hole.dat"), "wb") as f:
        f.write(b"needle before hole\n")
        f.seek(1024 * 1024)
        f.write(b"needle after hole\n")
    write_file(j("bin", "latin1.txt"), b"needle caf\xe9\n")

    os.makedirs(j("empty_dir"))
    os.makedirs(j("empty_dir2", "sub_empty"))
    os.makedirs(j("sub1", "d", "e", "f"))
    write_file(j("sub1", "b.c"), "int needle;\n")
    write_file(j("sub1", "c.h"), "nothing\n")
    write_file(j("sub1", "d", "e", "f", "g.txt"), "needle deep\n")
    os.makedirs(j("sub2"))
    write_file(j("sub2", "k.c"), "needle in sub2\n")
    os.makedirs(j("inc"))
    for name in ("x.c", "x.h", "x.txt", "y.o", "z.c"):
        write_file(j("inc", name), f"needle in {name}\n")
    os.makedirs(j(".git", "objects", "aa"))
    write_file(j(".git", "config"), "needle in git\n")
    write_file(j(".git", "objects", "aa", "bb"), "needle object\n")
    os.makedirs(j("build"))
    write_file(j("build", "out.txt"), "needle build\n")

    os.symlink("a.txt", j("filelink"))
    os.symlink("sub1", j("dirlink"))
    os.symlink("does_not_exist", j("dangling"))
    os.symlink("selfloop", j("selfloop"))
    os.makedirs(j("loopdir"))
    os.symlink("..", j("loopdir", "up"))
    os.symlink(".", j("loopdir", "self"))
    write_file(j("loopdir", "file.txt"), "needle loop\n")

    os.makedirs(j("noperm_dir"))
    write_file(j("noperm_dir", "f.txt"), "needle hidden by perms\n")
    write_file(j("noperm_file.txt"), "needle unreadable\n")
    os.makedirs(j("noexec_dir", "subdir"))
    write_file(j("noexec_dir", "x.txt"), "needle noexec\n")
    write_file(j("noexec_dir", "subdir", "y.txt"), "needle noexec sub\n")

    os.makedirs(j("deep"))
    make_chain(j("deep"), "d", 60, 20, "bottom.txt", b"needle at the bottom\n")
    os.makedirs(j("longdiag"))
    make_chain(j("longdiag"), "L", 150, 20, "secret_needle.txt",
               b"needle behind a long name\n", leaf_mode=0)

    os.makedirs(j("wide", "subdir_in_wide"))
    write_file(j("wide", "subdir_in_wide", "w.txt"), "needle in wide subdir\n")
    for i in range(wide_count):
        data = f"needle wide {i}\n" if i % 1000 == 0 else f"file {i}\n"
        write_file(j("wide", f"f{i:05d}"), data)
    os.makedirs(j("out"))

    # Permissions last, so the tree could be populated.
    os.chmod(j("noperm_dir"), 0)
    os.chmod(j("noperm_file.txt"), 0)
    os.chmod(j("noexec_dir"), 0o600)


def build_pathmax_tree(root):
    """A tree with names longer than PATH_MAX."""
    os.makedirs(os.path.join(root, "verydeep"))
    write_file(os.path.join(root, "top.txt"), "needle at the top\n")
    make_chain(os.path.join(root, "verydeep"), "v", 230, 19, "bottom.txt",
               b"needle beyond PATH_MAX\n")


def build_comb_tree(root, depth=120):
    """A deep tree whose every level also has a leaf directory, so that a
    depth-first traversal keeps many directories pending at once."""
    fd = os.open(os.path.dirname(root), os.O_RDONLY | os.O_DIRECTORY)
    name = os.path.basename(root)
    try:
        for i in range(depth + 1):
            os.mkdir(name, dir_fd=fd)
            nfd = os.open(name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd)
            os.close(fd)
            fd = nfd
            os.mkdir("leaf", dir_fd=fd)
            lf = os.open("leaf/f.txt", os.O_WRONLY | os.O_CREAT, 0o644,
                         dir_fd=fd)
            os.write(lf, f"needle in leaf {i}\n".encode())
            os.close(lf)
            lf = os.open("g.txt", os.O_WRONLY | os.O_CREAT, 0o644, dir_fd=fd)
            os.write(lf, f"needle at level {i}\n".encode())
            os.close(lf)
            name = "next"
    finally:
        os.close(fd)


def build_fifo_tree(root):
    os.makedirs(os.path.join(root, "d"))
    write_file(os.path.join(root, "a.txt"), "needle a\n")
    write_file(os.path.join(root, "d", "b.txt"), "needle b\n")
    os.mkfifo(os.path.join(root, "d", "pipe"))
    write_file(os.path.join(root, "z.txt"), "needle z\n")


def fix_perms_for_removal(root):
    for dirpath, dirnames, filenames in os.walk(root):
        for d in dirnames:
            p = os.path.join(dirpath, d)
            try:
                if not os.path.islink(p):
                    os.chmod(p, 0o755)
            except OSError:
                pass
    for p in ("noperm_dir", "noexec_dir"):
        try:
            os.chmod(os.path.join(root, p), 0o755)
        except OSError:
            pass


def remove_tree(root):
    if not os.path.lexists(root):
        return
    for p in ("noperm_dir", "noexec_dir"):
        try:
            os.chmod(os.path.join(root, p), 0o755)
        except OSError:
            pass
    fix_perms_for_removal(root)
    shutil.rmtree(root)


# ---------------------------------------------------------------- runner

class Runner:
    def __init__(self, args):
        self.args = args
        self.env = dict(os.environ)
        if args.locale:
            self.env["LC_ALL"] = args.locale
        self.env.pop("GREP_THREADS", None)
        self.env.pop("GREP_COLOR", None)
        self.env.pop("GREP_COLORS", None)
        self.failures = []
        self.xfails = []
        self.xpasses = []
        self.checks = 0
        self.runs = 0

    def run(self, binary, opts, roots, threads=None, cwd=None,
            stdout_path=None, timeout=300, argv0=ARGV0, combined=False,
            pattern=None, nofile=None):
        env = dict(self.env)
        if threads is not None:
            env["GREP_THREADS"] = str(threads)
        # Same argv[0] for both binaries, so that diagnostics (which
        # start with argv[0]) are comparable byte for byte.
        cmd = [argv0] + opts + ["-e", pattern or self.pattern] + roots
        out_f = open(stdout_path, "wb") if stdout_path else subprocess.PIPE
        try:
            limit = None
            if nofile:
                limit = lambda: resource.setrlimit(resource.RLIMIT_NOFILE,
                                                   (nofile, nofile))
            p = subprocess.run(cmd, executable=binary, stdout=out_f,
                               preexec_fn=limit,
                               stderr=(subprocess.STDOUT if combined
                                       else subprocess.PIPE),
                               stdin=subprocess.DEVNULL, env=env, cwd=cwd,
                               timeout=timeout)
        finally:
            if stdout_path:
                out_f.close()
        self.runs += 1
        out = p.stdout
        err = b"" if combined else p.stderr
        if stdout_path:
            with open(stdout_path, "rb") as f:
                out = f.read()
        return out, err, p.returncode

    def compare(self, label, opts, roots, cwd=None, stdout_path=None,
                fifo=None, argv0=ARGV0, combined=False, pattern=None,
                nofile=None):
        if self.args.only and not any(o in label for o in self.args.only):
            return True
        def go(binary, threads):
            if stdout_path:
                with open(stdout_path, "wb"):
                    pass
            if fifo:
                fifo.arm()
            r = self.run(binary, opts, roots, threads, cwd, stdout_path,
                         argv0=argv0, combined=combined, pattern=pattern,
                         nofile=nofile)
            if fifo:
                fifo.disarm()
            return r

        known_fail = any(label.startswith(k) for k in self.args.known_fail)
        b_out, b_err, b_rc = go(self.base, None)
        for t in self.args.threads:
            c_out, c_err, c_rc = go(self.cand, t)
            self.checks += 1
            if known_fail:
                same = (c_out, c_err, c_rc) == (b_out, b_err, b_rc)
                (self.xpasses if same else self.xfails).append(
                    f"{label} opts={' '.join(opts) or '(none)'} t={t}")
                continue
            if (c_out, c_err, c_rc) != (b_out, b_err, b_rc):
                what = []
                if c_rc != b_rc:
                    what.append(f"rc {b_rc} vs {c_rc}")
                if c_out != b_out:
                    what.append(f"stdout {len(b_out)}B vs {len(c_out)}B")
                if c_err != b_err:
                    what.append(f"stderr {len(b_err)}B vs {len(c_err)}B")
                msg = (f"FAIL {label} opts={' '.join(opts) or '(none)'} "
                       f"t={t}: {', '.join(what)}")
                log("  " + msg)
                self.failures.append(msg)
                self.save_diff(label, opts, t, b_out, c_out, b_err, c_err)
                if self.args.stop_on_fail:
                    return False
        return True

    def save_diff(self, label, opts, t, b_out, c_out, b_err, c_err):
        if not self.args.diff_dir:
            return
        os.makedirs(self.args.diff_dir, exist_ok=True)
        key = hashlib.sha1(f"{label}|{opts}|{t}".encode()).hexdigest()[:10]
        base = os.path.join(self.args.diff_dir, key)
        for suffix, data in (("base.out", b_out), ("cand.out", c_out),
                             ("base.err", b_err), ("cand.err", c_err)):
            with open(f"{base}.{suffix}", "wb") as f:
                f.write(data)
        with open(f"{base}.info", "w") as f:
            f.write(f"{label}\n{opts}\nGREP_THREADS={t}\n")


class FifoWriter:
    """Feed one line to a FIFO each time a reader opens it."""

    def __init__(self, path):
        self.path = path
        self.stop = threading.Event()
        self.active = threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def arm(self):
        self.active.set()

    def disarm(self):
        self.active.clear()
        time.sleep(0.05)

    def loop(self):
        while not self.stop.is_set():
            if not self.active.wait(0.05):
                continue
            try:
                fd = os.open(self.path, os.O_WRONLY | os.O_NONBLOCK)
            except OSError:
                time.sleep(0.001)
                continue
            try:
                os.write(fd, b"needle from the fifo\n")
            finally:
                os.close(fd)
            time.sleep(0.3)


def link_binaries(work, base, cand):
    hb = os.path.join(work, "bin-base")
    hc = os.path.join(work, "bin-cand")
    for d, target in ((hb, base), (hc, cand)):
        os.makedirs(d, exist_ok=True)
        link = os.path.join(d, "grep")
        if os.path.lexists(link):
            os.unlink(link)
        os.symlink(os.path.abspath(target), link)
    return os.path.join(hb, "grep"), os.path.join(hc, "grep")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", default="benchmarks/grep-baseline")
    ap.add_argument("--candidate", default="src/grep")
    ap.add_argument("--work-dir", action="append", default=[],
                    help="directory in which adversarial trees are built "
                    "(repeatable; use different file systems)")
    ap.add_argument("--tree", action="append", default=[],
                    help="existing read-only tree to scan (repeatable)")
    ap.add_argument("--tree-pattern", default="spin_lock_irqsave")
    ap.add_argument("--threads", default=",".join(map(str, DEFAULT_THREADS)))
    ap.add_argument("--locale", default="en_US.UTF-8")
    ap.add_argument("--wide-count", type=int, default=12000)
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--quick", action="store_true",
                    help="only the base option sets on adversarial trees")
    ap.add_argument("--skip-R", action="store_true",
                    help="skip -R (dereference-recursive) runs on --tree")
    ap.add_argument("--stop-on-fail", action="store_true")
    ap.add_argument("--known-fail", action="append", default=None,
                    help="label prefix of comparisons that are expected to "
                    "fail (repeatable)")
    ap.add_argument("--only", action="append", default=[],
                    help="only run comparisons whose label contains this "
                    "string (repeatable)")
    ap.add_argument("--diff-dir", default=None)
    args = ap.parse_args()
    args.threads = [int(x) for x in args.threads.split(",") if x]
    if args.known_fail is None:
        args.known_fail = []

    r = Runner(args)
    if os.geteuid() == 0:
        log("warning: running as root; permission tests are meaningless")

    work_dirs = args.work_dir or ["/tmp/tree-conformance"]
    for wd in work_dirs:
        os.makedirs(wd, exist_ok=True)
        r.base, r.cand = link_binaries(wd, args.baseline, args.candidate)
        adv = os.path.join(wd, "adv")
        if args.rebuild:
            remove_tree(adv)
        if not os.path.exists(adv):
            log(f"building adversarial tree in {adv}")
            build_adversarial(adv, args.wide_count)
        pathmax = os.path.join(wd, "pathmax")
        if args.rebuild:
            remove_tree(pathmax)
        if not os.path.exists(pathmax):
            build_pathmax_tree(pathmax)
        comb = os.path.join(wd, "comb")
        if args.rebuild:
            remove_tree(comb)
        if not os.path.exists(comb):
            build_comb_tree(comb)
        fifo_root = os.path.join(wd, "fifo")
        if args.rebuild:
            remove_tree(fifo_root)
        if not os.path.exists(fifo_root):
            build_fifo_tree(fifo_root)

        r.pattern = PATTERN
        sets = BASE_OPTION_SETS + ([] if args.quick else EXTRA_OPTION_SETS)
        for mode in ("-r", "-R"):
            log(f"[{wd}] adversarial tree, {mode}")
            for opts in sets:
                if not r.compare(f"adv{mode}", [mode] + opts, ["adv"], cwd=wd):
                    break
            # Implicit "." operand (names printed without "./").
            for opts in ([], ["-l"], ["-c"]):
                r.compare(f"adv-dot{mode}", [mode] + opts, [],
                          cwd=os.path.join(wd, "adv", "sub1"))
            # Several operands, files and directories mixed.
            r.compare(f"adv-multi{mode}", [mode, "-n"],
                      ["adv/sub1", "adv/a.txt", "adv/inc", "adv/sub2/k.c"],
                      cwd=wd)
            # Operands that are unreadable, missing, symbolic links, or
            # given with trailing slashes.
            r.compare(f"adv-operands{mode}", [mode, "-c"],
                      ["adv/noperm_dir", "adv/does_not_exist", "adv/dirlink",
                       "adv/filelink", "adv/sub2//", "adv/empty_dir/"],
                      cwd=wd)
            # Diagnostics must start with argv[0] verbatim.
            r.compare(f"adv-argv0{mode}", [mode, "-c"], ["adv"], cwd=wd,
                      argv0="grep")
            # Standard output and standard error to the same file.
            for opts in COMBINED_OPTION_SETS:
                r.compare(f"adv-2>&1{mode}", [mode] + opts, ["adv"], cwd=wd,
                          combined=True)
            # Output file inside the searched tree.
            for sub in ("out", "out_early"):
                out_path = os.path.join(wd, "adv", sub, "result.txt")
                for opts in BASE_OPTION_SETS + [["-m2"], ["-c", "-H"],
                                                ["-l", "-n"]]:
                    r.compare(f"adv-outfile-{sub}{mode}", [mode] + opts,
                              ["adv"], cwd=wd, stdout_path=out_path)
                # A pattern that matches the output itself, so that what
                # is read from the output file depends on how much output
                # has reached it.
                for opts in (["-c"], ["-l"], ["-L"], ["-m1"], ["-c", "-s"]):
                    r.compare(f"adv-outfile-{sub}{mode}", [mode] + opts,
                              ["adv"], cwd=wd, stdout_path=out_path,
                              pattern="wide")
                if os.path.exists(out_path):
                    os.unlink(out_path)

        log(f"[{wd}] names longer than PATH_MAX")
        for mode in ("-r", "-R"):
            for opts in ([], ["-n"], ["-c"], ["-l"], ["-L"], ["-s"]):
                r.compare(f"deep_path_max{mode}", [mode] + opts, ["pathmax"],
                          cwd=wd)
            r.compare(f"deep_path_max-dot{mode}", [mode, "-c"], [],
                      cwd=os.path.join(wd, "pathmax"))

        log(f"[{wd}] deep tree with a low limit on open files")
        for mode in ("-r", "-R"):
            for nofile in (40, 64, 1024):
                for opts in ([], ["-c"], ["-l", "-n"]):
                    r.compare(f"comb{mode}-nofile{nofile}", [mode] + opts,
                              ["comb"], cwd=wd, nofile=nofile)

        log(f"[{wd}] FIFO tree")
        fifo = FifoWriter(os.path.join(fifo_root, "d", "pipe"))
        try:
            for opts, needs_writer in (
                    (["-r"], False), (["-r", "-D", "skip"], False),
                    (["-r", "-D", "read"], True), (["-R", "-D", "skip"], False),
                    (["-R"], True), (["-R", "-D", "read"], True)):
                r.compare("fifo", opts + ["-n"], ["fifo"], cwd=wd,
                          fifo=fifo if needs_writer else None)
        finally:
            fifo.stop.set()

    for tree in args.tree:
        r.pattern = args.tree_pattern
        modes = ["-r"] if args.skip_R else ["-r", "-R"]
        for mode in modes:
            log(f"[{tree}] {mode} pattern={args.tree_pattern}")
            for opts in BASE_OPTION_SETS:
                r.compare(f"tree{mode}", [mode] + opts, [tree])

    log(f"\n{r.checks} comparisons, {r.runs} runs, "
        f"{len(r.failures)} failures, {len(r.xfails)} expected failures, "
        f"{len(r.xpasses)} unexpected passes")
    for f in r.failures:
        log("  " + f)
    if r.xfails:
        log("expected failures: " + ", ".join(sorted(set(
            x.split(" opts=")[0] for x in r.xfails))))
    if r.xpasses:
        log("unexpected passes: " + ", ".join(sorted(set(
            x.split(" opts=")[0] for x in r.xpasses))))
    sys.exit(1 if r.failures else 0)


if __name__ == "__main__":
    main()
