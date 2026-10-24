#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# Copying and distribution of this file, with or without modification,
# are permitted in any medium without royalty provided the copyright
# notice and this notice are preserved.

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile


def normalize_stderr(raw: bytes, bin_path: str) -> bytes:
    text = raw.decode("latin1")
    base = os.path.basename(bin_path)
    lines = []
    for line in text.splitlines(keepends=True):
        if line.startswith(bin_path + ":"):
            line = "grep:" + line[len(bin_path) + 1 :]
        elif line.startswith(base + ":"):
            line = "grep:" + line[len(base) + 1 :]
        lines.append(line)
    return "".join(lines).encode("latin1")


def run_cmd(
    bin_path: str,
    args: list[str],
    *,
    env_extra: dict[str, str] | None = None,
    stdin_bytes: bytes | None = None,
    stdout_path: str | None = None,
    cwd: str | None = None,
    timeout: float = 3.0,
) -> tuple[bytes, bytes, int]:
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    out_fh = open(stdout_path, "wb") if stdout_path else subprocess.PIPE
    try:
        proc = subprocess.run(
            [bin_path] + args,
            input=stdin_bytes,
            stdout=out_fh,
            stderr=subprocess.PIPE,
            env=env,
            cwd=cwd,
            timeout=timeout,
        )
    finally:
        if stdout_path and out_fh is not subprocess.PIPE:
            out_fh.close()
    out_bytes = b"" if stdout_path else proc.stdout
    return out_bytes, normalize_stderr(proc.stderr, bin_path), proc.returncode


def assert_equiv(
    label: str,
    ref_bin: str,
    cand_bins: list[tuple[str, dict[str, str]]],
    args: list[str],
    *,
    ref_args: list[str] | None = None,
    ref_env: dict[str, str] | None = None,
    stdin_bytes: bytes | None = None,
    stdout_path: str | None = None,
    cwd: str | None = None,
) -> list[str]:
    failures = []
    base_env = {"GREP_THREADS": "1"}
    if ref_env:
        base_env.update(ref_env)
    try:
        ref_out, ref_err, ref_rc = run_cmd(
            ref_bin,
            ref_args if ref_args is not None else args,
            env_extra=base_env,
            stdin_bytes=stdin_bytes,
            stdout_path=stdout_path,
            cwd=cwd,
        )
    except subprocess.TimeoutExpired:
        return [f"{label}: reference timed out ({args})"]

    for cand_bin, cand_env in cand_bins:
        env_merged = dict(ref_env or {})
        env_merged.update(cand_env)
        try:
            got_out, got_err, got_rc = run_cmd(
                cand_bin,
                args,
                env_extra=env_merged,
                stdin_bytes=stdin_bytes,
                stdout_path=stdout_path,
                cwd=cwd,
            )
        except subprocess.TimeoutExpired:
            failures.append(
                f"{label}: HANG/TIMEOUT on {os.path.basename(cand_bin)} env={cand_env} args={args}"
            )
            continue
        if (got_out, got_err, got_rc) != (ref_out, ref_err, ref_rc):
            failures.append(
                f"{label}: MISMATCH on {os.path.basename(cand_bin)} env={cand_env} args={args}\n"
                f"  rc: got={got_rc} exp={ref_rc}\n"
                f"  stdout: got={got_out[:200]!r} exp={ref_out[:200]!r}\n"
                f"  stderr: got={got_err[:200]!r} exp={ref_err[:200]!r}"
            )
    return failures


UNSATISFIABLE_NO_LITERAL_BRANCH = "$.^"


def disable_prefilter_args(args: list[str]) -> list[str]:
    if "-P" in args or "-F" in args:
        return list(args)
    if "-e" in args:
        return list(args[:-1]) + ["-e", UNSATISFIABLE_NO_LITERAL_BRANCH, args[-1]]
    return list(args[:-2]) + ["-e", args[-2], "-e", UNSATISFIABLE_NO_LITERAL_BRANCH, args[-1]]


