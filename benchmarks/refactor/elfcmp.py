#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Check that two ELF files load exactly the same bytes.

usage: elfcmp.py [--strict] [--ignore SECTION]... A B
       elfcmp.py [--programs LIST] BUILD_A BUILD_B

A and B are ELF files: executables, shared objects or object files.
Given two build directories instead, elfcmp compares src/grep,
src/grep-hooks and src/grep-shrunk in each (see --programs).

Every allocated section (SHF_ALLOC) must have the same name, type,
flags, address, size and contents; for SHT_NOBITS sections such as
.bss only the size and address are compared.  The entry point and the
program headers must match too.  In object files, the relocations of
the allocated sections must also match, because the linker applies
them.  Other unallocated sections (symbols, debug information,
.comment) are not loaded, so they are ignored.

.note.gnu.build-id is ignored by default.  The linker computes it over
the whole file, so it changes whenever the debug information does,
for example when code moves to another line, even though the loaded
code is identical.  --strict compares it too.

Exit status: 0 if the files are identical, 1 if they differ, 2 on
error.  The ELF reader in this file is also used by asmdiff.py.
"""

import argparse
import os
import struct
import sys
import traceback

SHF_ALLOC = 0x2
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHT_DYNSYM = 11
SHN_XINDEX = 0xFFFF
PT_TLS = 7
ET_REL = 1
STT_SECTION = 3
STT_TLS = 6
BUILD_ID = ".note.gnu.build-id"
PROGRAMS = "grep,grep-hooks,grep-shrunk"


class ElfError(Exception):
    """A file is not an ELF file that this module can read."""


class Section:
    """One section header, with its contents read on demand."""

    def __init__(self, elf, index, fields):
        (self.name_off, self.type, self.flags, self.addr, self.offset,
         self.size, self.link, self.info, self.align,
         self.entsize) = fields
        self.elf = elf
        self.index = index
        self.name = ""

    @property
    def alloc(self):
        return bool(self.flags & SHF_ALLOC)

    def data(self):
        if self.type == SHT_NOBITS:
            return b""
        end = self.offset + self.size
        if end > len(self.elf.raw):
            raise ElfError(f"{self.elf.path}: section {self.name} is "
                           "truncated")
        return self.elf.raw[self.offset:end]


class Symbol:
    """One symbol table entry."""

    __slots__ = ("name", "value", "size", "type", "shndx")

    def __init__(self, name, value, size, info, shndx):
        self.name = name
        self.value = value
        self.size = size
        self.type = info & 0xF
        self.shndx = shndx


class Elf:
    """A parsed ELF file: header fields, sections, symbols, segments."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.raw = f.read()
        raw = self.raw
        if raw[:4] != b"\x7fELF":
            raise ElfError(f"{path}: not an ELF file")
        if len(raw) < 16:
            raise ElfError(f"{path}: truncated ELF file")
        if raw[4] not in (1, 2) or raw[5] not in (1, 2):
            raise ElfError(f"{path}: unknown ELF class or byte order")
        self.is64 = raw[4] == 2
        self.end = "<" if raw[5] == 1 else ">"
        if self.is64:
            hdr = self._unpack("HHIQQQIHHHHHH", 16)
        else:
            hdr = self._unpack("HHIIIIIHHHHHH", 16)
        (self.type, self.machine, _, self.entry, self.phoff,
         self.shoff, _, _, self.phentsize, self.phnum, self.shentsize,
         shnum, shstrndx) = hdr
        self.sections = self._read_sections(shnum, shstrndx)
        self.segments = self._read_segments()

    def _unpack(self, fmt, offset):
        fmt = self.end + fmt
        if offset + struct.calcsize(fmt) > len(self.raw):
            raise ElfError(f"{self.path}: truncated ELF file")
        return struct.unpack_from(fmt, self.raw, offset)

    def _read_sections(self, shnum, shstrndx):
        if self.shoff == 0:
            raise ElfError(f"{self.path}: no section headers")
        fmt = "IIQQQQIIQQ" if self.is64 else "IIIIIIIIII"
        first = self._unpack(fmt, self.shoff)
        if shnum == 0:
            shnum = first[5]
        if shstrndx == SHN_XINDEX:
            shstrndx = first[6]
        sections = [
            Section(self, i,
                    self._unpack(fmt, self.shoff + i * self.shentsize))
            for i in range(shnum)]
        if shstrndx >= len(sections):
            raise ElfError(f"{self.path}: bad section name table index")
        names = sections[shstrndx].data()
        for s in sections:
            s.name = _cstring(names, s.name_off)
        return sections

    def _read_segments(self):
        """Return the program headers as (type, flags, offset, vaddr,
        filesz, memsz, align) tuples."""
        segments = []
        fmt = "IIQQQQQQ" if self.is64 else "IIIIIIII"
        for i in range(self.phnum):
            f = self._unpack(fmt, self.phoff + i * self.phentsize)
            if self.is64:
                p_type, flags, offset, vaddr, _, filesz, memsz, align = f
            else:
                p_type, offset, vaddr, _, filesz, memsz, flags, align = f
            segments.append((p_type, flags, offset, vaddr, filesz, memsz,
                             align))
        return segments

    def symbol_table(self, table):
        """Return the symbols of section TABLE, indexed as in the file
        (entry 0 is the null symbol)."""
        strtab = self.sections[table.link].data()
        data = table.data()
        if self.is64:
            fmt, size = self.end + "IBBHQQ", 24
        else:
            fmt, size = self.end + "IIIBBH", 16
        result = []
        for off in range(0, len(data) - size + 1, size):
            f = struct.unpack_from(fmt, data, off)
            if self.is64:
                name, info, _, shndx, value, sym_size = f
            else:
                name, value, sym_size, info, _, shndx = f
            result.append(Symbol(_cstring(strtab, name), value, sym_size,
                                 info, shndx))
        return result

    def has_symtab(self):
        return any(s.type == SHT_SYMTAB for s in self.sections)

    def symbols(self):
        """Return the symbols of .symtab, or of .dynsym if it is absent."""
        tables = [s for s in self.sections if s.type == SHT_SYMTAB]
        if not tables:
            tables = [s for s in self.sections if s.type == SHT_DYNSYM]
        result = []
        for table in tables:
            result.extend(self.symbol_table(table)[1:])
        return result

    def relocations(self, allocated_only=False):
        """Return {target section index: [(offset, type, symbol, addend)]}
        for the SHT_RELA and SHT_REL sections; SYMBOL is a Symbol or
        None.  With ALLOCATED_ONLY, only the relocation sections that
        are loaded (the dynamic relocations of linked files)."""
        result = {}
        tables = {}
        for sec in self.sections:
            if sec.type not in (SHT_RELA, SHT_REL):
                continue
            if allocated_only and not sec.alloc:
                continue
            if sec.link not in tables:
                tables[sec.link] = (self.symbol_table(self.sections[sec.link])
                                    if sec.link else [])
            syms = tables[sec.link]
            rela = sec.type == SHT_RELA
            if self.is64:
                fmt, size = self.end + ("QQq" if rela else "QQ"), (
                    24 if rela else 16)
            else:
                fmt, size = self.end + ("IIi" if rela else "II"), (
                    12 if rela else 8)
            data = sec.data()
            entries = result.setdefault(sec.info, [])
            for off in range(0, len(data) - size + 1, size):
                f = struct.unpack_from(fmt, data, off)
                r_offset, info = f[0], f[1]
                addend = f[2] if rela else 0
                if self.is64:
                    sym, rtype = info >> 32, info & 0xFFFFFFFF
                else:
                    sym, rtype = info >> 8, info & 0xFF
                entries.append((r_offset, rtype,
                                syms[sym] if 0 < sym < len(syms) else None,
                                addend))
        return result

    def tls_size(self):
        """Return the size of the TLS block rounded to its alignment,
        or None if the file has no PT_TLS segment."""
        for p_type, _, _, _, _, memsz, align in self.segments:
            if p_type == PT_TLS:
                align = max(align, 1)
                return (memsz + align - 1) // align * align
        return None

    def tls_segment(self):
        """Return (vaddr, filesz) of the PT_TLS segment, or None."""
        for p_type, _, _, vaddr, filesz, _, _ in self.segments:
            if p_type == PT_TLS:
                return vaddr, filesz
        return None


