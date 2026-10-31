#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Compare the machine code and data of two builds, function by function.

usage: asmdiff.py [--watch FILE] [--fail-on any|watch|none] [--show N]
                  [--diff NAME]... A B

A and B are unstripped x86-64 ELF executables or object files.
asmdiff disassembles both with GNU objdump and compares every function
after normalizing what changes when code only moves:

- a branch target becomes the index of the target instruction (L7),
  with the function name if it is in another function (name+L7);
- a RIP-relative operand becomes what it addresses: a symbol plus the
  offset into it, a pointer slot (such as a GOT entry) named by what
  it points to, the targets of a jump table, a string, a constant
  (its bytes, when the instruction shows their size), or else the
  section;
- a thread-local operand (%fs:OFFSET, an offset from the thread
  pointer loaded from %fs:0x0, or an offset loaded into the register
  of %fs:(REG)) becomes the thread-local symbol; what a register holds
  is followed only along straight-line code, up to a call or an
  instruction that another path can reach, and an offset that names
  no thread-local symbol stays as it is;
- alignment padding is dropped;
- GCC clone and local-static numbers (.part.N, .isra.N, used.0) and
  LTO partition names (.lto_priv.N, .localalias) are dropped.

In object files, relocations name the referenced symbols.

asmdiff also compares the contents of every named data object, with
pointers written as what they point to, and reports pairs of writable
objects that start or stop sharing a 64-byte cache line, and likewise
pairs of thread-local objects (assuming a 64-byte aligned thread
pointer, as in glibc): a new data layout can change speed without
changing any instruction.

Not checked: anonymous constants whose size no instruction shows,
and code or data reached only through computed addresses.  Two local
symbols whose names differ only in an LTO or clone number get the same
name.

The watch file lists one function name or shell pattern per line; "#"
starts a comment.  A name matches the function and its clones
(name.part, name.isra, name.constprop, name.cold).

