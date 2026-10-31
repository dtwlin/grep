/* Unit tests for src/dfa-prefilter.h and src/dfa-prefilter.c.
   Copyright (C) 2026 David Lin <dtwlin@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#include "dfa-prefilter.h"
#include "regex-lex.h"
#include "system.h"

#include <stdio.h>

#define S(lit) (lit), (idx_t) (sizeof (lit) - 1)

static int failures;

/* Copy SRC[0..LEN) into an exact-sized heap buffer without any trailing
   NUL or newline sentinel so AddressSanitizer catches over-reads.  */
static char *
exact_dup (char const *src, idx_t len)
{
  char *p = malloc (len ? (size_t) len : 1);
  if (!p)
    abort ();
  if (len > 0)
    memcpy (p, src, (size_t) len);
  return p;
}

static void
check_branches_2 (char const *label, char const *pat, idx_t pat_len,
                  reg_syntax_t syn, bool utf8, bool words_or_lines,
                  ptrdiff_t exp_n,
                  char const *lit0, bool exact0,
                  char const *lit1, bool exact1)
{
  char *buf = exact_dup (pat, pat_len);
  struct prefilter_branch *br = nullptr;
  ptrdiff_t n = prefilter_branch_literals (buf, pat_len, syn, utf8,
                                           words_or_lines, &br);
  if (n != exp_n)
    {
      fprintf (stderr, "FAIL %s: n=%td, expected %td\n", label, n, exp_n);
      failures++;
    }
  else if (n == 2)
    {
      idx_t len0 = strlen (lit0);
      idx_t len1 = strlen (lit1);
      if (br[0].len != len0 || memcmp (br[0].lit, lit0, len0) != 0
          || br[0].exact != exact0
          || br[1].len != len1 || memcmp (br[1].lit, lit1, len1) != 0
          || br[1].exact != exact1)
        {
          fprintf (stderr,
                   "FAIL %s: got (%.*s/%d, %.*s/%d), "
                   "expected (%s/%d, %s/%d)\n",
                   label, (int) br[0].len, br[0].lit, (int) br[0].exact,
                   (int) br[1].len, br[1].lit, (int) br[1].exact,
                   lit0, (int) exact0, lit1, (int) exact1);
          failures++;
        }
    }
  prefilter_branches_free (br, n > 0 ? n : 0);
  free (buf);
}

static void
check_must2 (char const *label, char const *pat, idx_t pat_len,
             reg_syntax_t syn, bool utf8, char const *primary,
             char const *exp_must2)
{
  char *buf = exact_dup (pat, pat_len);
  char must2[64];
  memset (must2, 'X', sizeof must2);
  idx_t got_len = prefilter_secondary_must (buf, pat_len, syn, utf8,
                                            primary, must2);
  idx_t exp_len = exp_must2 ? strlen (exp_must2) : 0;
  if (got_len != exp_len
      || (exp_len > 0 && memcmp (must2, exp_must2, exp_len) != 0))
    {
      fprintf (stderr, "FAIL %s: got len=%td, expected %s (len=%td)\n",
               label, got_len, exp_must2 ? exp_must2 : "<none>", exp_len);
      failures++;
    }
  free (buf);
}

static void
check_backref (char const *label, char const *pat, idx_t pat_len,
               reg_syntax_t syn, bool utf8,
               bool exp_enabled, idx_t exp_min_len, char const *exp_sep)
{
  char *buf = exact_dup (pat, pat_len);
  struct backref_filter bf;
  memset (&bf, 0x5a, sizeof bf);
  prefilter_backref_filter (buf, pat_len, syn, utf8, &bf);
  idx_t exp_sep_len = exp_sep ? strlen (exp_sep) : 0;
  if (bf.enabled != exp_enabled
      || (exp_enabled
          && (bf.min_len != exp_min_len
              || bf.sep_len != exp_sep_len
              || (exp_sep_len > 0
                  && memcmp (bf.sep, exp_sep, exp_sep_len) != 0))))
    {
      fprintf (stderr,
               "FAIL %s: got enabled=%d min=%td sep_len=%td, "
               "expected enabled=%d min=%td sep_len=%td\n",
               label, (int) bf.enabled, bf.min_len, bf.sep_len,
               (int) exp_enabled, exp_min_len, exp_sep_len);
      failures++;
    }
  free (buf);
}