def _cstring(table, offset):
    end = table.find(b"\0", offset)
    if end < 0:
        end = len(table)
    return table[offset:end].decode("utf-8", "replace")


def alloc_sections(elf, ignore):
    """Map (name, occurrence) to each allocated section of ELF."""
    result = {}
    seen = {}
    for s in elf.sections:
        if not s.alloc or s.name in ignore:
            continue
        n = seen.get(s.name, 0)
        seen[s.name] = n + 1
        result[(s.name, n)] = s
    return result


def _label(key):
    name, n = key
    return name if n == 0 else f"{name}#{n + 1}"


def compare(path_a, path_b, ignore):
    """Return (number of compared sections, list of differences)."""
    a, b = Elf(path_a), Elf(path_b)
    diffs = []
    if (a.is64, a.end, a.machine, a.type) != (b.is64, b.end, b.machine,
                                              b.type):
        diffs.append("ELF class, byte order, machine or type")
    if a.entry != b.entry:
        diffs.append(f"entry point ({a.entry:#x} vs {b.entry:#x})")
    sa, sb = alloc_sections(a, ignore), alloc_sections(b, ignore)
    for key in sa:
        if key not in sb:
            diffs.append(f"{_label(key)} (only in A)")
    for key in sb:
        if key not in sa:
            diffs.append(f"{_label(key)} (only in B)")
    for key, x in sa.items():
        y = sb.get(key)
        if y is None:
            continue
        what = []
        if x.type != y.type or x.flags != y.flags:
            what.append("type or flags")
        if x.addr != y.addr:
            what.append(f"address {x.addr:#x} vs {y.addr:#x}")
        if x.size != y.size:
            what.append(f"size {x.size} vs {y.size}")
        else:
            dx, dy = x.data(), y.data()
            if dx != dy:
                n = sum(1 for i, j in zip(dx, dy) if i != j)
                first = next(i for i, (p, q) in enumerate(zip(dx, dy))
                             if p != q)
                what.append(f"{n} of {x.size} bytes differ, the first at "
                            f"offset {first:#x}")
        if what:
            diffs.append(f"{_label(key)} ({'; '.join(what)})")
    if a.segments != b.segments:
        diffs.append("program headers")
    if a.type == ET_REL and b.type == ET_REL:
        diffs.extend(compare_relocations(a, b, ignore))
    return len(sa), diffs


