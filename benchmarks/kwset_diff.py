#!/usr/bin/env python3
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

Example:
  benchmarks/kwset_diff.py --baseline benchmarks/grep-baseline \\
      --candidate src/grep --corpus /path/to/source/tree --cases 3000
"""

import argparse
import concurrent.futures
import os
import random
import shutil
import subprocess
import sys
import tempfile

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


def run(binary, args, env, threads, merge):
    e = dict(os.environ)
    e.pop('GREP_COLOR', None)
    e.pop('GREP_COLORS', None)
    e.update(env)
    if threads is not None:
        e['GREP_THREADS'] = str(threads)
    # With MERGE, stderr goes to the same pipe as stdout, which also
    # checks how diagnostics interleave with output.
    p = subprocess.run(['grep'] + args, executable=binary,
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--baseline', required=True)
    ap.add_argument('--candidate', required=True)
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
    opts = ap.parse_args()

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
    with concurrent.futures.ThreadPoolExecutor(opts.jobs) as ex:
        for i, (case, want, failures, shared) in enumerate(
                ex.map(one, range(opts.cases))):
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
    print(f'{opts.cases} cases ({stats}), {nfail} failures, '
          f'{npre} pre-existing failures shared with the control build')
    if not opts.workdir and nfail == 0:
        shutil.rmtree(workdir, ignore_errors=True)
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