def detect_utf8_locale() -> str | None:
    try:
        out = subprocess.check_output(["locale", "-a"], stderr=subprocess.DEVNULL).decode(
            "latin1"
        )
    except Exception:
        return None
    for pref in ("en_US.utf8", "en_US.UTF-8", "C.utf8", "C.UTF-8"):
        for line in out.splitlines():
            if line.strip().lower() == pref.lower():
                return line.strip()
    return None


def suite_prefilter_regex_differential(
    ref_bin: str, cand_bins: list[tuple[str, dict[str, str]]], td: str, seed: int
) -> list[str]:
    failures = []
    corpus_lines = [
        "ab)cd",
        "abcd",
        "ab))cd",
        "ef123gh",
        "cd",
        "ef",
        "ab",
        "ab)ef",
        "foo€bar",
        "foobar",
        "foo€€bar",
        "abécd",
        "abéécd",
        "ab😀cd",
        "ab{1,a}cd",
        "ab{1,a}-ab{1,a}",
        "ab{,2}cd",
        "a-a",
        "aa-aa",
        "[[a]bcd",
        "[[:a]bcd",
        "[[.a]bcd",
        "[[=a]bcd",
        "[]a]bcd",
        "xyz--xyz",
        "xy--xy",
        "ab\\cd",
        "CAFE_TOKEN",
        "cafe_token",
    ]
    corpus_path = os.path.join(td, "regex_corpus.txt")
    with open(corpus_path, "wb") as f:
        f.write(("\n".join(corpus_lines) + "\n").encode("utf-8"))

    targeted_cases = [
        ("ere_unmatched_rparen_exact", ["-E", "ef.*gh|ab)cd"]),
        ("ere_unmatched_rparen_plus", ["-E", "ef.*gh|ab)+cd"]),
        ("ere_unmatched_rparen_qmark", ["-E", "ef.*gh|ab)?cd"]),
        ("ere_unmatched_rparen_star", ["-E", "ef.*gh|ab)*cd"]),
        ("ere_unmatched_rparen_brace", ["-E", "ef.*gh|ab){1,2}cd"]),
        ("ere_unmatched_rparen_before_group", ["-E", "ab)?|(cd|ef)"]),
        ("ere_unmatched_rparen_nested", ["-E", "(a)b)c|ef.*gh"]),
        ("ere_unmatched_rparen_multi_e", ["-E", "-e", "xy.*zw", "-e", "ab)", "-e", "cd"]),
        ("ere_unmatched_rparen_must2", ["-E", "ab)c.*d|ef.*gh"]),
        ("ere_bad_brace_backref", ["-E", "(ab{1,a})-\\1"]),
        ("bre_bad_brace_backref", ["\\(ab\\{1,a\\}\\)-\\1"]),
        ("ere_open_comma_brace_backref", ["-E", "(a{,2})-\\1"]),
        ("bre_open_comma_brace_backref", ["\\(a\\{,2\\}\\)-\\1"]),
        ("ere_unclosed_collating_bracket", ["-E", "[[.a]bcd|ef.*gh"]),
        ("ere_unclosed_equiv_bracket", ["-E", "[[=a]bcd|ef.*gh"]),
        ("ere_unclosed_charclass_bracket", ["-E", "[[:a]bcd|ef.*gh"]),
        ("single_lit_bre_plain", ["CAFE_TOKEN"]),
        ("single_lit_ere_plain", ["-E", "CAFE_TOKEN"]),
        ("single_lit_ere_escaped_parens", ["-E", r"ab\)cd"]),
        ("single_lit_bre_escaped_dot", [r"ab\.cd"]),
        ("single_lit_bre_word", ["-w", "CAFE_TOKEN"]),
        ("single_lit_ere_word", ["-E", "-w", "CAFE_TOKEN"]),
        ("exact_deferred_dfa_ere", ["-E", r"ef[0-9]+gh"]),
    ]

    for name, args in targeted_cases:
        for extra in ([], ["-n"], ["-o"], ["-i"], ["-v"], ["-c"]):
            full_args = extra + args + [corpus_path]
            oracle_args = disable_prefilter_args(full_args)
            failures.extend(
                assert_equiv(
                    f"regex:{name}:{extra}",
                    ref_bin,
                    cand_bins,
                    full_args,
                    ref_args=oracle_args,
                    ref_env={"LC_ALL": "C"},
                )
            )

    u8_loc = detect_utf8_locale()
    if u8_loc:
        u8_cases = [
            ("u8_esc_euro_qmark_alt", ["-E", "foo\\€?bar|ef.*gh"]),
            ("u8_esc_euro_star_alt", ["-E", "foo\\€*bar|ef.*gh"]),
            ("u8_esc_euro_plus_alt", ["-E", "foo\\€+bar|ef.*gh"]),
            ("u8_esc_euro_plain_alt", ["-E", "foo\\€bar|ef.*gh"]),
            ("u8_esc_eacute_qmark_alt", ["-E", "ab\\é?cd|ef.*gh"]),
            ("u8_esc_emoji_qmark_alt", ["-E", "ab\\😀?cd|ef.*gh"]),
            ("u8_esc_euro_bre_alt", ["foo\\€\\?bar\\|ef.*gh"]),
            ("u8_esc_euro_must2_qmark", ["-E", "foo.*\\€?bar"]),
            ("u8_esc_euro_must2_star", ["-E", "foo.*\\€*bar"]),
            ("u8_esc_eacute_must2_brace", ["-E", "ab.*\\é{0,2}cd"]),
            ("u8_unmatched_rparen_alt", ["-E", "ef.*gh|ab)cd"]),
            ("u8_unmatched_rparen_group", ["-E", "ab)?|(cd|ef)"]),
        ]
        for name, args in u8_cases:
            for extra in ([], ["-n"], ["-o"], ["-i"]):
                full_args = extra + args + [corpus_path]
                oracle_args = disable_prefilter_args(full_args)
                failures.extend(
                    assert_equiv(
                        f"u8_regex:{name}:{extra}",
                        ref_bin,
                        cand_bins,
                        full_args,
                        ref_args=oracle_args,
                        ref_env={"LC_ALL": u8_loc},
                    )
                )

    rng = random.Random(seed)
    atoms_ere = [
        "ab",
        "cd",
        "ef",
        "a)b",
        "ab)",
        "[ab]",
        "[]a]",
        "[[:alpha:]]",
        "[[.a]",
        "\\.",
        "a{1,2}",
        "a{1,a}",
        "a{,2}",
        "(cd|ef)",
        ".*",
    ]
    quants_ere = ["", "?", "+", "*", "{1,2}", "{0,1}"]
    flag_combos = [[], ["-i"], ["-w"], ["-x"], ["-v"], ["-o"], ["-n", "-b"], ["-c"]]
    for idx in range(60):
        n_branches = rng.randint(2, 4)
        branches = []
        for _ in range(n_branches):
            n_atoms = rng.randint(1, 3)
            b = "".join(
                rng.choice(atoms_ere) + rng.choice(quants_ere) for _ in range(n_atoms)
            )
            branches.append(b)
        pat = "|".join(branches)
        fl = rng.choice(flag_combos)
        loc = u8_loc if (u8_loc and idx % 2 == 1) else "C"
        full_args = ["-E"] + fl + ["-e", pat, corpus_path]
        oracle_args = disable_prefilter_args(full_args)
        failures.extend(
            assert_equiv(
                f"fuzz_ere:{idx}:{pat}",
                ref_bin,
                cand_bins,
                full_args,
                ref_args=oracle_args,
                ref_env={"LC_ALL": loc},
            )
        )

    _, _, pcre_ok = run_cmd(ref_bin, ["-P", "^.$"], stdin_bytes=b".\n")
    if pcre_ok == 0:
        pcre_cases = [
            ("pcre_quote_meta", r"\Qab)cd\E"),
            ("pcre_inline_icase", r"(?i:cafe)_token"),
            ("pcre_extended_comment", "(?x) ab cd # ignore comment"),
            ("pcre_backref", r"(a+)\1-ab"),
            ("pcre_brace_quant", r"ab{1,2}cd"),
            ("pcre_lookahead", r"(?=abcd)ab.."),
            ("pcre_neg_lookahead", r"(?!abxx)ab.."),
        ]
        for name, ppat in pcre_cases:
            for fl in ([], ["-i"], ["-n", "-b"], ["-o"], ["-v"]):
                full_args = ["-P"] + fl + [ppat, corpus_path]
                oracle_args = ["-P"] + fl + [f"(?!)a|{ppat}", corpus_path]
                failures.extend(
                    assert_equiv(
                        f"pcre_prefilter:{name}:{fl}",
                        ref_bin,
                        cand_bins,
                        full_args,
                        ref_args=oracle_args,
                    )
                )
    return failures