def relocation_entries(elf, ignore):
    """Map (name, occurrence) of each allocated section of the object
    file ELF to its relocations, with symbols given by name."""
    keys = {s.index: key for key, s in alloc_sections(elf, ignore).items()}
    result = {}
    for target, entries in elf.relocations().items():
        if target not in keys:
            continue
        named = []
        for offset, rtype, sym, addend in entries:
            if sym is None:
                name = ""
            elif sym.type == STT_SECTION and sym.shndx < len(elf.sections):
                name = elf.sections[sym.shndx].name
            else:
                name = sym.name
            named.append((offset, rtype, name, addend))
        result[keys[target]] = sorted(named)
    return result


def compare_relocations(a, b, ignore):
    """Return the differences between the relocations of the allocated
    sections of the object files A and B."""
    ra, rb = relocation_entries(a, ignore), relocation_entries(b, ignore)
    diffs = []
    for key in sorted(set(ra) | set(rb)):
        x, y = ra.get(key, []), rb.get(key, [])
        if x != y:
            n = len(set(x) ^ set(y))
            diffs.append(f"relocations of {_label(key)} ({len(x)} vs "
                         f"{len(y)} entries, {n} differ)")
    return diffs


def pairs(args):
    """Yield (label, A path, B path) for the files to compare."""
    if os.path.isdir(args.a) and os.path.isdir(args.b):
        for prog in args.programs.split(","):
            rel = os.path.join("src", prog)
            yield (rel, os.path.join(args.a, rel),
                   os.path.join(args.b, rel))
    elif os.path.isdir(args.a) or os.path.isdir(args.b):
        raise ElfError("A and B must both be files or both be "
                       "directories")
    else:
        yield os.path.basename(args.b), args.a, args.b


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Check that two ELF files load the same bytes.")
    ap.add_argument("a", metavar="A", help="ELF file or build directory")
    ap.add_argument("b", metavar="B", help="ELF file or build directory")
    ap.add_argument("--ignore", action="append", default=[],
                    metavar="SECTION",
                    help="also ignore this allocated section")
    ap.add_argument("--strict", action="store_true",
                    help=f"compare {BUILD_ID} too")
    ap.add_argument("--programs", default=PROGRAMS,
                    help="programs under src/ to compare when A and B "
                    f"are build directories (default: {PROGRAMS})")
    args = ap.parse_args(argv)
    ignore = set(args.ignore)
    if not args.strict:
        ignore.add(BUILD_ID)
    status = 0
    try:
        for label, path_a, path_b in pairs(args):
            count, diffs = compare(path_a, path_b, ignore)
            if diffs:
                status = 1
                print(f"elfcmp: {label}: DIFFERENT")
                for d in diffs:
                    print(f"  {d}")
            else:
                note = ""
                if ignore:
                    note = f" ({', '.join(sorted(ignore))} ignored)"
                print(f"elfcmp: {label}: identical, {count} allocated "
                      f"sections{note}")
    except (OSError, ElfError) as e:
        print(f"elfcmp: {e}", file=sys.stderr)
        return 2
    return status


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:          # a bug, not a difference
        traceback.print_exc()
        sys.exit(2)