static void
check_line_match (char const *label, struct backref_filter const *bf,
                  char const *line, bool exp)
{
  idx_t len = strlen (line);
  char *buf = exact_dup (line, len);
  bool got = backref_line_can_match (bf, buf, len);
  if (got != exp)
    {
      fprintf (stderr, "FAIL %s: got %d, expected %d on '%s'\n",
               label, (int) got, (int) exp, line);
      failures++;
    }
  free (buf);
}

static void
run_c_locale_tests (void)
{
  reg_syntax_t ere = RE_SYNTAX_EGREP;
  reg_syntax_t bre = RE_SYNTAX_GREP;
  reg_syntax_t awk = RE_SYNTAX_AWK;

  /* Phase 22: bracket '|' does not split; BRE '\|' splits unless limited.  */
  check_branches_2 ("p22-bracket-pipe-single", S ("[a|b]"), ere, false, false,
                    -1, nullptr, false, nullptr, false);
  check_branches_2 ("p22-bracket-pipe-two", S ("[a|b]cd|ef.*gh"), ere, false,
                    false, 2, "cd", false, "ef", false);
  check_branches_2 ("p22-bre-vbar", S ("ab\\|cd"), bre, false, false,
                    2, "ab", true, "cd", true);
  check_branches_2 ("p22-bre-vbar-words", S ("ab\\|cd"), bre, false, true,
                    2, "ab", false, "cd", false);
  check_branches_2 ("p22-bre-limited", S ("ab\\|cd"),
                    RE_SYNTAX_POSIX_MINIMAL_BASIC, false, false,
                    -1, nullptr, false, nullptr, false);

  /* Phase 27: '{a}' vs BRE '\{1,2\}'.  */
  check_branches_2 ("p27-ere-non-interval", S ("ab{a}cd|ef.*gh"), ere, false,
                    false, 2, "ab", false, "ef", false);
  check_branches_2 ("p27-bre-brace-literal", S ("ab{a}cd\\|ef.*gh"), bre,
                    false, false, 2, "ab{a}cd", true, "ef", false);
  check_branches_2 ("p27-bre-interval", S ("ab\\{1,2\\}cd\\|ef.*gh"), bre,
                    false, false, 2, "cd", false, "ef", false);

  /* Phase 28: '[\]]' under RE_BACKSLASH_ESCAPE_IN_LISTS and '{,}' under
     RE_INTERVALS.  */
  check_branches_2 ("p28-bk-in-list-ere", S ("[\\]|x]ab|cd.*ef"), ere, false,
                    false, -1, nullptr, false, nullptr, false);
  check_branches_2 ("p28-bk-in-list-awk", S ("[\\]|x]ab|cd.*ef"), awk, false,
                    false, 2, "ab", false, "cd", false);
  check_branches_2 ("p28-empty-iv-on", S ("xb{,}cd|ef.*gh"), ere, false,
                    false, 2, "cd", false, "ef", false);
  check_branches_2 ("p28-empty-iv-off", S ("xb{,}cd|ef.*gh"),
                    ere & ~RE_INTERVALS, false, false,
                    2, "xb", false, "ef", false);

  /* Phase 28: back-reference filter bailout cases and interval bounds.  */
  check_backref ("p28-bref-alt-close", S ("(abcdef)\\1)|xy"), ere, false,
                 false, 0, nullptr);
  check_backref ("p28-bref-leading-star", S ("(*abcdef)\\1"), ere, false,
                 false, 0, nullptr);
  check_backref ("p28-bref-escaped-high", S ("(\\\303\251*abc)\\1"), ere,
                 false, false, 0, nullptr);
  check_backref ("p28-bref-qmin-2559", S ("(a{2559})_\\1"), ere, false,
                 true, 2559, "_");
  check_backref ("p28-bref-qmin-2560", S ("(a{2560})_\\1"), ere, false,
                 false, 0, nullptr);

  /* Phase 31: unclosed '[[:' inside '[...]', unbalanced ')', and exact-buffer
     trailing backslash / truncated token bounds.  */
  check_branches_2 ("p31-unclosed-charclass", S ("[[:a|b]cd|ef.*gh"), ere,
                    false, false, 2, "cd", false, "ef", false);
  check_branches_2 ("p31-unmatched-rparen-lit", S ("ef.*gh|ab)cd"), ere,
                    false, false, 2, "ef", false, "ab)cd", true);
  check_branches_2 ("p31-unmatched-rparen-quant", S ("ef.*gh|ab)+cd"), ere,
                    false, false, 2, "ef", false, "ab", false);
  check_branches_2 ("p31-bre-unmatched-rparen", S ("ef.*gh\\|ab\\)cd"), bre,
                    false, false, -1, nullptr, false, nullptr, false);
  check_branches_2 ("bounds-trailing-bs", S ("ef.*gh|abc\\"), ere, false,
                    false, 2, "ef", false, "abc", false);
  check_branches_2 ("bounds-trunc-bracket", S ("ef.*gh|abc[[:"), ere, false,
                    false, 2, "ef", false, "abc", false);
  check_backref ("bounds-bref-trailing-bs", S ("(abcd)\\1\\"), ere, false,
                 true, 4, nullptr);
  check_backref ("bounds-bref-unclosed-bs", S ("(abcd\\"), ere, false,
                 false, 0, nullptr);

  /* Secondary-must extraction.  */
  check_must2 ("must2-basic", S ("ab.*cd_efg"), ere, false, "ab", "cd_efg");
  check_must2 ("must2-substr-skip", S ("ab_cd.*cd"), ere, false, "ab_cd",
               nullptr);
  check_must2 ("must2-alt-bail", S ("ab.*(cd|ef)ghij"), ere, false, "ab",
               nullptr);

  /* P-01 style back-reference filter and line matcher.  */
  char const p01[] = "\\b([a-z]{4,})_\\1\\b";
  check_backref ("p01-compile", p01, sizeof p01 - 1, ere, false,
                 true, 4, "_");

  char *p01_buf = exact_dup (p01, sizeof p01 - 1);
  struct backref_filter bf;
  prefilter_backref_filter (p01_buf, sizeof p01 - 1, ere, false, &bf);
  free (p01_buf);

  check_line_match ("p01-hit", &bf, "static int irq_lock_irq_lock = 1;", true);
  check_line_match ("p01-short-word", &bf, "xx abc_abc yy", false);
  check_line_match ("p01-diff-word", &bf, "xx abcd_efgh yy", false);
  check_line_match ("p01-short-line", &bf, "abcd_abc", false);

  char const nosep[] = "(abcd)\\1";
  char *nosep_buf = exact_dup (nosep, sizeof nosep - 1);
  struct backref_filter bf_nosep;
  prefilter_backref_filter (nosep_buf, sizeof nosep - 1, ere, false,
                            &bf_nosep);
  free (nosep_buf);
  check_line_match ("nosep-hit", &bf_nosep, "zzabcdabcdzz", true);
  check_line_match ("nosep-miss", &bf_nosep, "zzabcabcdezz", false);

  char const sep2[] = "(ab)::\\1";
  char *sep2_buf = exact_dup (sep2, sizeof sep2 - 1);
  struct backref_filter bf_sep2;
  prefilter_backref_filter (sep2_buf, sizeof sep2 - 1, ere, false, &bf_sep2);
  free (sep2_buf);
  check_line_match ("sep2-hit", &bf_sep2, "xxab:ab::abzz", true);
  check_line_match ("sep2-miss", &bf_sep2, "xxab:ab:cdzz", false);
}