def suite_dev_full_and_output_ordering(
    ref_bin: str, cand_bins: list[tuple[str, dict[str, str]]], td: str
) -> list[str]:
    if not (os.path.exists("/dev/full") and os.access("/dev/full", os.W_OK)):
        return []
    failures = []
    f_small_match = os.path.join(td, "df_small.txt")
    f_bin_match = os.path.join(td, "df_bin.bin")
    f_intra_big_bin = os.path.join(td, "df_intra_big.bin")
    f_missing = os.path.join(td, "df_no_such_file.txt")

    with open(f_small_match, "wb") as f:
        f.write(b"PAT_DF_HIT line 1\n")
    with open(f_bin_match, "wb") as f:
        f.write(b"\x00PAT_DF_HIT binary\n")
    with open(f_intra_big_bin, "wb") as f:
        f.write((b"PAT_DF_HIT_LONG_LINE_" + b"x" * 80 + b"\n") * 1200)
        f.write(b"\x00PAT_DF_HIT_LONG_LINE_binary_tail\n")

    walk_dir1 = os.path.join(td, "df_walk_small_bin")
    os.makedirs(walk_dir1, exist_ok=True)
    shutil.copyfile(f_small_match, os.path.join(walk_dir1, "01_text.txt"))
    shutil.copyfile(f_bin_match, os.path.join(walk_dir1, "02_bin.bin"))

    walk_dir2 = os.path.join(td, "df_walk_intra")
    os.makedirs(walk_dir2, exist_ok=True)
    shutil.copyfile(f_small_match, os.path.join(walk_dir2, "01_text.txt"))
    shutil.copyfile(f_intra_big_bin, os.path.join(walk_dir2, "02_intra.bin"))

    dev_full_cases = [
        ("rob_small_then_bin", ["PAT_DF_HIT", f_small_match, f_bin_match], None),
        ("rob_small_then_missing", ["PAT_DF_HIT", f_small_match, f_missing], None),
        (
            "rob_intra_big_then_bin",
            ["PAT_DF_HIT_LONG_LINE_", f_small_match, f_intra_big_bin],
            None,
        ),
        ("walk_small_then_bin", ["-r", "PAT_DF_HIT", walk_dir1], None),
        ("walk_intra_big_then_bin", ["-r", "PAT_DF_HIT_LONG_LINE_", walk_dir2], None),
        (
            "stream_n_dev_full",
            ["-n", "PAT_DF_HIT"],
            (b"PAT_DF_HIT stream line\n" * 100),
        ),
        (
            "stream_bin_dev_full",
            ["PAT_DF_HIT"],
            (b"PAT_DF_HIT stream line\n" * 200) + b"\x00PAT_DF_HIT bin\n",
        ),
    ]

    for name, args, stdin_data in dev_full_cases:
        failures.extend(
            assert_equiv(
                f"dev_full:{name}",
                ref_bin,
                cand_bins,
                args,
                stdin_bytes=stdin_data,
                stdout_path="/dev/full",
            )
        )
    return failures