Exit status: 1 if a reported change counts as a failure under
--fail-on (any: a changed, added or removed function or data object,
the default; watch: a change to a watched function; none: never), 0
otherwise, 2 on error.  Cache-line changes do not change the exit
status; time them with an A/B run.
"""

import argparse
import bisect
import collections
import difflib
import fnmatch
import hashlib
import itertools
import os
import re
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elfcmp  # noqa: E402

EM_X86_64 = 62
ET_REL = 1
STT_OBJECT, STT_FUNC, STT_SECTION, STT_FILE = 1, 2, 3, 4
STT_TLS, STT_GNU_IFUNC = 6, 10
SHF_WRITE, SHF_EXECINSTR, SHF_MERGE = 0x1, 0x4, 0x10
SHT_NOBITS = 8
PT_GNU_RELRO = 0x6474E552
R_X86_64_RELATIVE, R_X86_64_IRELATIVE = 8, 37
CACHE_LINE = 64

LTO_TAG = re.compile(r"\.(lto_priv|localalias)(\.\d+)?")
NUMBER_TAG = re.compile(r"\.\d+")
PADDING = ("nop", "xchg %ax,%ax", "cs nop", "ds nop", "data16")

SECTION = re.compile(r"Disassembly of section (\S+):")
HEADER = re.compile(r"([0-9a-f]+) <(.+)>:")
# "ADDR:<TAB>BYTES <TAB>INSTRUCTION  # COMMENT<TAB>SITE: R_TYPE<TAB>SYM"
INSN = re.compile(r"\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*(?:\t(.*))?")
RELOC = re.compile(r"\s([0-9a-f]+): (R_[A-Z0-9_]+)\s+(\S+)")
TARGET = re.compile(r"\b([0-9a-f]+) <([^>]+)>")
RIP = re.compile(r"(-?0x[0-9a-f]+)\(%rip\)")
RIP_LEA = re.compile(r"lea -?0x[0-9a-f]+\(%rip\),%(\w+)$")
FS = re.compile(r"%fs:(-?0x[0-9a-f]+)(?!\()")
FS_REG = re.compile(r"%fs:(?:-?0x[0-9a-f]+)?\(%(\w+)")
TP_LOAD = re.compile(r"mov %fs:0x0,%(r\w+)$")
REG_COPY = re.compile(r"mov %(r\w+),%(r\w+)$")
IMM_LOAD = re.compile(r"mov \$(0x[0-9a-f]+),%(r\w+)$")
IMM_ARITH = re.compile(r"(add|sub) \$(0x[0-9a-f]+),%(r\w+)$")
CMP_IMM = re.compile(r"cmp[bwlq]? \$(0x[0-9a-f]+),\S+$")
LAST_REG = re.compile(r"%(\w+)$")
SYM_ADDEND = re.compile(r"(.*?)([-+]0x[0-9a-f]+)?")
PC_RELATIVE = ("R_X86_64_PC32", "R_X86_64_PLT32", "R_X86_64_PC64",
               "R_X86_64_GOTPCREL", "R_X86_64_GOTPCRELX",
               "R_X86_64_REX_GOTPCRELX")
NO_WRITE = ("push", "cmp", "test", "bt ", "call", "jmp", "notrack")
# Instruction prefixes, and the registers that instructions (without
# their size suffix) write without naming them as their last operand.
PREFIXES = ("lock", "rep", "repz", "repnz", "repe", "repne", "bnd",
            "notrack", "data16", "addr32")
IMPLICIT_WRITES = {
    "mul": ("rax", "rdx"), "imul": ("rax", "rdx"), "div": ("rax", "rdx"),
    "idiv": ("rax", "rdx"), "cqto": ("rdx",), "cltd": ("rdx",),
    "cwtd": ("rdx",), "cltq": ("rax",), "cwtl": ("rax",), "cbtw": ("rax",),
    "cpuid": ("rax", "rbx", "rcx", "rdx"), "rdtsc": ("rax", "rdx"),
    "rdtscp": ("rax", "rcx", "rdx"), "syscall": ("rax", "rcx", "r11"),
    "xchg": ("rax",), "xadd": ("rax",), "cmpxchg": ("rax", "rdx"),
    "cmpxchg8b": ("rax", "rdx"), "cmpxchg16b": ("rax", "rdx"),
    "lods": ("rax", "rsi"), "stos": ("rdi",), "movs": ("rsi", "rdi"),
    "scas": ("rdi",), "cmps": ("rsi", "rdi"),
}
# Instructions that write each register they name.
WRITE_ALL = ("xchg", "xadd", "cmpxchg")
# A call, and an instruction after which execution does not go on to
# the next one.
CALL = re.compile(r"(?:(?:bnd|notrack) )?call\b")
NO_FALLTHROUGH = re.compile(r"(?:(?:rep|repz|bnd|notrack) )?(?:ret|jmp)\b"
                            r"|(?:ud2|hlt)\b")
# In normalized text, an instruction of the current function (L7), and
# one of another function (name+L7).
LOCAL_LABEL = re.compile(r"(?<![^\s,(\[])L(\d+)\b")
FOREIGN_LABEL = re.compile(r"([^\s,(\[\]<>+]+)\+L(\d+)\b")
TEXT_CONTROLS = b"\t\n\r\x07\x08\x0b\x0c\x1b"
SIZE_BY_MNEMONIC = {
    "vpbroadcastb": 1, "vpbroadcastw": 2, "vpbroadcastd": 4,
    "vpbroadcastq": 8, "vbroadcastss": 4, "vbroadcastsd": 8,
    "vbroadcasti128": 16, "vbroadcastf128": 16,
    "movd": 4, "vmovd": 4, "movq": 8, "vmovq": 8,
}

# The 64-bit register that each register name is part of, and the size
# of each name.
REGS = {}
REG_SIZE = {}
for _r in "abcd":
    for _n, _s in ((f"r{_r}x", 8), (f"e{_r}x", 4), (f"{_r}x", 2),
                   (f"{_r}l", 1), (f"{_r}h", 1)):
        REGS[_n], REG_SIZE[_n] = f"r{_r}x", _s
for _r in ("si", "di", "bp", "sp"):
    for _n, _s in ((f"r{_r}", 8), (f"e{_r}", 4), (_r, 2), (f"{_r}l", 1)):
        REGS[_n], REG_SIZE[_n] = f"r{_r}", _s
for _i in range(8, 16):
    for _n, _s in ((f"r{_i}", 8), (f"r{_i}d", 4), (f"r{_i}w", 2),
                   (f"r{_i}b", 1)):
        REGS[_n], REG_SIZE[_n] = f"r{_i}", _s


class ToolError(Exception):
    """asmdiff cannot compare the given files."""


def norm_name(name):
    """Drop LTO partition names and clone or local-static numbers."""
    return NUMBER_TAG.sub("", LTO_TAG.sub("", name))


def base_name(name):
    """The C name of a normalized function name: drop clone tags."""
    return name.split("#", 1)[0].split(".", 1)[0]


def signed64(value):
    """Interpret VALUE, printed by objdump as unsigned, as signed."""
    return value - (1 << 64) if value >= 1 << 63 else value


def written_registers(ins):
    """Return the 64-bit registers that instruction INS may write, as a
    superset: its last register operand unless the instruction only
    reads it, each register an exchange names, and implicit outputs
    such as those of mul, cpuid or rep stos."""
    words = ins.split()
    rep = False
    while len(words) > 1 and words[0] in PREFIXES:
        rep = rep or words[0].startswith("rep")
        words = words[1:]
    op = words[0] if words else ""
    written = set(IMPLICIT_WRITES.get(op)
                  or IMPLICIT_WRITES.get(op[:-1], ()))
    if rep:
        written.update(("rcx", "rsi", "rdi"))
    if op.startswith(WRITE_ALL):
        written.update(REGS.get(r, r) for r in re.findall(r"%(\w+)", ins))
    lm = LAST_REG.search(ins)
    if lm and not op.startswith(NO_WRITE):
        written.add(REGS.get(lm.group(1), lm.group(1)))
    return written


def short_hash(data):
    return hashlib.sha1(data).hexdigest()[:12]


def string_at(data, off):
    """Describe the NUL-terminated text at DATA[OFF:], or return None
    if the bytes there are not UTF-8 text."""
    end = data.find(b"\0", off)
    if end <= off:
        return None
    text = data[off:end]
    if any(c < 0x20 and c not in TEXT_CONTROLS or c == 0x7F
           for c in text):
        return None
    try:
        shown = text.decode("utf-8")
    except UnicodeDecodeError:
        return None
    shown = shown.encode("unicode_escape").decode("ascii")
    if len(shown) > 40:
        shown = shown[:40] + "..." + short_hash(text)
    return '"' + shown.replace('"', r'\"') + '"'


def constant_text(data):
    """Describe constant bytes DATA."""
    if len(data) <= 16:
        return "const:" + data.hex()
    return "const:" + short_hash(data)


def operand_size(ins):
    """Return the size in bytes of the memory operand of the normalized
    instruction INS, or None if its text does not show it."""
    mnem, _, ops = ins.partition(" ")
    if mnem in SIZE_BY_MNEMONIC:
        return SIZE_BY_MNEMONIC[mnem]
    if mnem == "lea" or mnem.startswith(("j", "call", "nop", "prefetch")):
        return None
    vec = re.findall(r"%([xyz])mm\d+", ops)
    if vec:
        if mnem.endswith("ss"):
            return 4
        if mnem.endswith("sd"):
            return 8
        if re.search(r"pmov[sz]x|cvt|insr|extr|128|perm|gather", mnem):
            return None
        return {"x": 16, "y": 32, "z": 64}[max(vec, key="xyz".index)]
    m = re.fullmatch(r"mov[sz]([bwl])[wlq]?", mnem)
    if m:
        return {"b": 1, "w": 2, "l": 4}[m.group(1)]
    outside = re.sub(r"\([^)]*\)", "", ops)
    sizes = [REG_SIZE[r] for r in re.findall(r"%(\w+)", outside)
             if r in REG_SIZE]
    if sizes:
        return sizes[-1]
    return {"b": 1, "w": 2, "l": 4, "q": 8}.get(mnem[-1:])


class Symbols:
    """Describe what addresses, thread-local offsets and section offsets
    of one ELF file refer to."""

    def __init__(self, path):
        self.elf = elf = elfcmp.Elf(path)
        if elf.machine != EM_X86_64:
            raise ToolError(f"{path}: not an x86-64 file")
        self.relocatable = elf.type == ET_REL
        if not self.relocatable and not elf.has_symtab():
            raise ToolError(f"{path}: no symbol table; asmdiff needs "
                            "unstripped files")
        self.tls_size = elf.tls_size()
        self.tls_segment = elf.tls_segment()
        self.symbols = elf.symbols()
        defined = [s for s in self.symbols
                   if s.name and s.shndx != 0
                   and s.type not in (STT_SECTION, STT_FILE)
                   and not s.name.startswith(".L")]
        self.defined = defined
        self.addr = self._index(
            (s.value, s.size, s.name) for s in defined
            if s.type in (STT_OBJECT, STT_FUNC, STT_GNU_IFUNC))
        self.tls = self._index((s.value, s.size, s.name) for s in defined
                               if s.type == STT_TLS)
        per_section = collections.defaultdict(list)
        for s in defined:
            per_section[s.shndx].append((s.value, s.size, s.name))
        self.in_section = {k: self._index(v)
                           for k, v in per_section.items()}
        self.alloc = sorted(((s.addr, s.size, s.index, s)
                             for s in elf.sections if s.alloc and s.size),
                            key=lambda e: e[:3])
        self.alloc_starts = [e[0] for e in self.alloc]
        self.section_index = {}
        for s in elf.sections:
            self.section_index.setdefault(s.name, s.index)
        self.labels_by_name = {s.name: (s.shndx, s.value)
                               for s in self.symbols
                               if s.name.startswith(".L") and s.shndx}
        # Dynamic relocations of a linked file, by address: the pointer
        # slots that the dynamic linker fills in.
        self.dyn = {}
        self.relocs = {}
        if self.relocatable:
            self.relocs = elf.relocations()
        else:
            for entries in elf.relocations(allocated_only=True).values():
                for off, rtype, sym, addend in entries:
                    self.dyn[off] = (rtype, sym, addend)
        self.code = {}            # (section, address) -> (function, index)
        self.jump_table_sizes = {}  # address -> entries (0 if unknown)

    @staticmethod
    def _index(entries):
        entries = sorted(set(entries))
        return [e[0] for e in entries], entries

    @staticmethod
    def _lookup(index, value):
        """Name the symbol of INDEX that contains VALUE, plus the offset
        into it, or return None."""
        starts, entries = index
        # Symbols seldom overlap; aliases share a start address, so
        # looking at the few nearest starts below VALUE is enough.
        hi = bisect.bisect_right(starts, value)
        for start, size, name in reversed(entries[max(0, hi - 8):hi]):
            if value < start + size or value == start:
                name = norm_name(name)
                off = value - start
                return f"{name}+{off:#x}" if off else name
        return None

    def section_at(self, addr):
        i = bisect.bisect_right(self.alloc_starts, addr) - 1
        if i >= 0:
            start, size, _, sec = self.alloc[i]
            if addr < start + size:
                return sec
        return None

    def code_label(self, section, addr, cur):
        """Name the instruction at ADDR in SECTION as an index, prefixed
        with its function unless that is CUR."""
        hit = self.code.get((section, addr))
        if hit is None:
            return None
        func, idx = hit
        if func == cur:
            return f"L{idx}"
        return func if idx == 0 else f"{func}+L{idx}"

    def pointer(self, addr):
        """Describe the pointer slot at ADDR of a linked file, or return
        None if no dynamic relocation fills it."""
        hit = self.dyn.get(addr)
        if hit is None:
            return None
        rtype, sym, addend = hit
        if sym is not None and sym.name:
            name = norm_name(sym.name)
            return f"ptr:{name}+{addend:#x}" if addend else f"ptr:{name}"
        if rtype in (R_X86_64_RELATIVE, R_X86_64_IRELATIVE):
            return "ptr:" + (self.address(addend, None, None, None,
                                          follow=False) or "?")
        return f"ptr:type{rtype}"

    def address(self, addr, size, table, cur, follow=True):
        """Describe what ADDR addresses in a linked file.  SIZE is the
        size of the operand if known; TABLE, if not None, says that ADDR
        is a jump table with that many entries (0 if unknown); CUR is
        the current function."""
        sym = self._lookup(self.addr, addr)
        if sym:
            return sym
        if follow:
            ptr = self.pointer(addr)
            if ptr:
                return ptr
        sec = self.section_at(addr)
        if sec is None:
            return None
        if sec.flags & SHF_EXECINSTR:
            return self.code_label(sec.name, addr, cur) or sec.name
        if table is not None:
            return self.jump_table(sec, addr, table, cur)
        return self.data_at(sec, addr - sec.addr, size,
                            covered=lambda off: sec.addr + off in self.dyn)

    def data_at(self, sec, off, size, covered):
        """Describe the read-only data at offset OFF of SEC: a string,
        or a constant of SIZE bytes, or else just the section."""
        if (sec.type == SHT_NOBITS or sec.flags & SHF_WRITE
                or covered(off)):
            return sec.name
        data = sec.data()
        if size:
            return constant_text(data[off:off + size])
        if sec.flags & SHF_MERGE and sec.entsize and not string_at(
                data, off):
            return constant_text(data[off:off + sec.entsize])
        return string_at(data, off) or sec.name

    def jump_table(self, sec, addr, count, cur):
        """Describe the jump table at ADDR by its branch targets."""
        data = sec.data()
        off = addr - sec.addr
        targets = []
        for k in range(count or 1024):
            p = off + 4 * k
            if p + 4 > len(data):
                break
            entry = int.from_bytes(data[p:p + 4], "little", signed=True)
            dest = self.section_at(addr + entry)
            label = dest and self.code_label(dest.name, addr + entry, cur)
            # A jump table branches within its function (or its .cold
            # part); with no known size, stop at the first entry that
            # does not.
            inside = label and (label.startswith("L") or base_name(
                label.split("+", 1)[0]) == base_name(cur or ""))
            if not inside:
                if not count:
                    break
                label = label or "?"
            targets.append(label)
        return "jt[" + ",".join(targets) + "]"

    def thread_local(self, tpoff):
        """Describe the thread-local data at thread-pointer offset
        TPOFF in a linked file (x86-64: the block ends at the thread
        pointer)."""
        if self.tls_size is None or tpoff >= 0:
            return None
        return self._lookup(self.tls, tpoff + self.tls_size)

    def section_offset(self, shndx, off, size, cur):
        """Describe offset OFF in section SHNDX of an object file."""
        sym = self._lookup(self.in_section.get(shndx, ([], [])), off)
        if sym:
            return sym
        if shndx >= len(self.elf.sections):
            return None
        sec = self.elf.sections[shndx]
        if sec.flags & SHF_EXECINSTR:
            return self.code_label(sec.name, off, cur) or sec.name
        if not sec.alloc:
            return sec.name
        covered = {r[0] for r in self.relocs.get(shndx, ())}
        return self.data_at(sec, off, size,
                            covered=lambda o: any(o <= c < o + 8
                                                  for c in covered))


def check_objdump(objdump):
    """Fail unless OBJDUMP is GNU objdump, whose output asmdiff parses."""
    try:
        out = subprocess.run([objdump, "--version"], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise ToolError(f"cannot run {objdump}: {e}") from e
    if "GNU objdump" not in (out.splitlines() or [""])[0]:
        raise ToolError(f"{objdump} is not GNU objdump")


def disassemble(path, objdump):
    """Return [(raw function name, section, instructions)] in file
    order.  Each instruction is (address, length, text, comment,
    relocations), and each relocation (address, type, SYMBOL[+-ADDEND]).
    """
    out = subprocess.run([objdump, "-d", "-r", "-w", path],
                         capture_output=True, text=True, check=True,
                         env=dict(os.environ, LC_ALL="C")).stdout
    funcs = []
    section = None
    cur = None
    for line in out.splitlines():
        m = SECTION.fullmatch(line)
        if m:
            section, cur = m.group(1), None
            continue
        m = HEADER.fullmatch(line)
        if m:
            cur = []
            funcs.append((m.group(2), section, cur))
            continue
        m = INSN.fullmatch(line) if cur is not None else None
        if not m:
            continue
        rest = m.group(3) or ""
        relocs = [(int(r.group(1), 16), r.group(2), r.group(3))
                  for r in RELOC.finditer(rest)]
        text, _, comment = RELOC.sub("", rest).partition("#")
        text = re.sub(r"\s+", " ", text).strip()
        if text:
            cur.append((int(m.group(1), 16), len(m.group(2).split()),
                        text, comment.strip(), relocs))
    return funcs


def index_code(syms, funcs):
    """Name the functions, map each instruction address (and padding,
    to the next instruction) to (function, index) in SYMS.code, and
    return [(name, section, instructions without padding)]."""
    result = []
    seen = collections.Counter()
    for raw, section, insns in funcs:
        name = norm_name(raw)
        seen[name] += 1
        key = name if seen[name] == 1 else f"{name}#{seen[name]}"
        real = []
        pending = []
        for insn in insns:
            if insn[2].startswith(PADDING):
                pending.append(insn[0])
                continue
            for addr in pending + [insn[0]]:
                syms.code[(section, addr)] = (key, len(real))
            pending = []
            real.append(insn)
        result.append((key, section, real))
    return result


class Normalizer:
    """Turn the instructions of one function into comparable text."""

    def __init__(self, syms):
        self.syms = syms
        self.cur = self.section = self.shndx = None

    def texts(self, key, section, insns):
        """Return the instructions INSNS of function KEY in SECTION as
        normalized text, except for thread-local offsets in registers
        (see thread_pointers)."""
        self.cur, self.section = key, section
        self.shndx = self.syms.section_index.get(section)
        tables = self.jump_tables(insns)
        return [self.insn(text, comment, relocs, addr + length,
                          tables.get(i))
                for i, (addr, length, text, comment, relocs)
                in enumerate(insns)]

    def thread_pointers(self, texts, entries=()):
        """Resolve the thread-local offsets held in registers in TEXTS,
        the normalized instructions of one function of a linked file.
        What the registers hold is followed only along straight-line
        code: it is forgotten at each call and at each instruction that
        another path can reach, namely one that a branch, jump table or
        address of the function names, or whose index is in ENTRIES (a
        branch target in another function)."""
        joins = set(entries)
        for ins in texts:
            joins.update(int(k) for k in LOCAL_LABEL.findall(ins))
        result = []
        tp_regs = set()
        imm_regs = {}
        for i, ins in enumerate(texts):
            if i in joins:
                tp_regs.clear()
                imm_regs.clear()
            result.append(self.thread_pointer(ins, result, tp_regs,
                                              imm_regs))
        return result

    @staticmethod
    def jump_tables(insns):
        """Map the index of each instruction that loads the address of
        a jump table to its number of entries, or 0 if unknown.  The
        table is the base of a later "movslq (%REG,%X,4)" before REG
        changes."""
        tables = {}
        for i, insn in enumerate(insns):
            m = RIP_LEA.match(insn[2])
            if not m:
                continue
            reg = REGS.get(m.group(1), m.group(1))
            load = f"movslq (%{m.group(1)},%"
            found = False
            for later in insns[i + 1:i + 65]:
                text = later[2]
                if load in text:
                    found = True
                    break
                if text.startswith(("call", "ret", "jmp")):
                    break
                lm = LAST_REG.search(text)
                if (lm and "," in text and not text.startswith(NO_WRITE)
                        and REGS.get(lm.group(1)) == reg):
                    break
            if not found:
                continue
            count = 0
            for j in range(i - 1, max(-1, i - 9), -1):
                c = CMP_IMM.match(insns[j][2])
                if c:
                    if any(insns[k][2].startswith("ja ")
                           for k in range(j + 1, i)):
                        count = int(c.group(1), 16) + 1
                    break
            tables[i] = count
        return tables

    def target(self, m):
        """Name the branch target that objdump printed as ADDR <SYM>."""
        syms = self.syms
        addr = int(m.group(1), 16)
        label = syms.code_label(self.section, addr, self.cur)
        if label is None and not syms.relocatable:
            sec = syms.section_at(addr)
            label = sec and syms.code_label(sec.name, addr, self.cur)
        return label or "<" + norm_name(m.group(2).split("+", 1)[0]) + ">"

    def insn(self, text, comment, relocs, end, table):
        syms = self.syms
        size = operand_size(text)
        if relocs:
            # The relocation names the target; objdump's annotation of a
            # relocated branch shows the unrelocated displacement.
            ins = TARGET.sub("R", text)
            ins = RIP.sub("X(%rip)", ins)
            ins = FS.sub(lambda m: "%fs:X" if m.group(1) != "0x0"
                         else m.group(0), ins)
            for site, rtype, target in relocs:
                ins += " " + self.reloc_text(rtype, target, site, end,
                                             size)
            return ins
        ins = TARGET.sub(self.target, text)
        cm = re.match(r"([0-9a-f]+)", comment)

        def rip(m):
            what = None
            if cm and syms.relocatable and self.shndx is not None:
                what = syms.section_offset(self.shndx, int(cm.group(1), 16),
                                           size, self.cur)
            elif cm and not syms.relocatable:
                addr = int(cm.group(1), 16)
                tbl = table
                if tbl is None and text.startswith("lea "):
                    # Another load of a known jump table's address.
                    tbl = syms.jump_table_sizes.get(addr)
                what = syms.address(addr, size, tbl, self.cur)
            return f"{what or 'X'}(%rip)"

        ins = RIP.sub(rip, ins)
        if not syms.relocatable:
            ins = FS.sub(self.fs_operand, ins)
        return ins

    def fs_operand(self, m):
        off = signed64(int(m.group(1), 16))
        if off >= 0:                      # the thread control block
            return m.group(0)
        what = self.syms.thread_local(off)
        # An offset that names no thread-local symbol stays as it is,
        # so that two different such offsets still differ.
        return f"%fs:{what}" if what else m.group(0)

    def reloc_text(self, rtype, target, site, end, size):
        """Describe relocation RTYPE against TARGET (SYMBOL[+-ADDEND])
        at address SITE of an instruction that ends at END."""
        syms = self.syms
        m = SYM_ADDEND.fullmatch(target)
        name, addend = m.group(1), m.group(2) or ""
        if not name.startswith("."):
            return f"@{rtype} {norm_name(name)}{addend}"
        # The assembler refers to local symbols through their section,
        # and to constants through local labels such as .LC0.
        shndx, off = syms.section_index.get(name), 0
        if shndx is None:
            label = syms.labels_by_name.get(name)
            if label is None:
                return f"@{rtype} {name}"
            shndx, off = label
        off += int(addend or "0", 16)
        if rtype in PC_RELATIVE:
            off += end - site
        what = syms.section_offset(shndx, off, size, self.cur)
        return f"@{rtype} {what or name}"

    def thread_pointer(self, ins, result, tp_regs, imm_regs):
        """Resolve thread-local offsets held in registers: offsets from a
        register that holds the thread pointer, and immediates loaded
        into the register of a later %fs:(REG) operand.  Then update
        what the registers hold."""
        syms = self.syms
        m = FS_REG.search(ins)
        if m:
            hit = imm_regs.get(REGS.get(m.group(1), m.group(1)))
            what = hit and syms.thread_local(hit[1])
            if what:
                idx = hit[0]
                result[idx] = re.sub(r"\$0x[0-9a-f]+", f"$tls:{what}",
                                     result[idx], count=1)
        m = TP_LOAD.match(ins)
        if m:
            tp_regs.add(REGS.get(m.group(1), m.group(1)))
            return ins
        for reg in list(tp_regs):
            def disp(mm):
                what = syms.thread_local(int(mm.group(1), 16))
                return f"{what}(%{mm.group(2)}" if what else mm.group(0)
            ins = re.sub(r"(-0x[0-9a-f]+)\(%(" + reg + r")\b", disp, ins)
            am = IMM_ARITH.match(ins)
            if am and REGS.get(am.group(3)) == reg:
                imm = signed64(int(am.group(2), 16))
                what = syms.thread_local(imm if am.group(1) == "add"
                                         else -imm)
                if what:
                    ins = f"{am.group(1)} $tls:{what},%{am.group(3)}"
        cm = REG_COPY.match(ins)
        if cm and REGS.get(cm.group(1)) in tp_regs:
            tp_regs.add(REGS.get(cm.group(2), cm.group(2)))
            return ins
        im = IMM_LOAD.match(ins)
        if im:
            reg = REGS.get(im.group(2), im.group(2))
            tp_regs.discard(reg)
            imm_regs[reg] = (len(result), signed64(int(im.group(1), 16)))
            return ins
        # A call may not return, so the instruction after it may be
        # reached only from elsewhere.
        if CALL.match(ins) or NO_FALLTHROUGH.match(ins):
            tp_regs.clear()
            imm_regs.clear()
            return ins
        for reg in written_registers(ins):
            tp_regs.discard(reg)
            imm_regs.pop(reg, None)
        return ins


def functions(path, objdump):
    """Return (Symbols, {function name: [normalized instructions]})."""
    syms = Symbols(path)
    norm = Normalizer(syms)
    funcs = index_code(syms, disassemble(path, objdump))
    if not syms.relocatable:
        for _, _, insns in funcs:
            for i, count in Normalizer.jump_tables(insns).items():
                cm = re.match(r"([0-9a-f]+)", insns[i][3])
                if cm:
                    addr = int(cm.group(1), 16)
                    old = syms.jump_table_sizes.get(addr, 0)
                    syms.jump_table_sizes[addr] = max(old, count)
    result = collections.OrderedDict(
        (key, norm.texts(key, section, insns))
        for key, section, insns in funcs)
    if syms.relocatable:
        return syms, result
    entries = collections.defaultdict(set)
    for ins in itertools.chain.from_iterable(result.values()):
        for name, idx in FOREIGN_LABEL.findall(ins):
            entries[name].add(int(idx))
    for key in result:
        result[key] = norm.thread_pointers(result[key], entries[key])
    return syms, result


def reloc_width(rtype):
    """Bytes that relocation type RTYPE fills."""
    return 8 if rtype in (1, 6, 7, 8, 16, 17, 18, 24, 37) else 4


def span_text(data):
    return data.hex() if len(data) <= 32 else (
        f"#{short_hash(data)}/{len(data)}")


def object_bytes(syms, s, sec):
    """Return (start, data, relocations) of data object S in SEC, with
    each relocation as (offset in S, width, description); DATA is None
    for an object in a SHT_NOBITS section."""
    if syms.relocatable:
        start = s.value
        data = None if sec.type == SHT_NOBITS else (
            sec.data()[start:start + s.size])
        relocs = []
        for off, rtype, sym, addend in syms.relocs.get(s.shndx, ()):
            if not start <= off < start + s.size:
                continue
            if sym is None:
                what = "?"
            elif sym.type == STT_SECTION:
                what = syms.section_offset(sym.shndx, addend, None, None)
            else:
                what = norm_name(sym.name) + (f"{addend:+#x}" if addend
                                              else "")
            relocs.append((off - start, reloc_width(rtype), f"&{what}"))
        return start, data, sorted(relocs)
    if s.type == STT_TLS:
        if syms.tls_segment is None:
            return 0, None, []
        vaddr, filesz = syms.tls_segment
        if s.value >= filesz:
            return 0, None, []
        start = vaddr + s.value
        sec = syms.section_at(start)
    else:
        start = s.value
    if sec is None or sec.type == SHT_NOBITS:
        return start, None, []
    data = sec.data()[start - sec.addr:start - sec.addr + s.size]
    lo = bisect.bisect_left(syms.dyn_sorted, start)
    hi = bisect.bisect_left(syms.dyn_sorted, start + s.size)
    relocs = [(addr - start, reloc_width(syms.dyn[addr][0]),
               syms.pointer(addr)) for addr in syms.dyn_sorted[lo:hi]]
    return start, data, relocs


def describe_object(syms, s, sec):
    """Describe data object S: its section, size and contents, with
    pointers written as what they point to."""
    _, data, relocs = object_bytes(syms, s, sec)
    if data is None:
        return (sec.name, s.size, "zero")
    parts = []
    pos = 0
    for off, width, what in relocs:
        if off < pos:
            continue
        if off > pos:
            parts.append(span_text(data[pos:off]))
        parts.append(what)
        pos = off + width
    if pos < len(data):
        parts.append(span_text(data[pos:]))
    return (sec.name, s.size, *parts)


def data_objects(syms):
    """Return ({name: description} of the named data objects,
    [(start, size, name)] of the writable ones of a linked file, and
    [(offset from the thread pointer, size, name)] of its thread-local
    ones)."""
    elf = syms.elf
    syms.dyn_sorted = sorted(syms.dyn)
    relro = [(vaddr, vaddr + memsz)
             for p_type, _, _, vaddr, _, memsz, _ in elf.segments
             if p_type == PT_GNU_RELRO]
    groups = collections.defaultdict(list)
    for s in syms.defined:
        if s.type not in (STT_OBJECT, STT_TLS) or not s.size:
            continue
        if s.shndx >= len(elf.sections):
            continue                      # absolute or common symbols
        sec = elf.sections[s.shndx]
        if not sec.alloc or sec.flags & SHF_EXECINSTR:
            continue
        groups[norm_name(s.name)].append((describe_object(syms, s, sec),
                                          s, sec))
    objects = {}
    writable = []
    thread_local = []
    for name, members in groups.items():
        members.sort(key=lambda m: m[0])
        for k, (desc, s, sec) in enumerate(members):
            key = name if k == 0 else f"{name}#{k + 1}"
            objects[key] = desc
            if syms.relocatable:
                continue
            if (s.type == STT_OBJECT and sec.flags & SHF_WRITE
                    and not any(lo <= s.value < hi for lo, hi in relro)):
                writable.append((s.value, s.size, key))
            elif s.type == STT_TLS and syms.tls_size is not None:
                thread_local.append((s.value - syms.tls_size, s.size, key))
    return objects, writable, thread_local


def shared_lines(placed):
    """Return the pairs of objects in PLACED, a list of (start, size,
    name), that share a cache line."""
    lines = collections.defaultdict(set)
    for start, size, key in placed:
        # Objects of the same name are told apart only by their
        # contents, so leave them unnumbered here.
        for line in range(start // CACHE_LINE,
                          (start + size - 1) // CACHE_LINE + 1):
            lines[line].add(key.split("#", 1)[0])
    pairs = set()
    for keys in lines.values():
        pairs.update(itertools.combinations(sorted(keys), 2))
    return pairs


def read_watch(path):
    patterns = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                patterns.append(line)
    return patterns


def watched(name, patterns):
    base = base_name(name)
    return any(fnmatch.fnmatchcase(base, p) for p in patterns)


def report(title, a, b, show, mark):
    """Print the changed, removed and added entries of the dicts A and B
    (values are instruction lists or data descriptions); return their
    names as (changed, only in A, only in B)."""
    changed = [f for f in a if f in b and a[f] != b[f]]
    only_a = [f for f in a if f not in b]
    only_b = [f for f in b if f not in a]
    same = sum(1 for f in a if f in b and a[f] == b[f])
    print(f"asmdiff: {title}: {len(a)} in A, {len(b)} in B; {same} "
          f"identical, {len(changed)} changed, {len(only_a)} only in A, "
          f"{len(only_b)} only in B")
    kinds = (("changed", changed), ("only in A", only_a),
             ("only in B", only_b))
    for kind, names in kinds:
        if names:
            print(f"  {title} {kind}:")
        for f in names[:show]:
            sizes = "/".join(str(len(x[f])) for x in (a, b) if f in x)
            print(f"    {f} ({sizes}){mark(f)}")
        if len(names) > show:
            print(f"    ... and {len(names) - show} more")
    return changed, only_a, only_b


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Compare the machine code and data of two builds, "
        "function by function.")
    ap.add_argument("a", metavar="A")
    ap.add_argument("b", metavar="B")
    ap.add_argument("--watch", metavar="FILE",
                    help="file of hot function names or patterns")
    ap.add_argument("--fail-on", choices=("any", "watch", "none"),
                    default="any",
                    help="which changes make the exit status 1 "
                    "(default: any)")
    ap.add_argument("--show", type=int, default=50, metavar="N",
                    help="list at most N names per kind")
    ap.add_argument("--diff", action="append", default=[],
                    metavar="NAME",
                    help="print the normalized instructions of function "
                    "NAME (or the description of data object NAME) in A "
                    "and B as a unified diff")
    ap.add_argument("--objdump", default="objdump",
                    help="GNU objdump program (default: objdump)")
    args = ap.parse_args(argv)
    if args.fail_on == "watch" and not args.watch:
        problem = "--fail-on watch needs --watch"
        ap.error(problem)
    try:
        check_objdump(args.objdump)
        patterns = read_watch(args.watch) if args.watch else []
        syms_a, fa = functions(args.a, args.objdump)
        syms_b, fb = functions(args.b, args.objdump)
        da, wa, ta = data_objects(syms_a)
        db, wb, tb = data_objects(syms_b)
    except (OSError, elfcmp.ElfError, ToolError,
            subprocess.CalledProcessError) as e:
        print(f"asmdiff: {e}", file=sys.stderr)
        return 2

    def mark(f):
        return " *" if patterns and watched(f, patterns) else ""

    code = report("functions", fa, fb, args.show, mark)
    data = report("data objects", da, db, args.show, lambda f: "")
    for kind, xa, xb in (("writable", wa, wb), ("thread-local", ta, tb)):
        pairs_a, pairs_b = shared_lines(xa), shared_lines(xb)
        added = sorted(pairs_b - pairs_a)
        removed = sorted(pairs_a - pairs_b)
        if added or removed:
            print(f"asmdiff: cache lines: {len(added)} pairs of {kind} "
                  f"objects now share a line, {len(removed)} no longer "
                  "do; time this change")
            for x, y in added[:5]:
                print(f"    now together: {x}, {y}")
            for x, y in removed[:5]:
                print(f"    now apart: {x}, {y}")
        elif xa or xb:
            print(f"asmdiff: cache lines: {kind} objects share lines as "
                  "before")

    hits = [f for f in code[0] + code[1] + code[2]
            if patterns and watched(f, patterns)]
    if patterns:
        names = list(fa) + list(fb)
        unused = [p for p in patterns
                  if not any(fnmatch.fnmatchcase(base_name(f), p)
                             for f in names)]
        present = {base_name(f) for f in names if watched(f, patterns)}
        print(f"asmdiff: watch list: {len(patterns)} patterns match "
              f"{len(present)} functions; {len(hits)} changed"
              + (" (marked *)" if hits else ""))
        if unused:
            print("asmdiff: watch patterns with no out-of-line function "
                  f"(inlined everywhere): {', '.join(unused)}")

    for name in args.diff:
        pair = next(((x.get(name), y.get(name))
                     for x, y in ((fa, fb), (da, db))
                     if name in x or name in y), None)
        if pair is None:
            print(f"asmdiff: {name}: no function or data object of that "
                  "name in A or B")
            continue
        lines = [[str(v) for v in (side or [])] for side in pair]
        diff = list(difflib.unified_diff(*lines, f"A {name}", f"B {name}",
                                         lineterm=""))
        print("\n".join(diff) if diff else f"asmdiff: {name}: same in "
              "A and B")

    if args.fail_on == "any" and any(code + data):
        return 1
    if args.fail_on == "watch" and hits:
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:          # a bug, not a difference
        traceback.print_exc()
        sys.exit(2)
