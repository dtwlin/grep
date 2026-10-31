#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Self-test of the refactor gate tools.

usage: selftest.py [-v]

Compiles small C programs in several variants and checks that
elfcmp.py and asmdiff.py ignore what only moves code (function order,
alignment, thread-local layout) and report what changes it (a
constant, a global, a thread-local field, a string, a called library
function, a data value, an inlining decision).  Also checks how
sc_diff.py keys syntax-check violations, the shape of the
route_matrix.py matrix, and how ab_bench.py --summarize applies the
decision rule.  The compiled tests need gcc, GNU objdump and strip on
x86-64 and are skipped otherwise.
"""

import contextlib
import io
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "kernel"))

import asmdiff  # noqa: E402
import elfcmp  # noqa: E402
import route_matrix  # noqa: E402
import sc_diff  # noqa: E402

HAVE_TOOLCHAIN = (shutil.which("gcc") and shutil.which("objdump")
                  and shutil.which("strip")
                  and platform.machine() in ("x86_64", "AMD64"))

PROGRAM = r"""
#include <stdio.h>
#include <string.h>

static _Thread_local int TLS_DECL;
static int g_count, g_other;

#if ORDER == 0
HELPER_ATTR static int
helper (int x)
{
  return x * 3 + HELPER_ADD;
}
#else
static int helper (int);
#endif

__attribute__ ((noipa)) static void
bump (int *p)
{
  *p += 1;
}

static int
body (char const *s)
{
  t_first += (int) strlen (s);
  t_second[TLS_INDEX] = t_first;
  bump (&t_second[1]);
  COUNTER += helper (t_first);
  return printf (MESSAGE, g_count + g_other);
}

#if ORDER != 0
HELPER_ATTR static int
helper (int x)
{
  return x * 3 + HELPER_ADD;
}
#endif

int
main (int argc, char **argv)
{
  return body (argc > 1 ? argv[1] : "x") < 0;
}
"""

BASE = {
    "TLS_DECL": "t_first, t_second[4]",
    "ORDER": "0",
    "HELPER_ATTR": "__attribute__ ((noinline))",
    "HELPER_ADD": "1",
    "TLS_INDEX": "2",
    "COUNTER": "g_count",
    "MESSAGE": '"count %d\\n"',
}

# Data that instructions read: named objects, an anonymous constant,
# strings with control characters, and a string longer than any
# printed prefix.
DATA_PROGRAM = r"""
#include <stdio.h>
static int threshold = THRESH;
static const unsigned char table[8] = { 1, 2, 3, 4, 5, 6, 7, TABV };
__attribute__ ((noinline)) void set (int x) { threshold = x; }
__attribute__ ((noinline)) double scale (double x) { return x * DCONST; }
__attribute__ ((noipa)) void say (char const *s) { fputs (s, stdout); }
int
main (int argc, char **argv)
{
  if (argc > 3)
    set (argc);
  say (SGR);
  say ("0123456789012345678901234567890123456789012345678901234567890"
       TAIL);
  return (argc > threshold) + table[argc & 7] + (int) scale (argc);
}
"""

DATA_BASE = {"THRESH": "5", "TABV": "8", "DCONST": "0.5",
             "SGR": '"\\33[01;31m"', "TAIL": '"x"'}

# A switch with a jump table, and functions to reorder around it.
SWITCH_PROGRAM = r"""
#include <stdio.h>
__attribute__ ((noipa)) int pick (int x);
#if ORDER
__attribute__ ((noipa)) int other (int x) { return x * 7; }
#endif
__attribute__ ((noipa)) int
pick (int x)
{
  switch (x)
    {
    case 0: return puts ("zero");
    case 1: return puts ("one") + 4;
    case 2: return putchar ('2') * 3;
    case 3: return puts ("three") - 9;
    case 4: return fflush (stdout) + 11;
    case 5: return puts ("five") * 13;
    case 6: return getchar () + 17;
    default: return -1;
    }
}
#if !ORDER
__attribute__ ((noipa)) int other (int x) { return x * 7; }
#endif
int main (int argc, char **argv) { return pick (argc) + other (argc); }
"""

# Thread-local variables defined in another file, which an executable
# reaches through offsets loaded into a register (initial-exec model
# relaxed by the linker).
TLS_DEFS = r"""
#if ORDER == 0
_Thread_local char flag;
_Thread_local long other[8];
#else
_Thread_local long other[8];
_Thread_local char flag;
#endif
"""

TLS_USER = r"""
extern _Thread_local char flag;
extern _Thread_local long other[8];
__attribute__ ((noipa)) void
touch (long x)
{
  WHICH = 1;
  other[3] += x;
}
int main (int argc, char **argv) { touch (argc); return flag; }
"""

# Functions that keep the thread pointer across a call, as GCC does in
# cold code: spill spills it to the stack and reloads it, and keep
# holds it in a callee-saved register.
SPILL_PROGRAM = r"""
struct buf { long len, cap; };
_Thread_local struct buf TLS_BUFS;
__attribute__ ((noipa, used)) void
fill (struct buf *b)
{
  b->len++;
}
void spill (void);
void keep (void);
__asm__ (".text\n"
         ".globl spill\n"
         ".type spill, @function\n"
         "spill:\n"
         "  sub $24, %rsp\n"
         "  mov %fs:0, %rax\n"
         "  mov %rax, 8(%rsp)\n"
         "  lea b_one@tpoff(%rax), %rdi\n"
         "  call fill\n"
         "  mov 8(%rsp), %rax\n"
         "  lea b_two@tpoff(%rax), %rdi\n"
         "  call fill\n"
         "  add $24, %rsp\n"
         "  ret\n"
         ".size spill, .-spill\n"
         ".globl keep\n"
         ".type keep, @function\n"
         "keep:\n"
         "  push %rbx\n"
         "  mov %fs:0, %rbx\n"
         "  lea b_one@tpoff(%rbx), %rdi\n"
         "  call fill\n"
         "  lea b_two@tpoff(%rbx), %rdi\n"
         "  call fill\n"
         "  pop %rbx\n"
         "  ret\n"
         ".size keep, .-keep\n");