static void
run_utf8_tests (void)
{
  reg_syntax_t ere = RE_SYNTAX_EGREP;

  /* Phase 27: quantified multibyte U+20AC '*' and U+00E9 '?' add no lead
     bytes.  */
  check_branches_2 ("p27-utf8-euro-star", S ("x\342\202\254*bar|ef.*gh"),
                    ere, true, false, 2, "bar", false, "ef", false);
  check_branches_2 ("p27-utf8-eacute-qmark", S ("a\303\251?bcd|ef.*gh"),
                    ere, true, false, 2, "bcd", false, "ef", false);
  check_must2 ("p27-utf8-must2-no-lead", S ("ab.*x\303\251?z"), ere, true,
               "ab", nullptr);
  check_must2 ("p27-utf8-must2-after", S ("ab.*\342\202\254?cd_ef"), ere, true,
               "ab", "cd_ef");

  /* Phase 28 & 31: invalid UTF-8 in capture group and escaped multibyte.  */
  check_backref ("p28-utf8-invalid-bref", S ("(\200\200{2})\\1"), ere, true,
                 false, 0, nullptr);
  check_backref ("p28-utf8-valid-bref", S ("(\342\202\254{2})_\\1"), ere,
                 true, true, 6, "_");
  check_backref ("p28-nonutf8-mb-guard", S ("(abcd)_\\1"), ere, false,
                 false, 0, nullptr);
  check_branches_2 ("p31-utf8-escaped-mb-star",
                    S ("x\\\303\251*bcd|ef.*gh"),
                    ere, true, false, 2, "bcd", false, "ef", false);
  check_must2 ("p31-utf8-escaped-mb-must2",
               S ("ab.*\\\342\202\254*cd_ef"),
               ere, true, "ab", "cd_ef");
}