def suite_stream_and_chunked_edges(
    ref_bin: str, cand_bins: list[tuple[str, dict[str, str]]], td: str
) -> list[str]:
    failures = []
    f_before = os.path.join(td, "st_before.txt")
    f_after = os.path.join(td, "st_after.txt")
    with open(f_before, "wb") as f:
        f.write(b"BEFORE_FILE_HIT_1\nno_hit\nBEFORE_FILE_HIT_2\n")
    with open(f_after, "wb") as f:
        f.write(b"AFTER_FILE_HIT_1\nno_hit\nAFTER_FILE_HIT_2\n")

    pipe_payload = (
        b"PIPE_SLICE0_HIT\n"
        + (b"filler_line_without_match_0123456789abcdef\n" * 30000)
        + b"PIPE_SLICE1_HIT\n"
    )
    for flags in ([], ["-n"], ["-b"], ["-c"], ["-H", "--label=PIPED_IN"]):
        failures.extend(
            assert_equiv(
                f"stdin_order:{flags}",
                ref_bin,
                cand_bins,
                flags + ["-E", "BEFORE_FILE_HIT|PIPE_SLICE|AFTER_FILE_HIT", f_before, "-", f_after],
                stdin_bytes=pipe_payload,
            )
        )

    big_self = os.path.join(td, "big_self_out.txt")
    with open(big_self, "wb") as f:
        f.write((b"SELF_OUT_LINE_0123456789abcdefghijklmnopqrstuvwxyz\n") * 45000)
    cp_path = os.path.join(td, "self_copy.txt")
    for s_flag in ([], ["-s"]):
        for label_bin, env_extra in [("ref", {"GREP_THREADS": "1"})] + cand_bins:
            bin_to_run = ref_bin if label_bin == "ref" else label_bin
            shutil.copyfile(big_self, cp_path)
            with open(cp_path, "ab") as out_f:
                env = os.environ.copy()
                env.update(env_extra)
                p = subprocess.run(
                    [bin_to_run] + s_flag + ["SELF_OUT_LINE", cp_path],
                    stdout=out_f,
                    stderr=subprocess.PIPE,
                    env=env,
                    timeout=15.0,
                )
            os.remove(cp_path)
            norm_err = normalize_stderr(p.stderr, bin_to_run)
            if label_bin == "ref":
                exp_rc, exp_err = p.returncode, norm_err
            else:
                if (p.returncode, norm_err) != (exp_rc, exp_err):
                    failures.append(
                        f"chunked_self_out:{s_flag} on {os.path.basename(bin_to_run)} env={env_extra}: "
                        f"got=({p.returncode}, {norm_err!r}) exp=({exp_rc}, {exp_err!r})"
                    )

    _, _, ver_rc = run_cmd(ref_bin, ["-P", "^.$"], stdin_bytes=b".\n")
    if ver_rc == 0:
        pcre_big = os.path.join(td, "pcre_backtrack_2m.txt")
        with open(pcre_big, "wb") as f:
            f.write((b"filler_line_0123456789abcdef\n") * 80000)
            f.write(b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab\n")
        failures.extend(
            assert_equiv(
                "chunked_pcre_backtrack_filename",
                ref_bin,
                cand_bins,
                ["-n", "-P", "^((a+)*)+$", pcre_big],
            )
        )
    return failures


def suite_fault_injection_and_walker_defer(
    src_grep: str, shrunk_grep: str | None, hooks_grep: str | None, td: str
) -> list[str]:
    failures = []
    cc = os.environ.get("CC", "cc")
    fail_pt_c = os.path.join(td, "fail_pthread.c")
    fail_pt_so = os.path.join(td, "fail_pthread.so")
    with open(fail_pt_c, "w") as f:
        f.write(
            """#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    static int (*real_create)(pthread_t *, const pthread_attr_t *,
                              void *(*)(void *), void *) = NULL;
    static atomic_int call_num = 0;
    if (!real_create)
        real_create = dlsym(RTLD_NEXT, "pthread_create");
    const char *at_s = getenv("FAIL_PTHREAD_CREATE_AT");
    int at = at_s ? atoi(at_s) : 0;
    int cur = atomic_fetch_add(&call_num, 1) + 1;
    if (at > 0 && cur >= at)
        return EAGAIN;
    return real_create(thread, attr, start_routine, arg);
}
"""
        )

    preload_ok = False
    if (
        not os.environ.get("ASAN_OPTIONS")
        and not os.environ.get("TSAN_OPTIONS")
        and subprocess.run(
            [cc, "-shared", "-fPIC", "-O2", fail_pt_c, "-o", fail_pt_so, "-ldl", "-lpthread"],
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    ):
        probe_out, _, probe_rc = run_cmd(
            src_grep,
            ["probe"],
            env_extra={"LD_PRELOAD": fail_pt_so, "GREP_THREADS": "1"},
            stdin_bytes=b"probe\n",
        )
        preload_ok = probe_rc == 0 and probe_out == b"probe\n"

    big_file = os.path.join(td, "pt_big.txt")
    with open(big_file, "wb") as f:
        for i in range(50000):
            if i % 5000 == 0:
                f.write(f"line_{i} PT_TARGET_HIT value\n".encode("ascii"))
            else:
                f.write(f"line_{i} ordinary filler 0123456789abcdef\n".encode("ascii"))

    many_files = []
    many_dir = os.path.join(td, "pt_many")
    os.makedirs(many_dir, exist_ok=True)
    for i in range(12):
        p = os.path.join(many_dir, f"f_{i:02d}.txt")
        with open(p, "wb") as f:
            f.write(f"file_{i} PT_TARGET_HIT\nother_{i}\n".encode("ascii"))
        many_files.append(p)

    if preload_ok:
        pt_cands = []
        for fail_at in ("1", "2"):
            pt_cands.append(
                (
                    src_grep,
                    {
                        "GREP_THREADS": "4",
                        "LD_PRELOAD": fail_pt_so,
                        "FAIL_PTHREAD_CREATE_AT": fail_at,
                    },
                )
            )
            if shrunk_grep:
                pt_cands.append(
                    (
                        shrunk_grep,
                        {
                            "GREP_THREADS": "4",
                            "LD_PRELOAD": fail_pt_so,
                            "FAIL_PTHREAD_CREATE_AT": fail_at,
                        },
                    )
                )
        for case_name, args, stdin_data in [
            ("pt_chunk_sparse", ["-n", "PT_TARGET_HIT", big_file], None),
            ("pt_chunk_count", ["-c", "PT_TARGET_HIT", big_file], None),
            ("pt_chunk_ctx", ["-C1", "-n", "PT_TARGET_HIT", big_file], None),
            ("pt_batch_files", ["-n", "PT_TARGET_HIT"] + many_files[:10], None),
            ("pt_walk_dir", ["-r", "-n", "PT_TARGET_HIT", many_dir], None),
            ("pt_stream_pipe", ["-n", "PT_TARGET_HIT"], b"PT_TARGET_HIT\n" * 6000),
        ]:
            failures.extend(
                assert_equiv(
                    f"pthread_fail:{case_name}",
                    src_grep,
                    pt_cands,
                    args,
                    stdin_bytes=stdin_data,
                )
            )

    for mode_flag in ("-r", "-R"):
        for t in ("1", "4"):
            wdir = os.path.join(td, f"wdefer_{mode_flag[1]}_{t}")
            os.makedirs(os.path.join(wdir, "sub1"), exist_ok=True)
            os.makedirs(os.path.join(wdir, "sub2"), exist_ok=True)
            for idx in range(6):
                sd = os.path.join(wdir, f"dir_{idx}")
                os.makedirs(sd, exist_ok=True)
                with open(os.path.join(sd, "f.txt"), "wb") as f:
                    f.write(f"DEFER_WALK_HIT dir_{idx}\n".encode("ascii"))
            out_inside = os.path.join(wdir, "sub2", "zz_out.txt")
            with open(out_inside, "wb") as f:
                f.write(b"DEFER_WALK_HIT initial_seed\n")
            if mode_flag == "-R":
                os.symlink(os.path.join("..", "sub2", "zz_out.txt"), os.path.join(wdir, "sub1", "aa_link_to_out.txt"))

            bin_use = shrunk_grep if (shrunk_grep and t != "1") else src_grep
            env = os.environ.copy()
            env["GREP_THREADS"] = t
            env["GREP_TEST_WALK_MAX_DIR_FDS"] = "1"
            with open(out_inside, "ab") as out_fh:
                p = subprocess.run(
                    [bin_use, mode_flag, "-m", "1", "DEFER_WALK_HIT", "."],
                    stdout=out_fh,
                    stderr=subprocess.PIPE,
                    env=env,
                    cwd=wdir,
                    timeout=15.0,
                )
            with open(out_inside, "rb") as f:
                final_bytes = f.read()
            if t == "1":
                exp_rc, exp_err, exp_bytes = (
                    p.returncode,
                    normalize_stderr(p.stderr, bin_use),
                    final_bytes,
                )
            else:
                got_err = normalize_stderr(p.stderr, bin_use)
                if (p.returncode, got_err, final_bytes) != (exp_rc, exp_err, exp_bytes):
                    failures.append(
                        f"walk_defer:{mode_flag}: rc=({p.returncode} vs {exp_rc}) err=({got_err!r} vs {exp_err!r}) "
                        f"bytes=({final_bytes!r} vs {exp_bytes!r})"
                    )
    return failures


def suite_cross_path_consistency(
    ref_bin: str, cand_bins: list[tuple[str, dict[str, str]]], td: str
) -> list[str]:
    failures = []
    xp_dir = os.path.join(td, "xp_tree")
    os.makedirs(os.path.join(xp_dir, "sub_a"), exist_ok=True)
    os.makedirs(os.path.join(xp_dir, "sub_b"), exist_ok=True)

    cmd_files = []
    for i in range(18):
        sub = "sub_a" if i < 9 else "sub_b"
        p = os.path.join(xp_dir, sub, f"file_{i:02d}.txt")
        with open(p, "wb") as f:
            if i == 5:
                for k in range(2500):
                    if k % 500 == 0:
                        f.write(f"XP_ALPHA_{i}_{k} XP_BETA_TOKEN line\n".encode("ascii"))
                    else:
                        f.write(f"ordinary_filler_line_{i}_{k}_0123456789abcdef\n".encode("ascii"))
            else:
                f.write(
                    f"header_{i}\nXP_ALPHA_{i} XP_BETA_TOKEN\nmiddle_{i}\nXP_GAMMA_{i} xp_beta_token\nfooter_{i}\n".encode(
                        "ascii"
                    )
                )
        cmd_files.append(p)

    big_chunk = os.path.join(td, "xp_chunk_2m.txt")
    with open(big_chunk, "wb") as f:
        for k in range(55000):
            if k % 4000 == 0:
                f.write(f"chunk_line_{k} XP_ALPHA_CHUNK XP_BETA_TOKEN end\n".encode("ascii"))
            elif k % 7777 == 0:
                f.write(f"chunk_line_{k} XP_GAMMA_CHUNK xp_beta_token end\n".encode("ascii"))
            else:
                f.write(f"chunk_line_{k} filler_content_0123456789abcdef\n".encode("ascii"))
    with open(big_chunk, "rb") as f:
        big_bytes = f.read()

    engines = [
        ("F", ["-F", "XP_BETA_TOKEN"]),
        ("G", ["-G", r"XP_ALPHA_[0-9A-Z_]* XP_BETA_TOKEN"]),
        ("E", ["-E", r"XP_ALPHA_[0-9A-Z_]+|XP_GAMMA_[0-9A-Z_]+"]),
    ]
    _, _, pcre_ok = run_cmd(ref_bin, ["-P", "^.$"], stdin_bytes=b".\n")
    if pcre_ok == 0:
        engines.append(("P", ["-P", r"(?:XP_ALPHA|XP_GAMMA)_[0-9A-Z_]+\s+xp_beta_token"]))

    flag_sets = [
        [],
        ["-n", "-b"],
        ["-i", "-n"],
        ["-o", "-n"],
        ["-v", "-c"],
        ["-c"],
        ["-l"],
        ["-L"],
        ["-q"],
        ["-m", "2", "-n"],
        ["-C", "1", "-n", "--group-separator=--XPSEP--"],
    ]

    for eng_name, eng_args in engines:
        for fl in flag_sets:
            targets = [
                ("1file", [cmd_files[0]], None),
                ("6files_rob", cmd_files[:6], None),
                ("18files_walk", cmd_files, None),
                ("tree_r", ["-r", xp_dir], None),
                ("chunk_2m", [big_chunk], None),
            ]
            if "-C" not in fl and "-m" not in fl and "-q" not in fl:
                targets.append(("stream_2m", ["-"], big_bytes))
            for tname, targs, tstdin in targets:
                failures.extend(
                    assert_equiv(
                        f"cross_path:{eng_name}:{tname}:{fl}",
                        ref_bin,
                        cand_bins,
                        fl + eng_args + targs,
                        stdin_bytes=tstdin,
                    )
                )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser.add_argument("--seed", type=int, default=20261018)
    args = parser.parse_args()

    repo = os.path.abspath(args.repo_root)
    src_grep = os.path.join(repo, "src", "grep")
    base_grep = os.path.join(repo, "benchmarks", "grep-baseline")
    shrunk_grep = os.path.join(repo, "src", "grep-shrunk")
    hooks_grep = os.path.join(repo, "src", "grep-hooks")

    ref_bin = base_grep if (os.path.exists(base_grep) and os.access(base_grep, os.X_OK)) else src_grep
    shrunk_bin = shrunk_grep if (os.path.exists(shrunk_grep) and os.access(shrunk_grep, os.X_OK)) else None
    hooks_bin = hooks_grep if (os.path.exists(hooks_grep) and os.access(hooks_grep, os.X_OK)) else None

    cand_bins: list[tuple[str, dict[str, str]]] = [
        (src_grep, {"GREP_THREADS": "1"}),
        (src_grep, {"GREP_THREADS": "2"}),
        (src_grep, {"GREP_THREADS": "4"}),
    ]
    if shrunk_bin:
        cand_bins.append((shrunk_bin, {"GREP_THREADS": "2"}))
        cand_bins.append((shrunk_bin, {"GREP_THREADS": "4"}))

    all_failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        all_failures.extend(suite_prefilter_regex_differential(ref_bin, cand_bins, td, args.seed))
        all_failures.extend(suite_dev_full_and_output_ordering(ref_bin, cand_bins, td))
        all_failures.extend(suite_stream_and_chunked_edges(ref_bin, cand_bins, td))
        all_failures.extend(suite_fault_injection_and_walker_defer(src_grep, shrunk_bin, hooks_bin, td))
        all_failures.extend(suite_cross_path_consistency(ref_bin, cand_bins, td))

    if all_failures:
        for msg in all_failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        print(f"exhaustive_gap_fuzzer: {len(all_failures)} failure(s)", file=sys.stderr)
        return 1

    print("exhaustive_gap_fuzzer: ALL SUITES PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