int
main (void)
{
  spill ();
  keep ();
  return (int) (b_one.len + b_two[1].len);
}
"""

# Thread pointers that do not reach an instruction on every path into
# it: join reaches label 1 from a path that never loads the thread
# pointer, outer jumps into inner past its load, and swap and wide
# overwrite the register that held it (xchg, and mul's implicit %rdx).
JOIN_PROGRAM = r"""
_Thread_local long TLS_VARS;
long join (int, long);
long inner (void);
long outer (long);
long swap (long);
long wide (long);
__asm__ (".text\n"
         ".globl join\n"
         ".type join, @function\n"
         "join:\n"
         "  test %edi, %edi\n"
         "  jne 1f\n"
         "  mov %fs:0, %rsi\n"
         "1:\n"
         "  lea j_two@tpoff(%rsi), %rax\n"
         "  ret\n"
         ".size join, .-join\n"
         ".globl inner\n"
         ".type inner, @function\n"
         "inner:\n"
         "  mov %fs:0, %rdi\n"
         "2:\n"
         "  lea j_two@tpoff(%rdi), %rax\n"
         "  ret\n"
         ".size inner, .-inner\n"
         ".globl outer\n"
         ".type outer, @function\n"
         "outer:\n"
         "  jmp 2b\n"
         ".size outer, .-outer\n"
         ".globl swap\n"
         ".type swap, @function\n"
         "swap:\n"
         "  mov %fs:0, %rsi\n"
         "  xchg %rsi, %rdi\n"
         "  lea j_two@tpoff(%rsi), %rax\n"
         "  ret\n"
         ".size swap, .-swap\n"
         ".globl wide\n"
         ".type wide, @function\n"
         "wide:\n"
         "  mov %fs:0, %rdx\n"
         "  mov %rdi, %rax\n"
         "  mul %rdi\n"
         "  lea j_two@tpoff(%rdx), %rax\n"
         "  ret\n"
         ".size wide, .-wide\n");