static void
run_regex_lex_c_tests (void)
{
  char *b1 = exact_dup (S ("\342\202\254xyz"));
  if (utf8_byte_run_len (b1, b1 + 6) != 3
      || utf8_byte_run_len (b1 + 3, b1 + 6) != 1)
    {
      fprintf (stderr, "FAIL lex-utf8-run\n");
      failures++;
    }
  free (b1);

  struct regex_interval iv;
  char *iv1 = exact_dup (S ("{,3}tail"));
  if (!regex_parse_interval (iv1, iv1 + 8, REGEX_IV_ALLOW_EMPTY_MIN, &iv)
      || iv.min_rep != 0 || !iv.has_comma || iv.end != iv1 + 4
      || regex_parse_interval (iv1, iv1 + 8, 0, &iv))
    {
      fprintf (stderr, "FAIL lex-iv-empty-min\n");
      failures++;
    }
  free (iv1);

  char *iv2 = exact_dup (S ("{,}"));
  if (regex_parse_interval (iv2, iv2 + 3, REGEX_IV_ALLOW_EMPTY_MIN, &iv)
      || !regex_parse_interval (iv2, iv2 + 3,
                                REGEX_IV_ALLOW_EMPTY_MIN
                                | REGEX_IV_ALLOW_EMPTY_BOTH, &iv)
      || iv.end != iv2 + 3)
    {
      fprintf (stderr, "FAIL lex-iv-empty-both\n");
      failures++;
    }
  free (iv2);

  char *iv3 = exact_dup (S ("\\{12,34\\}z"));
  if (!regex_parse_interval (iv3, iv3 + 10, REGEX_IV_BACKSLASH, &iv)
      || iv.min_rep != 12 || !iv.has_comma || iv.end != iv3 + 9)
    {
      fprintf (stderr, "FAIL lex-iv-backslash\n");
      failures++;
    }
  free (iv3);

  char *iv4 = exact_dup (S ("{99999999999999999999999999}"));
  if (!regex_parse_interval (iv4, iv4 + 28, 0, &iv) || iv.min_rep != IDX_MAX)
    {
      fprintf (stderr, "FAIL lex-iv-overflow\n");
      failures++;
    }
  free (iv4);

  char *cc1 = exact_dup (S ("[:alpha:]]"));
  char *cc2 = exact_dup (S ("[:unclosed]"));
  if (regex_bracket_class_end (cc1, cc1 + 10) != cc1 + 9
      || regex_bracket_class_end (cc2, cc2 + 11) != nullptr
      || regex_bracket_class_end (cc1, cc1 + 1) != nullptr)
    {
      fprintf (stderr, "FAIL lex-bracket-class\n");
      failures++;
    }
  free (cc1);
  free (cc2);
}

static void
run_regex_lex_utf8_tests (void)
{
  char *u1 = exact_dup (S ("\342\202\254"));
  char *u2 = exact_dup (S ("\200"));
  char *u3 = exact_dup (S ("\342\202"));
  if (utf8_char_len (u1, u1 + 3) != 3
      || utf8_char_len (u2, u2 + 1) != (size_t) -1
      || utf8_char_len (u3, u3 + 2) != (size_t) -2)
    {
      fprintf (stderr, "FAIL lex-utf8-char-len\n");
      failures++;
    }
  free (u1);
  free (u2);
  free (u3);
}

int
main (int argc, char **argv)
{
  if (argc == 2 && STREQ (argv[1], "--utf8"))
    {
      if ((!setlocale (LC_ALL, "en_US.UTF-8") || MB_CUR_MAX <= 1)
          && (!setlocale (LC_ALL, "C.UTF-8") || MB_CUR_MAX <= 1))
        {
          fprintf (stderr, "SKIP: UTF-8 locale unavailable\n");
          return 77;
        }
      run_utf8_tests ();
      run_regex_lex_utf8_tests ();
    }
  else
    {
      setlocale (LC_ALL, "C");
      run_c_locale_tests ();
      run_regex_lex_c_tests ();
    }

  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
