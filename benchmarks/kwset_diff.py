#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Randomized differential test of the keyword-set (-F) search paths.

Runs thousands of randomized cases through a baseline grep (normally
the stock upstream build, benchmarks/grep-baseline) and a candidate
build, and requires byte-identical stdout, stderr and exit status.

The cases cover keyword lists of 1 to 10,000 entries with minimum
lengths of 1 to 20 bytes, overlapping and prefix/suffix-sharing
keywords, duplicates, the empty keyword, keywords drawn from every
byte value, random text and (optionally) slices of a real source
tree, the options -F, -F -i, -F -w, -F -x, -F -o, -F -c, -F -v, -z,
-E with literal alternations (the must-literal path) and -P with
literals, in the C, ISO-8859-15 and UTF-8 locales, at several
GREP_THREADS values, on small files, files large enough to be split
into chunks, and directory trees.

After the random cases a deterministic sweep runs (--sweep runs only
the sweep, --no-sweep skips it).  Read buffers are page aligned and
chunk maps 2 MiB aligned, so the file offset of a match fixes its
alignment within the vector loads, and one file with a match at every
offset modulo 64 covers all alignments in one invocation:

  * single keywords of 16 lengths from 1 to 64 bytes (every length
    with --full), one per line at offsets 0..63, plus a keyword that
    straddles a page boundary, a line that is just the keyword, and a
    keyword ending at the end of the file with and without a trailing
    newline; with -F, -F -o -b, -F -c and, by turns (all with --full),
    -E -o -b, -F -i -o -b, -F -w -o -b and -F -x -n;
  * Teddy: 2, 8, 16 and 32 keywords with minimum lengths 1, 2, 3, 4,
    8 and 16, sharing prefixes and suffixes, at offsets 0..63, -o -b;
  * the dense Aho-Corasick DFA: 33, 64 and 100 keywords, and 40
    identical keywords (ripgrep issue 1334);
  * self-overlapping and repetitive haystacks (ripgrep issue 1319,
    "153.230000", runs of '0', "abab...") at the 32 offsets of an AVX2
    vector, with -E, -P and -F.

Every sweep case runs on the baseline and on the candidate at each
--threads value, and through a pipe at one of them (all with --full),
and must match the baseline exactly.  Where the output can be computed
independently (every case but -w, and the haystacks, whose output is
hard-coded), it must also match that, which keeps the sweep useful
when the baseline is the candidate itself.  The default sweep is about
100 configurations and 400 runs.

Example:
  benchmarks/kwset_diff.py --baseline benchmarks/grep-baseline \\
      --candidate src/grep --corpus /path/to/source/tree --cases 3000