int
main (int argc, char **argv)
{
  j_one = argc;
  return (int) (join (argc, (long) argv) + inner () + outer (j_one)
                + swap (argc) + wide (argc) + j_two[1]);
}
"""

# A thread pointer offset into the padding at the end of the block,
# which names no thread-local symbol.
PAD_PROGRAM = r"""
_Thread_local _Alignas (16) long pad_a;
_Thread_local char pad_b;
int peek (void);
__asm__ (".text\n"
         ".globl peek\n"
         ".type peek, @function\n"
         "peek:\n"
         "  mov %fs:0, %rax\n"
         "  movzbl " OFF_REG "(%rax), %eax\n"
         "  movzbl %fs:" OFF_FS ", %ecx\n"
         "  add %ecx, %eax\n"
         "  ret\n"
         ".size peek, .-peek\n");
int
main (void)
{
  pad_a = 1;
  pad_b = 2;
  return peek ();
}
"""

# Two thread-local variables that share a cache line unless aligned.
TLS_LINES = r"""
_Thread_local ALIGN long x;
_Thread_local ALIGN long y;
int
main (int argc, char **argv)
{
  x = argc;
  y = x + 1;
  return (int) y;
}
"""

# Object-file references to global symbols, which relocations name.
OBJ_PROGRAM = r"""
_Thread_local int t_first, t_second[4];
int g_count, g_other;
extern int ext_a (int), ext_b (int);
int
f (int x)
{
  t_second[TLS_INDEX] = x;
  COUNTER += x;
  return CALLEE (x) + 1;
}
"""

OBJ_BASE = {"TLS_INDEX": "2", "COUNTER": "g_count", "CALLEE": "ext_a"}

# Library calls through the GOT (-fno-plt).
GOT_PROGRAM = r"""
#include <unistd.h>
int main (void) { return CALL () & 1; }
"""

# Functions whose cold parts end in calls, in two orders.
TAIL_PROGRAM = r"""
#include <stdlib.h>
extern void ext1 (int);
extern void ext2 (int);
#if ORDER == 0
void f1 (int x) { if (x > 3) abort (); ext1 (x * 7); }
void f2 (int x) { if (x > 5) abort (); ext2 (x * 3); }
#else
void f2 (int x) { if (x > 5) abort (); ext2 (x * 3); }
void f1 (int x) { if (x > 3) abort (); ext1 (x * 7); }
#endif
void f3 (int x) { ext2 (x + 9); }
"""


def run_tool(module, argv):
    """Run MODULE.main(ARGV); return (exit status, standard output)."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        status = module.main(argv)
    return status, out.getvalue()


class Builder:
    """Compile variants of C programs into a temporary directory."""

    def __init__(self, tmp):
        self.tmp = tmp
        self.count = 0

    def __call__(self, program, macros, flags=(), obj=False, extra=()):
        self.count += 1
        d = os.path.join(self.tmp, f"b{self.count}")
        os.mkdir(d)
        sources = []
        for i, text in enumerate((program, *extra)):
            path = os.path.join(d, f"s{i}.c")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            sources.append(path)
        out = os.path.join(d, "out.o" if obj else "out")
        cmd = ["gcc", "-O2", "-fPIE", *flags]
        cmd += [f"-D{k}={v}" for k, v in macros.items()]
        cmd += ["-c"] if obj else ["-pie", "-Wl,--build-id=sha1"]
        subprocess.run([*cmd, *sources, "-o", out], check=True,
                       capture_output=True)
        return out