"""

import argparse
import concurrent.futures
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

SET_SIZES = [1, 2, 8, 32, 64, 65, 100, 1000, 10000]
SET_WEIGHTS = [8, 8, 8, 8, 8, 8, 6, 4, 1]

OPTION_SETS = [
    ['-F'], ['-F', '-i'], ['-F', '-w'], ['-F', '-x'], ['-F', '-o'],
    ['-F', '-c'], ['-F', '-v'], ['-F', '-o', '-i'], ['-F', '-w', '-o'],
    ['-F', '-w', '-i'], ['-F', '-x', '-i'], ['-F', '-n', '-b', '-o'],
    ['-F', '-z'], ['-F', '-z', '-x'], ['-F', '-z', '-o'], ['-F', '-l'],
    ['-F', '--color=always'], ['-F', '-i', '--color=always'],
    ['-F', '-c', '-v', '-i'], ['-F', '-m', '3', '-n'],
]

LOCALES = ['C', 'en_US.ISO-8859-15', 'en_US.UTF-8']

# ISO-8859-15 case pairs that are not 0x20 apart, and other Latin-9 letters.
LATIN9_EXTRA = bytes([0xA6, 0xA8, 0xB4, 0xB8, 0xBC, 0xBD, 0xBE, 0xFF,
                      0xC0, 0xC9, 0xDE, 0xE0, 0xE9, 0xFE, 0xF7, 0xD7])
IDENT = b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_'
REGEX_META_E = set(b'\\.[]()*+?{}|^$')


def available_locales():
    try:
        out = subprocess.run(['locale', '-a'], capture_output=True,
                             text=True).stdout.split()
    except OSError:
        return ['C']
    norm = {x.lower().replace('-', ''): x for x in out}
    result = []
    for loc in LOCALES:
        key = loc.lower().replace('-', '')
        if loc == 'C' or key in norm:
            result.append(loc)
    # A locale whose case folding is not ASCII-compatible exercises
    # the paths that cannot fold bytes cheaply.
    if 'tr_tr.iso88599' in norm or 'tr_tr' in norm:
        result.append(norm.get('tr_tr.iso88599', norm.get('tr_tr')))
    return result


class Corpus:
    def __init__(self, root, rng, max_files=20000):
        self.files = []
        if not root:
            return
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d != '.git']
            for f in filenames:
                p = os.path.join(dirpath, f)
                if os.path.isfile(p) and not os.path.islink(p):
                    self.files.append(p)
        rng.shuffle(self.files)
        self.files = self.files[:max_files]

    def slice(self, rng, size):
        """Return about SIZE bytes made of slices of random files."""
        out = bytearray()
        while len(out) < size and self.files:
            p = rng.choice(self.files)
            try:
                with open(p, 'rb') as f:
                    data = f.read()
            except OSError:
                continue
            if not data:
                continue
            a = rng.randrange(len(data))
            b = min(len(data), a + rng.randint(64, max(64, size - len(out))))
            out += data[a:b]
            if rng.random() < 0.5:
                out += b'\n'
        return bytes(out[:size])


def random_bytes(rng, n, alphabet):
    return bytes(rng.choices(alphabet, k=n))


def case_variant(rng, s, locale):
    """Flip the case of some letters of S."""
    out = bytearray(s)
    for i, c in enumerate(out):
        if rng.random() < 0.5:
            continue
        if 0x41 <= c <= 0x5A or 0x61 <= c <= 0x7A:
            out[i] = c ^ 0x20
        elif 'ISO-8859-15' in locale:
            pairs = {0xA6: 0xA8, 0xA8: 0xA6, 0xB4: 0xB8, 0xB8: 0xB4,
                     0xBC: 0xBD, 0xBD: 0xBC, 0xBE: 0xFF, 0xFF: 0xBE}
            if c in pairs:
                out[i] = pairs[c]
            elif 0xC0 <= c <= 0xDE and c != 0xD7:
                out[i] = c + 0x20
            elif 0xE0 <= c <= 0xFE and c != 0xF7:
                out[i] = c - 0x20
    return bytes(out)


def make_text(rng, corpus, kind, size, alphabet, locale):
    if kind == 'corpus' and corpus.files:
        text = corpus.slice(rng, size)
    else:
        lines = []
        total = 0
        while total < size:
            n = rng.randint(0, 120)
            line = random_bytes(rng, n, alphabet)
            lines.append(line)
            total += n + 1
        text = b'\n'.join(lines)[:size]
    if 'ISO-8859-15' in locale and rng.random() < 0.5:
        b = bytearray(text)
        for _ in range(max(1, len(b) // 50)):
            if b:
                b[rng.randrange(len(b))] = rng.choice(LATIN9_EXTRA)
        text = bytes(b)
    return text


def make_patterns(rng, text, count, minlen, maxlen, alphabet, source, locale):
    pats = []
    for i in range(count):
        n = rng.randint(minlen, maxlen)
        r = rng.random()
        if pats and r < 0.15:
            # Share a prefix or a suffix with an earlier keyword, or
            # duplicate it.
            o = rng.choice(pats)
            k = rng.randrange(4)
            if k == 0:
                p = o
            elif k == 1:
                p = o[:max(1, len(o) - rng.randint(1, 3))]
            elif k == 2:
                p = o[rng.randint(1, 3):] or o
            else:
                p = o + random_bytes(rng, rng.randint(1, 3), alphabet)
        elif source == 'text' and len(text) > n and r < 0.85:
            for _ in range(8):
                a = rng.randrange(len(text) - n + 1)
                p = text[a:a + n]
                if b'\n' not in p:
                    break
            else:
                p = random_bytes(rng, n, alphabet)
        elif source == 'allbytes':
            p = rng.randbytes(n)
        else:
            p = random_bytes(rng, n, alphabet)
        p = p.replace(b'\n', b'')
        if rng.random() < 0.3:
            p = case_variant(rng, p, locale)
        if len(p) >= max(1, minlen) or (minlen <= 1 and p):
            pats.append(p)
        elif p:
            pats.append(p)
    return pats


def valid_utf8(p):
    try:
        p.decode('utf-8')
        return True
    except UnicodeDecodeError:
        return False


def escape_ere(p):
    out = bytearray()
    for c in p:
        if c in REGEX_META_E:
            out += b'\\'
        out.append(c)
    return bytes(out)


class Case:
    def __init__(self, idx, workdir):
        self.idx = idx
        self.dir = os.path.join(workdir, f'case{idx:05d}')
        os.makedirs(self.dir, exist_ok=True)
        self.args = []
        self.env = {}
        self.merge = False
        self.desc = ''


def build_case(idx, seed, workdir, corpus, locales, sizes_only=None):
    rng = random.Random(seed * 1000003 + idx)
    case = Case(idx, workdir)
    locale = rng.choice(locales)
    mode = rng.random()
    alphabet_kind = rng.random()
    if alphabet_kind < 0.25:
        alphabet = b'ab'[:rng.randint(1, 2)] + b'c' * rng.randint(0, 1)
    elif alphabet_kind < 0.5:
        alphabet = IDENT
    elif alphabet_kind < 0.65:
        alphabet = bytes(range(256))
    elif alphabet_kind < 0.8:
        alphabet = b'abAB_\t ' + LATIN9_EXTRA
    else:
        alphabet = IDENT + b' \t(){};,.*-+=<>/"'
    size_choice = rng.random()
    if size_choice < 0.6:
        tsize = rng.randint(0, 20000)
    elif size_choice < 0.9:
        tsize = rng.randint(20000, 400000)
    else:
        tsize = rng.randint(2_200_000, 3_000_000)  # chunked by workers
    kind = 'corpus' if corpus.files and rng.random() < 0.5 else 'random'
    text = make_text(rng, corpus, kind, tsize, alphabet, locale)

    count = sizes_only or rng.choices(SET_SIZES, SET_WEIGHTS)[0]
    minlen = rng.randint(1, 20)
    maxlen = minlen + rng.choice([0, 1, 3, 10, 30])
    src_r = rng.random()
    source = 'text' if src_r < 0.6 else ('allbytes' if src_r < 0.75 else 'alpha')
    pats = make_patterns(rng, text, count, minlen, maxlen, alphabet,
                         source, locale)
    if not pats:
        pats = [b'a']
    if rng.random() < 0.04:
        pats.insert(rng.randrange(len(pats) + 1), b'')

    if mode < 0.08:
        # -E with a literal alternation: the DFA must-literal path.
        alts = [escape_ere(p.replace(b'\x00', b''))
                for p in pats[:rng.randint(1, 4)] if p.replace(b'\x00', b'')]
        regex = b'|'.join(alts) or b'a'
        if rng.random() < 0.3:
            regex = b'(' + regex + b')[a-z]*'
        opts = ['-E'] + rng.choice([[], ['-i'], ['-o'], ['-c'], ['-w']])
        patarg = ['-e', regex]
    elif mode < 0.14:
        # -P with a literal, which uses the literal pre-filter.
        lit = pats[0] if pats[0] else b'ab'
        lit = lit.replace(b'\\', b'')
        lit = bytes(c for c in lit if c not in b'\x00') or b'ab'
        regex = b'\\Q' + lit + b'\\E' if rng.random() < 0.3 else escape_ere(lit)
        if rng.random() < 0.3:
            regex += b'\\w*'
        opts = ['-P'] + rng.choice([[], ['-o'], ['-c'], ['-i']])
        patarg = ['-e', regex]
    else:
        opts = list(rng.choice(OPTION_SETS))
        if ('UTF-8' in locale and len(pats) > 64 and not sizes_only
            and ('-i' in opts or not all(valid_utf8(p) for p in pats))):
            # grep -F gives up on such lists in UTF-8 locales and uses the
            # regex matchers, which are very slow for long alternations
            # and do not use keyword sets, so keep these lists short.
            pats = pats[:64]
        pf = os.path.join(case.dir, 'pats')
        with open(pf, 'wb') as f:
            f.write(b'\n'.join(pats) + b'\n')
        patarg = ['-f', pf]

    tree = rng.random() < 0.12
    if tree:
        root = os.path.join(case.dir, 'tree')
        nfiles = rng.randint(2, 40)
        for k in range(nfiles):
            sub = os.path.join(root, f'd{k % 5}')
            os.makedirs(sub, exist_ok=True)
            a = rng.randrange(len(text) + 1)
            b = min(len(text), a + rng.randint(0, 60000))
            with open(os.path.join(sub, f'f{k:03d}'), 'wb') as f:
                f.write(text[a:b])
        target = ['-r', root]
    else:
        inp = os.path.join(case.dir, 'input')
        with open(inp, 'wb') as f:
            f.write(text)
        target = [inp]
    case.args = opts + patarg + target
    case.env = {'LC_ALL': locale}
    case.merge = rng.random() < 0.1
    case.desc = (f'locale={locale} n={len(pats)} minlen={minlen} '
                 f'src={source} text={kind}/{tsize} tree={tree} merge={case.merge}')
    return case


def run(binary, args, env, threads, merge, stdin_path=None):
    e = dict(os.environ)
    e.pop('GREP_COLOR', None)
    e.pop('GREP_COLORS', None)
    e.update(env)
    if threads is not None:
        e['GREP_THREADS'] = str(threads)
    # With MERGE, stderr goes to the same pipe as stdout, which also
    # checks how diagnostics interleave with output.  With STDIN_PATH,
    # the input is fed through a pipe rather than given as a file.
    stdin = None
    if stdin_path is not None:
        with open(stdin_path, 'rb') as f:
            stdin = f.read()
    p = subprocess.run(['grep'] + args, executable=binary,
                       input=stdin,
                       stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT if merge else subprocess.PIPE,
                       env=e, timeout=600)
    return p.returncode, p.stdout, p.stderr


def check_case(case, baseline, candidate, threads):
    base_threads = (
        1 if os.path.realpath(baseline) == os.path.realpath(candidate)
        else None
    )
    want = run(baseline, case.args, case.env, base_threads, case.merge)
    failures = []
    for t in threads:
        got = run(candidate, case.args, case.env, t, case.merge)
        if got != want:
            what = [n for n, a, b in zip(('status', 'stdout', 'stderr'),
                                         want, got) if a != b]
            failures.append((t, what))
    return want, failures


# Deterministic sweep.  Read buffers are page aligned and chunk maps
# are 2 MiB aligned, so the file offset of a match fixes its alignment
# within the vector loads; one file holding a match at every offset
# modulo 64 therefore covers every alignment in one invocation.

SWEEP_LENGTHS = [1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64]
SWEEP_CORE_OPTS = [['-F'], ['-F', '-o', '-b'], ['-F', '-c']]
SWEEP_EXTRA_OPTS = [['-E', '-o', '-b'], ['-F', '-i', '-o', '-b'],
                    ['-F', '-w', '-o', '-b'], ['-F', '-x', '-n']]
SWEEP_TEDDY_SIZES = [2, 8, 16, 32]
SWEEP_TEDDY_MINLENS = [1, 2, 3, 4, 8, 16]
SWEEP_AC_SIZES = [33, 64, 100]
SWEEP_SEPS = b' .-_,;:'
LIT_ALPHABET = b'abcdefghijklmnopqrstuvwxyz0123456789'

# Self-overlapping and repetitive haystacks with output that does not
# depend on the build: (name, options, haystack, expected match).  Each
# haystack is placed at the 32 offsets of an AVX2 vector, one per line,
# so -o prints the expected match 32 times.  The expectations were
# checked against stock GNU grep 3.12.
SWEEP_TRAPS = [
    ('ere-overlap', ['-o', '-E', '[0-9]{3}00'], b'153.230000', b'23000'),
    ('pcre-overlap', ['-o', '-P', r'\d\d\d000'], b'153.230000', b'230000'),
    # ripgrep issue 1319.
    ('ere-dna', ['-o', '-E', 'TTGAGTCCAGGAG[ATCG]{2}C'],
     b'CCAGCTACTCGGGAGGCTGAGGCTGGAGGATCGCTTGAGTCCAGGAGTTC',
     b'TTGAGTCCAGGAGTTC'),
    ('zeros', ['-F', '-o', '-e', '00001'], b'0' * 69 + b'1', b'00001'),
    ('abab', ['-F', '-o', '-e', 'abababac'], b'ab' * 40 + b'ac', b'abababac'),
    # Two keywords: the leftmost match is the longer one.
    ('aaab', ['-F', '-o', '-e', 'aaab', '-e', 'aaaab'], b'a' * 60 + b'b',
     b'aaaab'),
]


class SweepCase:
    def __init__(self, name, args, path, env, expect=None):
        self.name = name
        self.args = args        # options and patterns, without the input
        self.path = path        # the single input file
        self.env = env
        self.expect = expect    # (status, stdout, stderr), or None


def cyc(unit, start, n):
    """Return N bytes of UNIT repeated, starting at offset START."""
    if n <= 0:
        return b''
    reps = (start % len(unit) + n) // len(unit) + 1
    s = start % len(unit)
    return (unit * reps)[s:s + n]


def width_for(longest):
    """Line width, a multiple of 64, that fits 63 filler bytes, a match
    of LONGEST bytes and a newline."""
    return 64 * ((63 + longest + 1 + 63) // 64)


def sweep_literal(n):
    rng = random.Random(0x5EED + n)
    lit = bytes([rng.choice(LIT_ALPHABET[:26])])
    lit += bytes(rng.choices(LIT_ALPHABET, k=n - 1))
    return lit


def mixed_case(s):
    return bytes(c ^ 0x20 if 0x61 <= c <= 0x7A and i % 2 == 0 else c
                 for i, c in enumerate(s))


def oracle(data, pats, flags):
    """Return the expected (status, stdout, stderr) of grep -F with the
    keyword list PATS and the one-letter options in FLAGS (any of
    'b', 'c', 'i', 'n', 'o', 'x'; not -w) on DATA, in a locale where DATA
    is ASCII.  Matches are leftmost-longest and do not overlap."""
    alts = sorted(set(pats), key=len, reverse=True)
    rx = re.compile(b'|'.join(re.escape(p) for p in alts),
                    re.IGNORECASE if 'i' in flags else 0)
    lines = data.split(b'\n')
    if data.endswith(b'\n'):
        lines.pop()
    out = []
    count = 0
    off = 0
    for no, line in enumerate(lines, 1):
        if 'x' in flags:
            found = [(0, line)] if rx.fullmatch(line) else []
        else:
            found = [(m.start(), m.group()) for m in rx.finditer(line)]
        if found:
            count += 1
            prefix = f'{no}:'.encode() if 'n' in flags else b''
            if 'o' in flags:
                for s, m in found:
                    b = f'{off + s}:'.encode() if 'b' in flags else b''
                    out.append(prefix + b + m + b'\n')
            elif 'c' not in flags:
                b = f'{off}:'.encode() if 'b' in flags else b''
                out.append(prefix + b + line + b'\n')
        off += len(line) + 1
    if 'c' in flags:
        out = [f'{count}\n'.encode()]
    return (0 if count else 1, b''.join(out), b'')


def oracle_flags(opts):
    """Return the oracle flags for OPTS, or None if there is no oracle."""
    flags = ''
    for o in opts:
        if o in ('-F', '-E'):
            continue
        if o not in ('-b', '-c', '-i', '-n', '-o', '-x'):
            return None
        flags += o[1]
    return flags


def write(path, data):
    with open(path, 'wb') as f:
        f.write(data)
    return path


def alignment_file(lit, nl):
    """One line per offset 0..63 of LIT, a line where LIT straddles a
    page boundary, a line that is exactly LIT, and a last line that ends
    in LIT at the end of the file, followed by a newline if NL."""
    n = len(lit)
    w = width_for(n)
    unit = lit[:max(1, n - 1)] + SWEEP_SEPS[n % len(SWEEP_SEPS):][:1]
    out = bytearray()
    for k in range(64):
        # Every fourth line has the case-swapped literal, for -i.
        word = lit.swapcase() if k % 4 == 3 else lit
        out += cyc(unit, 7 * k, k) + word + cyc(unit, 3 * k, w - k - n - 1)
        out += b'\n'
    # Make the next literal straddle the next page (and so 64-byte)
    # boundary.
    page = (len(out) // 4096 + 1) * 4096
    start = page - n // 2
    out += cyc(unit, 5, start - len(out)) + lit + cyc(unit, 11, 9) + b'\n'
    out += lit + b'\n'
    out += (cyc(unit, 1, n % 13) if n % 2 else b'') + lit
    if nl:
        out += b'\n'
    return bytes(out)


def multi_patterns(count, minlen, seed, alphabet):
    """COUNT keywords of at least MINLEN bytes, the first of length
    MINLEN, many sharing prefixes or suffixes with each other."""
    rng = random.Random(seed)
    pats = [bytes(rng.choices(alphabet, k=minlen))]
    while len(pats) < count:
        n = minlen + rng.randint(0, max(2, minlen // 2))
        r = rng.random()
        o = rng.choice(pats)
        if r < 0.3:
            # Share a prefix.
            k = min(len(o), max(1, n - 1))
            p = o[:k] + bytes(rng.choices(alphabet, k=n - k))
        elif r < 0.6:
            # Share a suffix.
            k = min(len(o), max(1, n - 1))
            p = bytes(rng.choices(alphabet, k=n - k)) + o[-k:]
        else:
            p = bytes(rng.choices(alphabet, k=n))
        if len(p) < minlen:
            p += bytes(rng.choices(alphabet, k=minlen - len(p)))
        if minlen > 1 and len(p) == minlen:
            p += alphabet[:1]
        pats.append(p)
    return pats


def multi_file(pats, nlines):
    """NLINES lines, line K holding keyword K at offset K modulo 64,
    every fifth line followed directly by another keyword."""
    longest = max(len(p) for p in pats)
    w = width_for(2 * longest)
    out = bytearray()
    for k in range(nlines):
        p = pats[k % len(pats)]
        q = pats[(7 * k + 3) % len(pats)]
        unit = (p[:-1] or p) + b' ' + (q[1:] or q) + b'.'
        word = p + (q if k % 5 == 0 else b'')
        a = k % 64
        out += cyc(unit, k, a) + word + cyc(unit, 2 * k, w - a - len(word) - 1)
        out += b'\n'
    return bytes(out)


def build_sweep(workdir, full, locales):
    """Return the list of SweepCase, and the number of skipped cases."""
    root = os.path.join(workdir, 'sweep')
    os.makedirs(root, exist_ok=True)
    locs = ['C'] + [x for x in locales if 'utf' in x.lower()][:1]
    cases = []

    def add(name, args, path, flags_from=None, pats=None, expect=None):
        env = {'LC_ALL': locs[len(cases) % len(locs)]}
        if expect is None and pats is not None:
            flags = oracle_flags(flags_from)
            if flags is not None:
                with open(path, 'rb') as f:
                    expect = oracle(f.read(), pats, flags)
        cases.append(SweepCase(name, args, path, env, expect))

    # 1. Alignment x length, single keyword.
    lengths = range(1, 65) if full else SWEEP_LENGTHS
    for li, n in enumerate(lengths):
        lit = sweep_literal(n)
        files = [write(os.path.join(root, f'align-L{n:02d}-nl{nl}'),
                       alignment_file(lit, nl)) for nl in (0, 1)]
        optsets = SWEEP_CORE_OPTS + (
            SWEEP_EXTRA_OPTS if full
            else [SWEEP_EXTRA_OPTS[li % len(SWEEP_EXTRA_OPTS)]])
        for oi, o in enumerate(optsets):
            pat = mixed_case(lit) if '-i' in o else lit
            path = files[(n + oi) % 2]
            add(f'align L={n} {" ".join(o)} {os.path.basename(path)}',
                o + ['-e', pat.decode()], path, o, [pat])

    # 2. Teddy: 2..32 keywords.
    for count in SWEEP_TEDDY_SIZES:
        for minlen in SWEEP_TEDDY_MINLENS:
            pats = multi_patterns(count, minlen, count * 100 + minlen,
                                  b'abcdefgh0123')
            tag = f'teddy-n{count:02d}-m{minlen:02d}'
            pf = write(os.path.join(root, tag + '.pats'),
                       b'\n'.join(pats) + b'\n')
            path = write(os.path.join(root, tag), multi_file(pats, 64))
            o = ['-F', '-o', '-b']
            add(f'teddy n={count} minlen={minlen}', o + ['-f', pf], path,
                o, pats)

    # 3. Dense Aho-Corasick DFA: more than 32 keywords.
    for count in SWEEP_AC_SIZES:
        pats = multi_patterns(count, 3, 7000 + count,
                              b'abcdefghijklmnop')
        tag = f'ac-n{count:03d}'
        pf = write(os.path.join(root, tag + '.pats'), b'\n'.join(pats) + b'\n')
        path = write(os.path.join(root, tag),
                     multi_file(pats, max(64, count)))
        o = ['-F', '-o', '-b']
        add(f'ac n={count}', o + ['-f', pf], path, o, pats)
    # ripgrep issue 1334: 40 identical keywords.
    dup = b'1.208.0.0/12'
    pf = write(os.path.join(root, 'ac-dup.pats'), (dup + b'\n') * 40)
    lines = []
    for k in range(64):
        near = [b'1.208.0.0/1 ', b'1.208.0.0/13', b'1.208.0.1/12'][k % 3]
        word = dup if k % 2 == 0 else near
        lines.append(b'x' * k + word + b' 10.0.0.0/8' + b'x' * (64 - k))
    path = write(os.path.join(root, 'ac-dup'), b'\n'.join(lines) + b'\n')
    add('ac duplicate keywords', ['-F', '-n', '-f', pf], path,
        ['-F', '-n'], [dup])

    # 4. Self-overlapping traps with fixed expected output.
    for name, o, hay, match in SWEEP_TRAPS:
        w = width_for(len(hay)) - 32
        lines = [b'x' * k + hay + b'x' * (w - k - len(hay) - 1)
                 for k in range(32)]
        path = write(os.path.join(root, 'trap-' + name),
                     b'\n'.join(lines) + b'\n')
        add(f'trap {name}', o, path, expect=(0, (match + b'\n') * 32, b''))
    # The traps run in the C locale.
    for c in cases[-len(SWEEP_TRAPS):]:
        c.env = {'LC_ALL': 'C'}
    return cases


def check_sweep(case, baseline, candidate, threads, stdin_threads):
    """Run CASE through the baseline, then the candidate at each of
    THREADS on the file and at STDIN_THREADS on a pipe.  Return the
    number of runs and a list of failures."""
    same = os.path.realpath(baseline) == os.path.realpath(candidate)
    want = run(baseline, case.args + [case.path], case.env,
               1 if same else None, False)
    runs = 1
    failures = []
    if case.expect is not None and want != case.expect:
        what = [n for n, a, b in zip(('status', 'stdout', 'stderr'),
                                     case.expect, want) if a != b]
        failures.append(('baseline vs expected', what))
    for t in threads:
        got = run(candidate, case.args + [case.path], case.env, t, False)
        runs += 1
        if got != want:
            what = [n for n, a, b in zip(('status', 'stdout', 'stderr'),
                                         want, got) if a != b]
            failures.append((t, what))
    for t in stdin_threads:
        got = run(candidate, case.args, case.env, t, False,
                  stdin_path=case.path)
        runs += 1
        if got != want:
            what = [n for n, a, b in zip(('status', 'stdout', 'stderr'),
                                         want, got) if a != b]
            failures.append((f'stdin/{t}', what))
    return runs, failures


def run_sweep(baseline, candidate, threads, workdir, full, locales, jobs):
    """Run the sweep; return the number of failures."""
    start = time.monotonic()
    cases = build_sweep(workdir, full, locales)

    def one(item):
        i, case = item
        # Feed each case through a pipe at one of the thread counts,
        # in turn, or at all of them with --full.
        st = threads if full else [threads[i % len(threads)]]
        return case, check_sweep(case, baseline, candidate, threads, st)

    nfail = nruns = 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for case, (runs, failures) in ex.map(one, enumerate(cases)):
            nruns += runs
            if failures:
                nfail += 1
                with open(case.path + '.cmd', 'w') as f:
                    f.write(f'{case.env} grep {case.args!r} {case.path}\n'
                            f'{case.name}\nFAIL {failures}\n')
                print(f'FAIL sweep {case.name}: {case.env} '
                      f'args={case.args!r} input={case.path} {failures}',
                      flush=True)
    nexp = sum(c.expect is not None for c in cases)
    print(f'sweep{" (full)" if full else ""}: {len(cases)} configurations '
          f'({nexp} with an expected output), {nruns} runs, '
          f'{nfail} failures, {time.monotonic() - start:.1f}s', flush=True)
    return nfail


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--baseline', default='benchmarks/grep-baseline')
    ap.add_argument('--candidate', default='src/grep')
    ap.add_argument('--corpus', help='directory tree to take real text from')
    ap.add_argument('--cases', type=int, default=2000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--threads', default='1,16',
                    help='comma-separated GREP_THREADS values for the candidate')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--workdir', help='scratch directory (default: a temporary one)')
    ap.add_argument('--keep', action='store_true', help='keep passing cases')
    ap.add_argument('--set-size', type=int, help='use only this keyword-list size')
    ap.add_argument('--control',
                    help='another build to rerun failing cases with; failures '
                    'that it shares are reported as pre-existing')
    ap.add_argument('--sweep', action='store_true',
                    help='run only the deterministic sweep, no random cases')
    ap.add_argument('--full', action='store_true',
                    help='run the full sweep: every keyword length 1..64, '
                    'every option set, pipes at every thread count')
    ap.add_argument('--no-sweep', action='store_true',
                    help='run only the random cases')
    opts = ap.parse_args()
    if opts.baseline == 'benchmarks/grep-baseline' and not os.path.exists(opts.baseline):
        opts.baseline = opts.candidate

    baseline = os.path.abspath(opts.baseline)
    candidate = os.path.abspath(opts.candidate)
    threads = [int(t) for t in opts.threads.split(',')]
    workdir = opts.workdir or tempfile.mkdtemp(prefix='kwset-diff-')
    os.makedirs(workdir, exist_ok=True)
    rng = random.Random(opts.seed)
    corpus = Corpus(opts.corpus, rng)
    locales = available_locales()
    print(f'locales: {locales}; corpus files: {len(corpus.files)}; '
          f'workdir: {workdir}', flush=True)

    def one(idx):
        case = build_case(idx, opts.seed, workdir, corpus, locales,
                          opts.set_size)
        want, failures = check_case(case, baseline, candidate, threads)
        shared = False
        if failures and opts.control:
            control = os.path.abspath(opts.control)
            shared = check_case(case, baseline, control, threads)[1] == failures
        if not failures and not opts.keep:
            shutil.rmtree(case.dir, ignore_errors=True)
        return case, want, failures, shared

    nfail = npre = 0
    stats = {'match': 0, 'nomatch': 0, 'trouble': 0}
    ncases = 0 if opts.sweep else opts.cases
    with concurrent.futures.ThreadPoolExecutor(opts.jobs) as ex:
        for i, (case, want, failures, shared) in enumerate(
                ex.map(one, range(ncases))):
            stats['match' if want[0] == 0 else 'nomatch' if want[0] == 1
                  else 'trouble'] += 1
            if failures:
                kind = 'PRE-EXISTING' if shared else 'FAIL'
                if shared:
                    npre += 1
                else:
                    nfail += 1
                with open(os.path.join(case.dir, 'cmd'), 'w') as f:
                    f.write(f'{case.env} grep {case.args!r}\n{case.desc}\n'
                            f'{kind} {failures}\n')
                print(f'{kind} case {case.idx}: {case.desc} '
                      f'args={case.args!r} {failures}', flush=True)
            if (i + 1) % 250 == 0:
                print(f'{i + 1} cases, {nfail} failures, {npre} pre-existing',
                      flush=True)
    print(f'{ncases} cases ({stats}), {nfail} failures, '
          f'{npre} pre-existing failures shared with the control build')
    if not opts.no_sweep:
        nfail += run_sweep(baseline, candidate, threads, workdir, opts.full,
                           locales, opts.jobs)
    if not opts.workdir and nfail == 0:
        shutil.rmtree(workdir, ignore_errors=True)
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