@unittest.skipUnless(HAVE_TOOLCHAIN, "needs gcc, objdump, strip, x86-64")
class CompiledTest(unittest.TestCase):
    """elfcmp and asmdiff on variants of the programs above."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="refactor-selftest-")
        cls.builder = Builder(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def build(self, flags=(), obj=False, program=PROGRAM, base=None,
              extra=(), **changes):
        macros = dict(BASE if base is None else base, **changes)
        return self.builder(program, macros, flags, obj, extra)

    def asmdiff(self, a, b, *extra):
        return run_tool(asmdiff, [a, b, *extra])

    def assert_same(self, a, b):
        self.assertEqual(run_tool(elfcmp, [a, b])[0], 1,
                         "the variants should differ in bytes")
        status, out = self.asmdiff(a, b)
        self.assertEqual(status, 0, out)

    def assert_changed(self, name, a, b):
        status, out = self.asmdiff(a, b)
        self.assertEqual(status, 1, out)
        self.assertIn(f"    {name} (", out)

    def test_identical_builds(self):
        a, b = self.build(), self.build()
        self.assertEqual(run_tool(elfcmp, [a, b])[0], 0)
        self.assertEqual(self.asmdiff(a, b)[0], 0)

    def test_alignment_is_ignored(self):
        self.assert_same(self.build(),
                         self.build(["-falign-functions=64",
                                     "-falign-loops=64",
                                     "-falign-jumps=16"]))

    def test_function_order_is_ignored(self):
        self.assert_same(self.build(), self.build(ORDER="1"))

    def test_thread_local_layout_is_ignored(self):
        self.assert_same(self.build(),
                         self.build(TLS_DECL="t_second[4], t_first"))

    def test_changed_constant(self):
        self.assert_changed("helper", self.build(),
                            self.build(HELPER_ADD="2"))

    def test_changed_global(self):
        self.assert_changed("main", self.build(),
                            self.build(COUNTER="g_other"))

    def test_changed_thread_local_field(self):
        self.assert_changed("main", self.build(),
                            self.build(TLS_INDEX="3"))

    def test_changed_string(self):
        self.assert_changed("main", self.build(),
                            self.build(MESSAGE='"total %d\\n"'))

    def test_inlining(self):
        status, out = self.asmdiff(self.build(),
                                   self.build(HELPER_ATTR=""))
        self.assertEqual(status, 1, out)
        self.assertIn("functions only in A:\n    helper (", out)

    def test_object_files(self):
        a = self.build(obj=True)
        status, out = self.asmdiff(a, self.build(obj=True, ORDER="1"))
        self.assertEqual(status, 0, out)
        for change in ({"COUNTER": "g_other"}, {"TLS_INDEX": "3"},
                       {"MESSAGE": '"total %d\\n"'}):
            self.assert_changed("main", a, self.build(obj=True, **change))

    def test_object_relocations(self):
        args = {"program": OBJ_PROGRAM, "base": OBJ_BASE, "obj": True}
        a = self.build(**args)
        self.assertEqual(run_tool(elfcmp, [a, self.build(**args)])[0], 0)
        for change in ({"TLS_INDEX": "3"}, {"COUNTER": "g_other"},
                       {"CALLEE": "ext_b"}):
            status, out = run_tool(elfcmp, [a, self.build(**args,
                                                          **change)])
            self.assertEqual(status, 1, out)
            self.assertIn("relocations of .text", out)

    def test_object_tail_calls(self):
        args = {"program": TAIL_PROGRAM, "base": {}, "obj": True}
        status, out = self.asmdiff(self.build(ORDER="0", **args),
                                   self.build(ORDER="1", **args))
        self.assertEqual(status, 0, out)

    def test_data_values(self):
        args = {"program": DATA_PROGRAM, "base": DATA_BASE}
        a = self.build(**args)
        status, out = self.asmdiff(a, self.build(THRESH="6", **args))
        self.assertEqual(status, 1, out)
        self.assertIn("data objects changed:\n    threshold (", out)
        status, out = self.asmdiff(a, self.build(TABV="9", **args))
        self.assertEqual(status, 1, out)
        self.assertIn("    table (", out)
        self.assert_changed("scale", a, self.build(DCONST="0.25", **args))
        self.assert_changed("main", a,
                            self.build(SGR='"\\33[01;32m"', **args))
        self.assert_changed("main", a, self.build(TAIL='"y"', **args))

    def test_calls_through_the_got(self):
        args = {"program": GOT_PROGRAM, "base": {}, "flags": ["-fno-plt"]}
        a = self.build(CALL="getpid", **args)
        self.assertEqual(self.asmdiff(a, self.build(CALL="getpid",
                                                    **args))[0], 0)
        self.assert_changed("main", a, self.build(CALL="getppid", **args))

    def test_thread_locals_of_another_file(self):
        args = {"program": TLS_USER, "base": {}}
        a = self.build(extra=[TLS_DEFS], ORDER="0", WHICH="flag", **args)
        b = self.build(extra=[TLS_DEFS], ORDER="1", WHICH="flag", **args)
        self.assert_same(a, b)
        c = self.build(extra=[TLS_DEFS], ORDER="0", WHICH="other[1]",
                       **args)
        self.assert_changed("touch", a, c)

    def test_thread_pointer_across_calls(self):
        # A call may not return, so asmdiff does not follow the thread
        # pointer across one: a new layout shows after the call.
        args = {"program": SPILL_PROGRAM, "base": {}}
        a = self.build(TLS_BUFS="b_one, b_two[2]", **args)
        b = self.build(TLS_BUFS="b_two[2], b_one", **args)
        self.assert_changed("spill", a, b)
        self.assert_changed("keep", a, b)
        _, funcs = asmdiff.functions(a, "objdump")
        for name in ("spill", "keep"):
            text = "\n".join(funcs[name])
            self.assertIn("b_one(%", text)       # before the call
            self.assertNotIn("b_two", text)      # after it

    def test_thread_pointer_at_joins(self):
        # The thread pointer reaches label 1 of join and label 2 of inner
        # on only one of their paths, and swap and wide overwrite it.
        args = {"program": JOIN_PROGRAM, "base": {}}
        a = self.build(TLS_VARS="j_one, j_two[2]", **args)
        b = self.build(TLS_VARS="j_two[2], j_one", **args)
        for name in ("join", "inner", "swap", "wide"):
            self.assert_changed(name, a, b)
        status, out = self.asmdiff(a, b)
        self.assertNotIn("    main (", out)

    def test_unknown_thread_local_offset(self):
        # Offsets that name no thread-local symbol stay apart, both
        # through a register and in %fs:OFFSET.
        args = {"program": PAD_PROGRAM, "base": {}}
        a = self.build(OFF_REG='"-1"', OFF_FS='"-1"', **args)
        for change in ({"OFF_REG": '"-2"'}, {"OFF_FS": '"-2"'}):
            macros = dict({"OFF_REG": '"-1"', "OFF_FS": '"-1"'}, **change)
            self.assert_changed("peek", a, self.build(**macros, **args))

    def test_thread_local_lines(self):
        args = {"program": TLS_LINES, "base": {}}
        a = self.build(ALIGN="", **args)
        status, out = self.asmdiff(a, self.build(ALIGN="_Alignas (64)",
                                                 **args))
        self.assertEqual(status, 0, out)
        self.assertIn("0 pairs of thread-local objects now share a line, "
                      "1 no longer do", out)
        self.assertIn("now apart: x, y", out)
        status, out = self.asmdiff(a, a)
        self.assertIn("thread-local objects share lines as before", out)
        # Offsets from the thread pointer, at the end of the block.
        syms = asmdiff.Symbols(a)
        placed = asmdiff.data_objects(syms)[2]
        self.assertEqual(sorted(name for _, _, name in placed), ["x", "y"])
        for start, size, name in placed:
            self.assertTrue(-syms.tls_size <= start < start + size <= 0,
                            (name, start, size, syms.tls_size))

    def test_jump_table(self):
        args = {"program": SWITCH_PROGRAM, "base": {}}
        a = self.build(ORDER="0", **args)
        self.assert_same(a, self.build(ORDER="1", **args))
        _, funcs = asmdiff.functions(a, "objdump")
        self.assertTrue(any("jt[" in i for i in funcs["pick"]),
                        funcs["pick"])

    def test_watch_list(self):
        a, b = self.build(), self.build(HELPER_ADD="2")
        watch = os.path.join(self.tmp, "watch.txt")
        for pattern, want in (("help*", 1), ("other", 0)):
            with open(watch, "w", encoding="utf-8") as f:
                f.write(f"# comment\n{pattern}\n")
            status, out = self.asmdiff(a, b, "--watch", watch,
                                       "--fail-on", "watch")
            self.assertEqual(status, want, out)
        self.assertIn("no out-of-line function", out)

    def test_refuses_stripped_files(self):
        a = self.build()
        b = a + ".stripped"
        subprocess.run(["strip", "-o", b, a], check=True)
        status, out = self.asmdiff(a, b)
        self.assertEqual(status, 2, out)
        self.assertIn("no symbol table", out)

    def test_refuses_other_objdumps(self):
        other = shutil.which("llvm-objdump") or shutil.which(
            "llvm-objdump-19")
        if not other:
            self.skipTest("no llvm-objdump")
        a = self.build()
        status, out = self.asmdiff(a, a, "--objdump", other)
        self.assertEqual(status, 2, out)

    def test_build_id_and_debug_info(self):
        a = self.build(["-g"])
        b = self.build(["-g"], program="\n" + PROGRAM)
        status, out = run_tool(elfcmp, [a, b])
        self.assertEqual(status, 0, out)
        status, out = run_tool(elfcmp, ["--strict", a, b])
        self.assertEqual(status, 1, out)
        self.assertIn(".note.gnu.build-id", out)

    def test_build_directories(self):
        prog = self.build()
        for tree in ("ta", "tb"):
            os.makedirs(os.path.join(self.tmp, tree, "src"))
            for name in ("grep", "grep-hooks", "grep-shrunk"):
                shutil.copy(prog, os.path.join(self.tmp, tree, "src",
                                               name))
        status, out = run_tool(elfcmp, [os.path.join(self.tmp, "ta"),
                                        os.path.join(self.tmp, "tb")])
        self.assertEqual(status, 0, out)
        self.assertEqual(out.count("identical"), 3)


class SharedLinesTest(unittest.TestCase):
    """asmdiff reports which writable objects share a cache line."""

    def test_pairs(self):
        pairs = asmdiff.shared_lines([(0, 8, "a"), (8, 8, "b"),
                                      (64, 4, "c"), (96, 64, "d#2"),
                                      (140, 4, "e")])
        self.assertEqual(pairs, {("a", "b"), ("c", "d"), ("d", "e")})

    def test_thread_pointer_offsets(self):
        # Offsets below a 64-byte aligned thread pointer.
        pairs = asmdiff.shared_lines([(-72, 8, "a"), (-64, 8, "b"),
                                      (-56, 8, "c")])
        self.assertEqual(pairs, {("b", "c")})


SC_LOG = """\
long_lines
src/grep.c:10:{long}
benchmarks/x.py:3:{long2}
maint.mk: line(s) with more than 80 characters; reindent
make: *** [cfg.mk:117: sc_long_lines] Error 1
avoid_if_before_free
src/dfasearch.c: {if_line}
    {free_line}
{halt}
sorted_tests
sort: -:92: disorder: parallel-x
{sorted_error}
trailing_blank
0.04 trailing_blank
"""


class ScDiffTest(unittest.TestCase):
    """sc_diff keys violations by rule and text."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="sc-diff-selftest-")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def diff(self, a_text, b_text):
        paths = []
        for name, text in (("a.log", a_text), ("b.log", b_text)):
            path = os.path.join(self.tmp, name)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            paths.append(path)
        return run_tool(sc_diff, paths)

    def log(self, old_make=False, **kw):
        # A halt message and make's error, on one line as make -k
        # interleaves them.  make 3.81 omits the file and line.
        halt = ('maint.mk: found useless "if"'
                "make: *** [maint.mk:364: sc_avoid_if_before_free] Error 1")
        sorted_error = ("make: *** [sc_sorted_tests] Error 1" if old_make
                        else "make: *** [cfg.mk:138: sc_sorted_tests] "
                        "Error 1")
        # Built at run time, so that syntax-check does not flag this
        # file for a useless "if" before free.
        if_line, free_line = "if (p)", "fr" + "ee (p);"
        return SC_LOG.format(long=kw.get("long", "x" * 90),
                             long2=kw.get("long2", "y" * 90), halt=halt,
                             sorted_error=sorted_error, if_line=if_line,
                             free_line=free_line)

    def test_same_log(self):
        self.assertEqual(self.diff(self.log(), self.log())[0], 0)

    def test_old_make(self):
        status, out = self.diff(self.log(), self.log(old_make=True))
        self.assertEqual(status, 0, out)
        self.assertIn("4 rules ran", out)

    def test_moved_violations(self):
        b = self.log().replace("src/grep.c:10:", "src/walk.c:99:")
        b = b.replace("src/dfasearch.c: ", "src/dfa-prefilter.c: ")
        b = b.replace("-:92:", "-:93:")
        status, out = self.diff(self.log(), b)
        self.assertEqual(status, 0, out)

    def test_added_violation(self):
        b = self.log().replace("benchmarks/x.py:3:",
                               "src/new.c:1:" + "z" * 90
                               + "\nbenchmarks/x.py:3:")
        status, out = self.diff(self.log(), b)
        self.assertEqual(status, 1, out)
        self.assertIn("sc_long_lines: src/new.c:1:", out)

    def test_duplicate_counts(self):
        b = self.log().replace("benchmarks/x.py:3:" + "y" * 90,
                               "benchmarks/x.py:3:" + "y" * 90
                               + "\nsrc/z.c:5:" + "y" * 90)
        status, out = self.diff(self.log(), b)
        self.assertEqual(status, 1, out)

    def test_rule_that_did_not_run(self):
        b = self.log().split("sorted_tests")[0]
        status, out = self.diff(self.log(), b)
        self.assertEqual(status, 1, out)
        self.assertIn("ran in A but not in B", out)


class RouteMatrixTest(unittest.TestCase):
    """The matrix has the documented shape."""

    def test_case_count(self):
        per_thread = (len(route_matrix.STDINS)
                      * len(route_matrix.OPERANDS)
                      * len(route_matrix.OPTIONS) + 4
                      + 2 * len(route_matrix.TARGETED))
        self.assertEqual(len(route_matrix.cases([1, 2, 32])),
                         3 * per_thread)
        self.assertEqual(len(route_matrix.cases([1], utf8=False)),
                         per_thread - 2)
        self.assertEqual(route_matrix.parse_size("4M"), 4 << 20)


class AbSummaryTest(unittest.TestCase):
    """ab_bench --summarize applies the decision rule."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="ab-selftest-")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def summarize(self, ratios, out_b="x", **fields):
        import ab_bench
        path = os.path.join(self.tmp, "raw.jsonl")
        with open(path, "w", encoding="utf-8") as f:
            for r, ratio in enumerate(ratios):
                for build, wall in (("A", 10.0), ("B", 10.0 * ratio)):
                    rec = {"scenario": "T-01", "phase": "warm",
                           "threads": None, "locale": "C", "round": r,
                           "is_warmup": False, "build": build,
                           "wall_ms": wall, "session": 1,
                           "exit_code": 0, "stdout_sha256":
                           out_b if build == "B" else "x"}
                    rec.update(fields)
                    f.write(json.dumps(rec) + "\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = ab_bench.summarize([self.tmp])
        return status, out.getvalue()

    def test_verdicts(self):
        jitter = [(-1) ** i * 0.002 * (i % 3) for i in range(20)]
        status, out = self.summarize([1.03 + j for j in jitter])
        self.assertEqual(status, 1, out)
        self.assertIn("slower", out)
        status, out = self.summarize([1.0 + j for j in jitter])
        self.assertEqual(status, 0, out)
        self.assertIn("same", out)
        status, out = self.summarize([1.026 + j for j in jitter[:10]]
                                     + [1.0 + j for j in jitter[10:]])
        self.assertEqual(status, 3, out)
        self.assertIn("inconclusive", out)
        status, out = self.summarize([0.9, 1.1] * 10)
        self.assertEqual(status, 3, out)
        self.assertIn("wide", out)

    def test_few_pairs_give_no_verdict(self):
        status, out = self.summarize([1.02, 1.03, 1.05])
        self.assertEqual(status, 3, out)
        self.assertIn("few", out)
        self.assertNotIn("slower", out)

    def test_output_difference(self):
        status, out = self.summarize([1.0] * 20, out_b="y")
        self.assertEqual(status, 1, out)
        self.assertIn("OUTPUT DIFFERS", out)

    def test_timed_out_runs_are_left_out(self):
        status, out = self.summarize([1.0] * 20, timed_out=True)
        self.assertEqual(status, -1, out)


if __name__ == "__main__":
    unittest.main()
