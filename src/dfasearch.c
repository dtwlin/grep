/* dfasearch.c - searching subroutines using dfa and regex for grep.
   Copyright 1992, 1998, 2000, 2007, 2009-2026 Free Software Foundation, Inc.
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

/* Written August 1992 by Mike Haertel. */

#include <config.h>
#include "intprops.h"
#include <search.h>
#include "die.h"
#include "grep-test-hooks.h"
#include <error.h>
#include <stdatomic.h>

struct backref_filter
{
  bool enabled;
  idx_t min_len;
  idx_t sep_len;
  char sep[8];
};

struct shared_dfa_kwset
{
  kwset_t kwset;
  idx_t kwset_exact_matches;
  char must2[64];
  idx_t must2_len;
  bool begline;
  atomic_ptrdiff_t refs;
  char *pattern;
  idx_t size;
  bool match_icase, match_words, match_lines, exact;
  char eolbyte;
  reg_syntax_t syntax_bits;
  bool dfa_supported;
};

static struct shared_dfa_kwset *dfa_kwset_cache;

struct dfa_comp
{
  /* KWset compiled pattern.  For GEAcompile, we compile
     a list of strings, at least one of which is known to occur in
     any string matching the regexp. */
  kwset_t kwset;
  struct shared_dfa_kwset *shared_kwset;
  char must2[64];
  idx_t must2_len;

  /* DFA compiled regexp. */
  struct dfa *dfa;
  char *dfa_pattern;
  idx_t dfa_pattern_size;
  bool dfa_compiled;

  /* Regex compiled regexps. */
  struct re_pattern_buffer *patterns;
  struct re_pattern_buffer *patterns_alloc;
  struct backref_filter *bref_filters;
  struct backref_filter *bref_filters_alloc;
  idx_t pcount;
  struct re_registers regs;

  /* Number of compiled fixed strings known to exactly match the regexp.
     If kwsexec returns < kwset_exact_matches, then we don't need to
     call the regexp matcher at all. */
  idx_t kwset_exact_matches;

  bool begline;
};

_Thread_local bool suppress_dfawarn;

void
dfaerror (char const *mesg)
{
  die (EXIT_TROUBLE, 0, "%s", mesg);
}

void
dfawarn (char const *mesg)
{
  if (suppress_dfawarn)
    return;
  error (0, 0, _("warning: %s"), mesg);
}

static bool _GL_ATTRIBUTE_PURE
is_dfa_brace_quantifier (char const *p, char const *lim,
                         reg_syntax_t syntax_bits)
{
  if (!(syntax_bits & RE_INTERVALS))
    return false;
  bool ere = (syntax_bits & RE_NO_BK_VBAR) != 0;
  if (ere)
    {
      if (p >= lim || *p != '{')
        return false;
      p++;
    }
  else
    {
      if (p + 1 >= lim || p[0] != '\\' || p[1] != '{')
        return false;
      p += 2;
    }
  idx_t digits = 0;
  bool has_comma = false;
  while (p < lim && *p >= '0' && *p <= '9')
    {
      digits++;
      p++;
    }
  if (p < lim && *p == ',')
    {
      has_comma = true;
      p++;
      while (p < lim && *p >= '0' && *p <= '9')
        {
          digits++;
          p++;
        }
    }
  if (digits == 0 && !has_comma)
    return false;
  if (ere)
    return p < lim && *p == '}';
  else
    return p + 1 < lim && p[0] == '\\' && p[1] == '}';
}

static bool _GL_ATTRIBUTE_PURE
is_quantifier (char const *p, char const *lim, reg_syntax_t syntax_bits)
{
  if (p >= lim)
    return false;
  if (syntax_bits & RE_NO_BK_VBAR) /* ERE */
    {
      if (*p == '*' || *p == '+' || *p == '?')
        return true;
      if (is_dfa_brace_quantifier (p, lim, syntax_bits))
        return true;
      return false;
    }
  else /* BRE */
    {
      if (*p == '*')
        return true;
      if (p + 1 < lim && *p == '\\')
        {
          if (p[1] == '+' || p[1] == '?')
            return true;
          if (is_dfa_brace_quantifier (p, lim, syntax_bits))
            return true;
        }
      return false;
    }
}

static char const * _GL_ATTRIBUTE_PURE
skip_quantifiers (char const *p, char const *lim, reg_syntax_t syntax_bits)
{
  while (p < lim)
    {
      if (syntax_bits & RE_NO_BK_VBAR) /* ERE */
        {
          if (*p == '*' || *p == '+' || *p == '?')
            {
              p++;
              continue;
            }
          if (is_dfa_brace_quantifier (p, lim, syntax_bits))
            {
              p++;
              while (*p != '}')
                p++;
              p++;
              continue;
            }
          break;
        }
      else /* BRE */
        {
          if (*p == '*')
            {
              p++;
              continue;
            }
          if (p + 1 < lim && *p == '\\')
            {
              if (p[1] == '+' || p[1] == '?')
                {
                  p += 2;
                  continue;
                }
              if (is_dfa_brace_quantifier (p, lim, syntax_bits))
                {
                  p += 2;
                  while (!(p[0] == '\\' && p[1] == '}'))
                    p++;
                  p += 2;
                  continue;
                }
            }
          break;
        }
    }
  return p;
}

/* Advance S past a bracket expression '[...]' bounded by LIM.  Since
   dfaparse and re_compile_pattern have already validated the pattern
   syntax, the bracket expression is guaranteed to be closed before LIM.  */
static char const * _GL_ATTRIBUTE_PURE
skip_bracket_expr (char const *s, char const *lim, reg_syntax_t syntax_bits)
{
  /* Skip '['.  */
  s++;
  if (s < lim && *s == '^')
    s++;
  if (s < lim && *s == ']')
    s++;
  while (s < lim && *s != ']')
    {
      if ((syntax_bits & RE_BACKSLASH_ESCAPE_IN_LISTS)
          && *s == '\\' && s + 1 < lim)
        {
          s += 2;
          continue;
        }
      if (s + 1 < lim && *s == '['
          && (s[1] == ':' || s[1] == '.' || s[1] == '='))
        {
          char ctype = s[1];
          char const *q = s + 2;
          while (q + 1 < lim && *q != ']' && !(q[0] == ctype && q[1] == ']'))
            q++;
          if (q + 1 < lim && q[0] == ctype && q[1] == ']')
            {
              s = q + 2;
              continue;
            }
        }
      s++;
    }
  return s < lim ? s + 1 : lim;
}

struct branch_slice
{
  char const *start;
  idx_t len;
};

struct branch_info
{
  char *best_lit;
  idx_t best_len;
  bool is_exact;
};

/* Split RAW_PAT (of length RAW_SIZE) into top-level alternation branches.
   Return a newly allocated array of at least 2 slices and store its length in
   *NBRANCHES_OUT, or return nullptr if RAW_PAT cannot be split cleanly.  */
static struct branch_slice *
split_top_level_branches (char const *raw_pat, idx_t raw_size,
                          reg_syntax_t syntax_bits, idx_t *nbranches_out)
{
  char const *p = raw_pat;
  char const *lim = raw_pat + raw_size;

  idx_t branch_cap = 16;
  idx_t nbranches = 0;
  struct branch_slice *slices = xnmalloc (branch_cap, sizeof *slices);

  char const *b_start = p;
  idx_t paren_depth = 0;

  while (p < lim)
    {
      if (*p == '[')
        {
          p = skip_bracket_expr (p, lim, syntax_bits);
          continue;
        }

      if (*p == '\n' && paren_depth != 0)
        goto fail;

      /* Top-level branch separators.  */
      if (paren_depth == 0)
        {
          idx_t sep_len = 0;
          if (*p == '\n')
            sep_len = 1;
          else if ((syntax_bits & RE_NO_BK_VBAR) && *p == '|')
            sep_len = 1;
          else if (!(syntax_bits & RE_NO_BK_VBAR)
                   && !(syntax_bits & RE_LIMITED_OPS)
                   && *p == '\\' && p + 1 < lim && p[1] == '|')
            sep_len = 2;

          if (sep_len > 0)
            {
              idx_t blen = p - b_start;
              if (blen == 0)
                goto fail;
              if (nbranches == branch_cap)
                slices = xpalloc (slices, &branch_cap, 1, -1, sizeof *slices);
              slices[nbranches].start = b_start;
              slices[nbranches].len = blen;
              nbranches++;
              p += sep_len;
              b_start = p;
              continue;
            }
        }

      /* Track parenthesized group nesting.  */
      if (syntax_bits & RE_NO_BK_PARENS) /* ERE */
        {
          if (*p == '(')
            {
              paren_depth++;
              p++;
              continue;
            }
          if (*p == ')')
            {
              if (paren_depth > 0)
                paren_depth--;
              p++;
              continue;
            }
          if (*p == '\\')
            {
              p += 2;
              continue;
            }
        }
      else /* BRE */
        {
          if (*p == '\\' && p + 1 < lim && p[1] == '(')
            {
              paren_depth++;
              p += 2;
              continue;
            }
          if (*p == '\\' && p + 1 < lim && p[1] == ')')
            {
              if (paren_depth == 0)
                goto fail;
              paren_depth--;
              p += 2;
              continue;
            }
          if (*p == '\\')
            {
              p += 2;
              continue;
            }
        }

      p++;
    }

  if (paren_depth != 0 || b_start >= lim || nbranches < 1)
    goto fail;

  if (nbranches == branch_cap)
    slices = xpalloc (slices, &branch_cap, 1, -1, sizeof *slices);
  slices[nbranches].start = b_start;
  slices[nbranches].len = lim - b_start;
  nbranches++;

  *nbranches_out = nbranches;
  return slices;

 fail:
  free (slices);
  *nbranches_out = 0;
  return nullptr;
}

static inline void
flush_branch_literal (char *best_lit, idx_t *best_len,
                      char const *cur_lit, idx_t *cur_len,
                      bool *branch_is_pure)
{
  if (branch_is_pure)
    *branch_is_pure = false;
  if (*cur_len > *best_len)
    {
      memcpy (best_lit, cur_lit, *cur_len);
      *best_len = *cur_len;
    }
  *cur_len = 0;
}

/* Extract the longest mandatory literal substring (>= 2 bytes) from SLICE.
   Store the result in *OUT and return true on success, or false if the
   branch has no mandatory literal of length >= 2.  */
static bool
extract_branch_best_literal (struct branch_slice slice,
                             reg_syntax_t syntax_bits,
                             struct branch_info *out)
{
  char const *bp = slice.start;
  char const *blim = slice.start + slice.len;
  idx_t blen = slice.len;

  char *cur_lit = xmalloc (blen + 1);
  char *best_lit = xmalloc (blen + 1);
  idx_t cur_len = 0;
  idx_t best_len = 0;
  bool branch_is_pure = true;
  idx_t b_paren = 0;

  while (bp < blim)
    {
      if (*bp == '[')
        {
          flush_branch_literal (best_lit, &best_len, cur_lit, &cur_len,
                                &branch_is_pure);
          bp = skip_bracket_expr (bp, blim, syntax_bits);
          if (b_paren == 0)
            bp = skip_quantifiers (bp, blim, syntax_bits);
          continue;
        }

      if (syntax_bits & RE_NO_BK_PARENS)
        {
          if (*bp == '(')
            {
              flush_branch_literal (best_lit, &best_len, cur_lit, &cur_len,
                                    &branch_is_pure);
              b_paren++;
              bp++;
              continue;
            }
          if (*bp == ')' && b_paren > 0)
            {
              b_paren--;
              bp++;
              if (b_paren == 0)
                bp = skip_quantifiers (bp, blim, syntax_bits);
              continue;
            }
        }
      else
        {
          if (*bp == '\\' && bp + 1 < blim && bp[1] == '(')
            {
              flush_branch_literal (best_lit, &best_len, cur_lit, &cur_len,
                                    &branch_is_pure);
              b_paren++;
              bp += 2;
              continue;
            }
          if (*bp == '\\' && bp + 1 < blim && bp[1] == ')')
            {
              if (b_paren > 0)
                b_paren--;
              bp += 2;
              if (b_paren == 0)
                bp = skip_quantifiers (bp, blim, syntax_bits);
              continue;
            }
        }

      if (b_paren > 0)
        {
          if (*bp == '\\' && bp + 1 < blim)
            bp += 2;
          else
            bp++;
          continue;
        }

      /* At top-level outside brackets/parens: check for escaped literal.  */
      if (*bp == '\\')
        {
          char c = bp[1];
          char const *lit_escapes = (syntax_bits & RE_NO_BK_VBAR)
                                    ? "^.[$()|*+?{}\\"
                                    : "^.[$*\\";
          if (c != '\0' && strchr (lit_escapes, c) != nullptr)
            {
              if (is_quantifier (bp + 2, blim, syntax_bits))
                {
                  flush_branch_literal (best_lit, &best_len, cur_lit, &cur_len,
                                        &branch_is_pure);
                  bp = skip_quantifiers (bp + 2, blim, syntax_bits);
                }
              else
                {
                  cur_lit[cur_len++] = c;
                  bp += 2;
                }
            }
          else
            {
              flush_branch_literal (best_lit, &best_len, cur_lit, &cur_len,
                                    &branch_is_pure);
              if (is_quantifier (bp, blim, syntax_bits))
                bp = skip_quantifiers (bp, blim, syntax_bits);
              else
                {
                  idx_t esc_len = bp + 1 < blim ? 2 : 1;
                  if (esc_len == 2 && localeinfo.using_utf8
                      && (unsigned char) bp[1] >= 0x80)
                    while (bp + esc_len < blim
                           && ((unsigned char) bp[esc_len] & 0xC0) == 0x80)
                      esc_len++;
                  bp = skip_quantifiers (bp + esc_len, blim, syntax_bits);
                }
            }
          continue;
        }

      bool is_meta = (syntax_bits & RE_NO_BK_VBAR)
                     ? (*bp == '^' || *bp == '$' || *bp == '.' || *bp == '*'
                        || *bp == '+' || *bp == '?' || *bp == '{' || *bp == '}'
                        || *bp == '|')
                     : (*bp == '^' || *bp == '$' || *bp == '.' || *bp == '*');
      if (is_meta)
        {
          flush_branch_literal (best_lit, &best_len, cur_lit, &cur_len,
                                &branch_is_pure);
          if (is_quantifier (bp, blim, syntax_bits))
            bp = skip_quantifiers (bp, blim, syntax_bits);
          else
            bp = skip_quantifiers (bp + 1, blim, syntax_bits);
          continue;
        }

      /* Ordinary character (possibly multibyte UTF-8).  */
      idx_t clen = 1;
      if (localeinfo.using_utf8 && (unsigned char) *bp >= 0x80)
        {
          while (bp + clen < blim
                 && ((unsigned char) bp[clen] & 0xC0) == 0x80)
            clen++;
        }
      if (is_quantifier (bp + clen, blim, syntax_bits))
        {
          flush_branch_literal (best_lit, &best_len, cur_lit, &cur_len,
                                &branch_is_pure);
          bp = skip_quantifiers (bp + clen, blim, syntax_bits);
        }
      else
        {
          memcpy (cur_lit + cur_len, bp, clen);
          cur_len += clen;
          bp += clen;
        }
    }

  flush_branch_literal (best_lit, &best_len, cur_lit, &cur_len, nullptr);
  free (cur_lit);

  if (best_len < 2)
    {
      free (best_lit);
      return false;
    }

  out->best_lit = best_lit;
  out->best_len = best_len;
  out->is_exact = branch_is_pure && !match_words && !match_lines;
  return true;
}

static void
kwsmusts_multi (struct dfa_comp *dc, char const *raw_pat, idx_t raw_size,
                reg_syntax_t syntax_bits)
{
  idx_t nbranches = 0;
  struct branch_slice *slices
    = split_top_level_branches (raw_pat, raw_size, syntax_bits, &nbranches);
  if (!slices)
    return;

  struct branch_info *branches = xnmalloc (nbranches, sizeof *branches);
  for (idx_t i = 0; i < nbranches; i++)
    {
      if (!extract_branch_best_literal (slices[i], syntax_bits, &branches[i]))
        {
          for (idx_t j = 0; j < i; j++)
            free (branches[j].best_lit);
          free (branches);
          free (slices);
          return;
        }
    }

  dc->kwset = kwsinit (true);

  /* Pass 1: exact keywords.  */
  for (idx_t i = 0; i < nbranches; i++)
    if (branches[i].is_exact)
      {
        GREP_TEST_PREFILTER ("dfa-kws", branches[i].best_lit,
                             branches[i].best_len, "exact=1");
        kwsincr (dc->kwset, branches[i].best_lit, branches[i].best_len);
        dc->kwset_exact_matches++;
      }

  /* Pass 2: non-exact keywords.  */
  for (idx_t i = 0; i < nbranches; i++)
    if (!branches[i].is_exact)
      {
        GREP_TEST_PREFILTER ("dfa-kws", branches[i].best_lit,
                             branches[i].best_len, "exact=0");
        kwsincr (dc->kwset, branches[i].best_lit, branches[i].best_len);
      }

  kwsprep (dc->kwset);

  for (idx_t i = 0; i < nbranches; i++)
    free (branches[i].best_lit);
  free (branches);
  free (slices);
}

static void
extract_secondary_must (struct dfa_comp *dc, char const *raw_pat,
                        idx_t raw_size, reg_syntax_t syntax_bits,
                        char const *primary_must)
{
  char const *lim = raw_pat + raw_size;
  char best_lit[64];
  char cur_lit[64];
  idx_t best_len = 0, cur_len = 0;

  for (char const *bp = raw_pat; bp < lim; )
    {
      if (*bp == '[')
        {
          cur_len = 0;
          bp = skip_quantifiers (skip_bracket_expr (bp, lim, syntax_bits),
                                 lim, syntax_bits);
          continue;
        }
      bool is_meta = (*bp == '\n' || *bp == '^' || *bp == '$'
                      || *bp == '.' || *bp == '*' || *bp == '\\'
                      || ((syntax_bits & RE_NO_BK_VBAR)
                          && (*bp == '|' || *bp == '+' || *bp == '?'
                              || *bp == '{' || *bp == '}'
                              || *bp == '(' || *bp == ')')));
      idx_t clen = 1;
      if (!is_meta && localeinfo.using_utf8 && (unsigned char) *bp >= 0x80)
        {
          while (bp + clen < lim && ((unsigned char) bp[clen] & 0xC0) == 0x80)
            clen++;
        }
      if (is_meta || is_quantifier (bp + clen, lim, syntax_bits))
        {
          if (*bp == '\n'
              || ((syntax_bits & RE_NO_BK_VBAR)
                  && (*bp == '|' || *bp == '(' || *bp == ')'
                      || *bp == '{' || *bp == '}'))
              || (*bp == '\\' && bp + 1 < lim
                  && (bp[1] == '|' || bp[1] == '(' || bp[1] == ')'
                      || bp[1] == '{' || bp[1] == '}')))
            {
              best_len = cur_len = 0;
              break;
            }
          cur_len = 0;
          if (*bp == '\\' && bp + 1 < lim)
            {
              idx_t esc_len = 2;
              if (localeinfo.using_utf8 && (unsigned char) bp[1] >= 0x80)
                while (bp + esc_len < lim
                       && ((unsigned char) bp[esc_len] & 0xC0) == 0x80)
                  esc_len++;
              bp = skip_quantifiers (bp + esc_len, lim, syntax_bits);
            }
          else
            bp = skip_quantifiers (bp + clen, lim, syntax_bits);
          continue;
        }
      if (cur_len + clen < (idx_t) sizeof cur_lit)
        {
          memcpy (cur_lit + cur_len, bp, clen);
          cur_len += clen;
          cur_lit[cur_len] = '\0';
          if (cur_len >= 2 && cur_len > best_len
              && !strstr (primary_must, cur_lit))
            {
              memcpy (best_lit, cur_lit, cur_len + 1);
              best_len = cur_len;
            }
        }
      bp += clen;
    }
  if (best_len >= 2)
    {
      memcpy (dc->must2, best_lit, best_len);
      dc->must2_len = best_len;
    }
}

/* If the DFA turns out to have some set of fixed strings one of
   which must occur in the match, then we build a kwset matcher
   to find those strings, and thus quickly filter out impossible
   matches. */
static void
kwsmusts (struct dfa_comp *dc, char const *raw_pat, idx_t raw_size,
          reg_syntax_t syntax_bits)
{
  struct dfamust *dm = dfamust (dc->dfa);
  GREP_TEST_PREFILTER ("dfa-pat", raw_pat, raw_size, "dm=%d", dm != nullptr);
  if (!dm)
    {
      if (dc->pcount == 0
          && (!localeinfo.multibyte || localeinfo.using_utf8)
          && (!match_icase || !localeinfo.multibyte))
        kwsmusts_multi (dc, raw_pat, raw_size, syntax_bits);
      GREP_TEST_PREFILTER ("dfa-done", dc->must2, dc->must2_len,
                           "exact_matches=%td begline=%d",
                           dc->kwset_exact_matches, (int) dc->begline);
      return;
    }
  dc->kwset = kwsinit (false);
  if (dm->exact)
    {
      /* Prepare a substring whose presence implies a match.
         The kwset matcher will return the index of the matching
         string that it chooses. */
      ++dc->kwset_exact_matches;
      idx_t old_len = strlen (dm->must);
      idx_t new_len = old_len + dm->begline + dm->endline;
      char *must = ximalloc (new_len);
      char *mp = must;
      *mp = eolbyte;
      mp += dm->begline;
      dc->begline |= dm->begline;
      memcpy (mp, dm->must, old_len);
      if (dm->endline)
        mp[old_len] = eolbyte;
      GREP_TEST_PREFILTER ("dfa-kws", must, new_len, "exact=1");
      kwsincr (dc->kwset, must, new_len);
      free (must);
    }
  else
    {
      /* Otherwise, filtering with this substring should help reduce the
         search space, but we'll still have to use the regexp matcher.  */
      GREP_TEST_PREFILTER ("dfa-kws", dm->must, (idx_t) strlen (dm->must),
                           "exact=0");
      kwsincr (dc->kwset, dm->must, strlen (dm->must));
      if (!match_icase && (!localeinfo.multibyte || localeinfo.using_utf8))
        extract_secondary_must (dc, raw_pat, raw_size, syntax_bits, dm->must);
    }
  kwsprep (dc->kwset);
  dfamustfree (dm);
  GREP_TEST_PREFILTER ("dfa-done", dc->must2, dc->must2_len,
                       "exact_matches=%td begline=%d",
                       dc->kwset_exact_matches, (int) dc->begline);
}

/* Return true if KEYS, of length LEN, might contain a back-reference.
   Return false if KEYS cannot contain a back-reference.
   BS_SAFE is true of encodings where a backslash cannot appear as the
   last byte of a multibyte character.  */
static bool _GL_ATTRIBUTE_PURE
possible_backrefs_in_pattern (char const *keys, idx_t len, bool bs_safe)
{
  /* Normally a backslash, but in an unsafe encoding this is a non-char
     value so that the comparison below always fails, because if there
     are two adjacent '\' bytes, the first might be the last byte of a
     multibyte character.  */
  int second_backslash = bs_safe ? '\\' : CHAR_MAX + 1;

  /* This code can return true even if KEYS lacks a back-reference, for
     patterns like [\2], or for encodings where '\' appears as the last
     byte of a multibyte character.  However, false alarms should be
     rare and do not affect correctness.  */

  /* Do not look for a backslash in the pattern's last byte, since it
     can't be part of a back-reference and this streamlines the code.  */
  len--;

  if (0 <= len)
    {
      char const *lim = keys + len;
      for (char const *p = keys; (p = memchr (p, '\\', lim - p)); p++)
        {
          if ('1' <= p[1] && p[1] <= '9')
            return true;
          if (p[1] == second_backslash)
            {
              p++;
              if (p == lim)
                break;
            }
        }
    }
  return false;
}

/* Locate the first top-level capture group [G1_START_OUT, G1_END_OUT) in
   [P, LIM) and verify that neither the top-level pattern nor group 1 contains
   alternation or nested groups.  Return true on success.  */
static bool
find_first_capture_group (char const *p, char const *lim,
                          reg_syntax_t syntax_bits,
                          char const **g1_start_out, char const **g1_end_out)
{
  bool no_bk_parens = (syntax_bits & RE_NO_BK_PARENS) != 0;
  int depth = 0;
  char const *g1_start = nullptr;
  char const *g1_end = nullptr;

  for (char const *s = p; s < lim; s++)
    {
      if (*s == '[')
        {
          s = skip_bracket_expr (s, lim, syntax_bits) - 1;
          continue;
        }
      if (*s == '\\')
        {
          s++;
          if (!no_bk_parens)
            {
              if (*s == '(')
                {
                  if (depth == 0 && !g1_start)
                    g1_start = s + 1;
                  depth++;
                }
              else if (*s == ')')
                {
                  depth--;
                  if (depth == 0 && g1_start && !g1_end)
                    g1_end = s - 1;
                }
              else if (*s == '|' && depth == 0)
                return false;
            }
          continue;
        }

      if (no_bk_parens)
        {
          if (*s == '(')
            {
              if (depth == 0 && !g1_start)
                g1_start = s + 1;
              depth++;
            }
          else if (*s == ')')
            {
              if (depth <= 0)
                return false;
              depth--;
              if (depth == 0 && g1_start && !g1_end)
                g1_end = s;
            }
          else if (*s == '|' && depth == 0)
            return false;
        }
    }

  if (depth != 0 || !g1_start || !g1_end)
    return false;

  for (char const *s = g1_start; s < g1_end; s++)
    {
      if (*s == '[')
        {
          s = skip_bracket_expr (s, g1_end, syntax_bits) - 1;
          continue;
        }
      if (*s == '\\')
        {
          s++;
          if (!no_bk_parens && (*s == '(' || *s == ')' || *s == '|'))
            return false;
          continue;
        }
      if (no_bk_parens && (*s == '(' || *s == ')' || *s == '|'))
        return false;
    }

  *g1_start_out = g1_start;
  *g1_end_out = g1_end;
  return true;
}

static bool _GL_ATTRIBUTE_PURE
is_backref_quantifier_start (char const *s, char const *lim, bool no_bk_parens)
{
  if (s >= lim)
    return false;
  if (*s == '*')
    return true;
  if (no_bk_parens)
    return *s == '?' || *s == '+' || *s == '{';
  return (*s == '\\' && s + 1 < lim
          && (s[1] == '?' || s[1] == '+' || s[1] == '{'));
}

/* Parse an optional quantifier starting at *SP (bounded by G1_END) and store
   its minimum repetition count in *QMIN_OUT.  Return false if a malformed or
   chained quantifier is encountered.  */
static bool
parse_group_quantifier (char const **sp, char const *g1_end,
                        reg_syntax_t syntax_bits, idx_t *qmin_out)
{
  bool no_bk_parens = (syntax_bits & RE_NO_BK_PARENS) != 0;
  char const *s = *sp;
  idx_t qmin = 1;
  bool had_quant = false;

  if (s < g1_end)
    {
      if (*s == '*')
        {
          qmin = 0;
          had_quant = true;
          s++;
        }
      else if (no_bk_parens && (*s == '?' || *s == '+'))
        {
          qmin = (*s == '+') ? 1 : 0;
          had_quant = true;
          s++;
        }
      else if (!no_bk_parens && *s == '\\' && s + 1 < g1_end
               && (s[1] == '?' || s[1] == '+'))
        {
          qmin = (s[1] == '+') ? 1 : 0;
          had_quant = true;
          s += 2;
        }
      else if ((no_bk_parens && *s == '{')
               || (!no_bk_parens && *s == '\\' && s + 1 < g1_end && s[1] == '{'))
        {
          if (!is_dfa_brace_quantifier (s, g1_end, syntax_bits))
            return false;
          s += no_bk_parens ? 1 : 2;
          qmin = 0;
          while (s < g1_end && *s >= '0' && *s <= '9')
            {
              if (qmin > 255)
                return false;
              qmin = qmin * 10 + (*s - '0');
              s++;
            }
          if (s < g1_end && *s == ',')
            {
              s++;
              while (s < g1_end && *s >= '0' && *s <= '9')
                s++;
            }
          s += no_bk_parens ? 1 : 2;
          had_quant = true;
        }

      if (had_quant && is_backref_quantifier_start (s, g1_end, no_bk_parens))
        return false;
    }

  *sp = s;
  *qmin_out = qmin;
  return true;
}

static void
analyze_backref_filter (char const *p, idx_t len, reg_syntax_t syntax_bits,
                        struct backref_filter *bf)
{
  bf->enabled = false;
  bf->min_len = 0;
  bf->sep_len = 0;

  if (localeinfo.multibyte && !localeinfo.using_utf8)
    return;

  bool no_bk_parens = (syntax_bits & RE_NO_BK_PARENS) != 0;
  char const *lim = p + len;
  char const *g1_start = nullptr;
  char const *g1_end = nullptr;

  if (!find_first_capture_group (p, lim, syntax_bits, &g1_start, &g1_end))
    return;

  idx_t min_len = 0;
  char const *s = g1_start;
  while (s < g1_end)
    {
      idx_t atom_base = 1;
      if (*s == '[')
        {
          s = skip_bracket_expr (s, g1_end, syntax_bits);
        }
      else if (*s == '\\')
        {
          s++;
          if (s >= g1_end)
            return;
          if ((unsigned char) *s >= 0x80
              || *s == '+' || *s == '?' || *s == '{' || *s == '}'
              || *s == '(' || *s == ')' || *s == '|'
              || (*s >= '1' && *s <= '9'))
            return;
          if (*s == 'b' || *s == 'B' || *s == '<' || *s == '>'
              || *s == '\'' || *s == '`')
            atom_base = 0;
          s++;
        }
      else if (*s == '^' || *s == '$')
        {
          atom_base = 0;
          s++;
        }
      else if (*s == '*' || *s == '+' || *s == '?'
               || *s == '{' || *s == '}'
               || *s == '(' || *s == ')' || *s == '|')
        return;
      else
        {
          char const *atom_start = s;
          if (localeinfo.using_utf8 && (unsigned char) atom_start[0] >= 0x80)
            {
              mbstate_t mbs = { 0 };
              size_t clen = mbrlen (s, g1_end - s, &mbs);
              if (clen == (size_t) -1 || clen == (size_t) -2 || clen == 0)
                return;
              s += clen;
            }
          else
            s++;
          atom_base = (*atom_start == '.') ? 1 : (s - atom_start);
        }

      idx_t qmin = 1;
      if (!parse_group_quantifier (&s, g1_end, syntax_bits, &qmin))
        return;
      min_len += atom_base * qmin;
    }

  bf->min_len = min_len;

  char const *after_g = no_bk_parens ? (g1_end + 1) : (g1_end + 2);
  idx_t sep_len = 0;
  while (after_g + sep_len < lim && sep_len < 8)
    {
      char c = after_g[sep_len];
      if (c == '^' || c == '$' || c == '.' || c == '[' || c == ']'
          || c == '*' || c == '+' || c == '?' || c == '(' || c == ')'
          || c == '{' || c == '}' || c == '|' || c == '\\')
        break;
      bf->sep[sep_len] = c;
      sep_len++;
    }
  bf->sep_len = sep_len;

  char const *bref = after_g + sep_len;
  if (bref + 1 >= lim || bref[0] != '\\' || bref[1] != '1')
    return;

  if (is_backref_quantifier_start (bref + 2, lim, no_bk_parens))
    return;

  if (bf->min_len >= 2 || (bf->sep_len >= 1 && bf->min_len >= 1))
    bf->enabled = true;
}

static bool _GL_ATTRIBUTE_PURE
backref_line_can_match (struct backref_filter const *bf, char const *line, idx_t len)
{
  if (len < 2 * bf->min_len + bf->sep_len)
    return false;

  if (bf->sep_len > 0)
    {
      char const *lim = line + len - bf->sep_len - bf->min_len;
      char const *pos = line + bf->min_len;

      while (pos <= lim)
        {
          pos = memchr (pos, (unsigned char) bf->sep[0], lim - pos + 1);
          if (!pos)
            break;

          if (bf->sep_len > 1 && memcmp (pos + 1, bf->sep + 1, bf->sep_len - 1) != 0)
            {
              pos++;
              continue;
            }

          char const *after = pos + bf->sep_len;
          idx_t rem = line + len - after;
          char const *q_lim = pos - bf->min_len;
          char const *q_min = pos - rem;
          char const *q = q_min > line ? q_min : line;

          while (q <= q_lim)
            {
              q = memchr (q, (unsigned char) after[0], q_lim - q + 1);
              if (!q)
                break;
              idx_t k = pos - q;
              if (after[k - 1] == pos[-1] && memcmp (q, after, k) == 0)
                return true;
              q++;
            }

          pos++;
        }
      return false;
    }

  char const *pos;
  char const *lim = line + len - bf->min_len;
  for (pos = line + bf->min_len; pos <= lim; pos++)
    {
      char const *after = pos;
      char const *q_lim = pos - bf->min_len;
      char const *q = line;
      while (q <= q_lim)
        {
          q = memchr (q, (unsigned char) after[0], q_lim - q + 1);
          if (!q)
            break;
          idx_t k = pos - q;
          if (after + k <= line + len && memcmp (q, after, k) == 0)
            return true;
          q++;
        }
    }
  return false;
}

static bool
regex_compile (struct dfa_comp *dc, char const *p, idx_t len,
               idx_t pcount, idx_t lineno, reg_syntax_t syntax_bits,
               bool syntax_only)
{
  struct re_pattern_buffer pat;
  pat.buffer = nullptr;
  pat.allocated = 0;

  /* Do not use a fastmap with -i, to work around glibc Bug#20381.  */
  static_assert (UCHAR_MAX < IDX_MAX);
  idx_t uchar_max = UCHAR_MAX;
  pat.fastmap = syntax_only | match_icase ? nullptr : ximalloc (uchar_max + 1);

  pat.translate = nullptr;

  if (syntax_only)
    re_set_syntax (syntax_bits | RE_NO_SUB);
  else
    re_set_syntax (syntax_bits);

  char const *err = re_compile_pattern (p, len, &pat);
  if (!err)
    {
      if (syntax_only)
        regfree (&pat);
      else
        dc->patterns[pcount] = pat;

      return true;
    }

  free (pat.fastmap);

  /* Emit a filename:lineno: prefix for patterns taken from files.  */
  idx_t pat_lineno;
  char const *pat_filename = pattern_file_name (lineno, &pat_lineno);

  if (*pat_filename == '\0')
    error (0, 0, "%s", err);
  else
    {
      ptrdiff_t n = pat_lineno;
      error (0, 0, "%s:%td: %s", pat_filename, n, err);
    }

  return false;
}

/* Split PATTERN[0..SIZE-1] into lines, compiling each line with GNU regex
   (either for syntax checking or backreference execution) and accumulating
   backreference-free lines into *BUF_OUT.  Return false if any pattern line
   fails to compile.  */
static bool
compile_pattern_lines (struct dfa_comp *dc, char *pattern, idx_t size,
                       reg_syntax_t syntax_bits, bool bs_safe,
                       char **buf_out, idx_t *buflen_out, idx_t *bufalloc_out)
{
  char const *p = pattern;
  char const *patlim = pattern + size;
  bool compilation_failed = false;

  dc->patterns_alloc = xmalloc (sizeof *dc->patterns);
  dc->patterns = dc->patterns_alloc + 1;
  dc->bref_filters_alloc = xcalloc (1, sizeof *dc->bref_filters);
  dc->bref_filters = dc->bref_filters_alloc + 1;
  dc->pcount = 0;
  idx_t palloc = 1;

  char const *prev = pattern;
  char *buf = nullptr;
  idx_t buflen = 0;
  idx_t bufalloc = 0;
  idx_t lineno = 0;

  do
    {
      char const *sep = rawmemchr (p, '\n');
      idx_t len = sep - p;
      bool backref = possible_backrefs_in_pattern (p, len, bs_safe);

      if (backref && prev < p)
        {
          idx_t prevlen = p - prev;
          ptrdiff_t bufshortage = buflen - bufalloc + prevlen;
          if (0 < bufshortage)
            buf = xpalloc (buf, &bufalloc, bufshortage, -1, 1);
          memcpy (buf + buflen, prev, prevlen);
          buflen += prevlen;
        }

      /* Ensure room for at least two more patterns.  The extra one is
         for the regex_compile that may be executed after this loop
         exits, and its (unused) slot is patterns[-1] until then.  */
      ptrdiff_t shortage = dc->pcount - palloc + 2;
      if (0 < shortage)
        {
          dc->patterns_alloc = xpalloc (dc->patterns_alloc, &palloc, shortage,
                                        -1, sizeof *dc->patterns);
          dc->patterns = dc->patterns_alloc + 1;
          dc->bref_filters_alloc = xrealloc (dc->bref_filters_alloc,
                                             palloc * sizeof *dc->bref_filters);
          memset (dc->bref_filters_alloc + (dc->pcount + 1), 0,
                  (palloc - (dc->pcount + 1)) * sizeof *dc->bref_filters);
          dc->bref_filters = dc->bref_filters_alloc + 1;
        }

      bool ok = true;
      if (!suppress_dfawarn || backref)
        {
          if (!regex_compile (dc, p, len, dc->pcount, lineno, syntax_bits,
                              !backref))
            {
              compilation_failed = true;
              ok = false;
            }
        }

      char const *pat_line = p;
      p = sep + 1;
      lineno++;

      if (backref)
        {
          if (ok)
            {
              if (!match_icase)
                {
                  analyze_backref_filter (pat_line, len, syntax_bits,
                                          &dc->bref_filters[dc->pcount]);
#ifdef GREP_TEST_HOOKS
                  struct backref_filter *bf = &dc->bref_filters[dc->pcount];
                  GREP_TEST_PREFILTER ("backref-pat", pat_line, len,
                                       "enabled=%d min_len=%td sep_len=%td",
                                       (int) bf->enabled, bf->min_len,
                                       bf->sep_len);
                  GREP_TEST_PREFILTER ("backref-sep", bf->sep, bf->sep_len,
                                       "enabled=%d", (int) bf->enabled);
#endif
                }
              dc->pcount++;
            }
          prev = p;
        }
    }
  while (p <= patlim);

  if (compilation_failed)
    {
      free (buf);
      return false;
    }

  if (patlim < prev)
    buflen--;
  else if (pattern < prev)
    {
      idx_t prevlen = patlim - prev;
      buf = xirealloc (buf, buflen + prevlen);
      memcpy (buf + buflen, prev, prevlen);
      buflen += prevlen;
    }
  else
    {
      buf = pattern;
      buflen = size;
    }

  *buf_out = buf;
  *buflen_out = buflen;
  *bufalloc_out = bufalloc;
  return true;
}

/* Wrap PATTERN[0..SIZE-1] in word-boundary or line-boundary regex anchors
   for the DFA matcher when -w or -x is active.  Store the new length in
   *SIZE_OUT and return the newly allocated motif buffer.  */
static char *
wrap_word_or_line_motif (char const *pattern, idx_t size,
                         reg_syntax_t syntax_bits, idx_t *size_out)
{
  static char const line_beg_no_bk[] = "^(";
  static char const line_end_no_bk[] = ")$";
  static char const word_beg_no_bk[] = "(^|[^[:alnum:]_])(";
  static char const word_end_no_bk[] = ")([^[:alnum:]_]|$)";
  static char const line_beg_bk[] = "^\\(";
  static char const line_end_bk[] = "\\)$";
  static char const word_beg_bk[] = "\\(^\\|[^[:alnum:]_]\\)\\(";
  static char const word_end_bk[] = "\\)\\([^[:alnum:]_]\\|$\\)";

  int bk = !(syntax_bits & RE_NO_BK_PARENS);
  idx_t bracket_bytes = sizeof word_beg_bk - 1 + sizeof word_end_bk;
  char *n = ximalloc (size + bracket_bytes);

  strcpy (n, match_lines ? (bk ? line_beg_bk : line_beg_no_bk)
                         : (bk ? word_beg_bk : word_beg_no_bk));
  idx_t total = strlen (n);
  memcpy (n + total, pattern, size);
  total += size;
  strcpy (n + total, match_lines ? (bk ? line_end_bk : line_end_no_bk)
                                 : (bk ? word_end_bk : word_end_no_bk));
  total += strlen (n + total);
  *size_out = total;
  return n;
}

/* Attach the shared must-literal kwset to DC (populating dfa_kwset_cache on
   first compilation) and compile or defer DC->dfa.  */
static void
setup_shared_dfa_and_kwset (struct dfa_comp *dc, char const *pattern,
                            idx_t size, char const *raw_pat, idx_t raw_size,
                            reg_syntax_t syntax_bits, bool exact)
{
  struct shared_dfa_kwset *sk = dfa_kwset_cache;
  if (sk && sk->size == size && memcmp (sk->pattern, pattern, size) == 0
      && sk->match_icase == match_icase && sk->match_words == match_words
      && sk->match_lines == match_lines && sk->exact == exact
      && sk->eolbyte == eolbyte && sk->syntax_bits == syntax_bits)
    {
      dc->shared_kwset = sk;
      dc->kwset = sk->kwset;
      dc->kwset_exact_matches = sk->kwset_exact_matches;
      dc->begline = sk->begline;
      dc->must2_len = sk->must2_len;
      if (sk->must2_len)
        memcpy (dc->must2, sk->must2, sk->must2_len);
      atomic_fetch_add (&sk->refs, 1);

      if (suppress_dfawarn && sk->dfa_supported && dc->pcount == 0)
        {
          dc->dfa_compiled = false;
          dc->dfa_pattern = sk->pattern;
          dc->dfa_pattern_size = sk->size;
        }
      else
        {
          dfaparse (pattern, size, dc->dfa);
          dfacomp (nullptr, 0, dc->dfa, 1);
          dc->dfa_compiled = true;
        }
      return;
    }

  dfaparse (pattern, size, dc->dfa);
  kwsmusts (dc, raw_pat, raw_size, syntax_bits);
  dfacomp (nullptr, 0, dc->dfa, 1);
  dc->dfa_compiled = true;

  if (!dfa_kwset_cache)
    {
      sk = xmalloc (sizeof *sk);
      sk->kwset = dc->kwset;
      sk->kwset_exact_matches = dc->kwset_exact_matches;
      sk->begline = dc->begline;
      sk->must2_len = dc->must2_len;
      if (dc->must2_len)
        memcpy (sk->must2, dc->must2, dc->must2_len);
      sk->pattern = ximemdup (pattern, size);
      sk->size = size;
      sk->match_icase = match_icase;
      sk->match_words = match_words;
      sk->match_lines = match_lines;
      sk->exact = exact;
      sk->eolbyte = eolbyte;
      sk->syntax_bits = syntax_bits;
      sk->dfa_supported = dfasupported (dc->dfa);
      atomic_init (&sk->refs, 1);
      dfa_kwset_cache = sk;
      dc->shared_kwset = sk;
    }
}

/* Compile PATTERN, containing SIZE bytes that are followed by '\n'.
   SYNTAX_BITS specifies whether PATTERN uses style -G, -E, or -A.
   Return a description of the compiled pattern.  */

void *
GEAcompile (char *pattern, idx_t size, reg_syntax_t syntax_bits,
            bool exact)
{
  struct dfa_comp *dc = xcalloc (1, sizeof (*dc));
  dc->dfa = dfaalloc ();

  if (match_icase)
    syntax_bits |= RE_ICASE;
  int dfaopts = (DFA_CONFUSING_BRACKETS_ERROR | DFA_STRAY_BACKSLASH_WARN
                 | DFA_PLUS_WARN
                 | (syntax_bits & RE_CONTEXT_INDEP_OPS ? DFA_STAR_WARN : 0)
                 | (eolbyte ? 0 : DFA_EOL_NUL));
  dfasyntax (dc->dfa, &localeinfo, syntax_bits, dfaopts);
  bool bs_safe = !localeinfo.multibyte | localeinfo.using_utf8;

  char *buf = nullptr;
  idx_t buflen = 0;
  idx_t bufalloc = 0;
  if (!compile_pattern_lines (dc, pattern, size, syntax_bits, bs_safe,
                              &buf, &buflen, &bufalloc))
    {
      EGfree (dc);
      exit (EXIT_TROUBLE);
    }

  char const *raw_pat = pattern;
  idx_t raw_size = size;
  char *motif = nullptr;
  if (match_words || match_lines)
    pattern = motif = wrap_word_or_line_motif (pattern, size, syntax_bits,
                                               &size);

  setup_shared_dfa_and_kwset (dc, pattern, size, raw_pat, raw_size,
                              syntax_bits, exact);

  if (buf)
    {
      bool dfa_ok = (dc->shared_kwset
                     ? dc->shared_kwset->dfa_supported
                     : dfasupported (dc->dfa));
      if (exact || !dfa_ok)
        {
          dc->patterns--;
          dc->bref_filters--;
          dc->pcount++;
          dc->bref_filters[0].enabled = false;

          if (!regex_compile (dc, buf, buflen, 0, -1, syntax_bits, false))
            abort ();
        }

      if (bufalloc != 0)
        free (buf);
    }

  free (motif);
  return dc;
}

static char *
safe_dfaexec (struct dfa *d, char const *begin, char *end,
              char const *buflim _GL_UNUSED, idx_t *count, bool *backref)
{
  /* END always points one byte past an eolbyte (either buflim or
     rawmemchr (..., eol) + 1), and BEGIN < END.  Passing END - 1 as
     the end pointer makes *(END - 1) == eolbyte in dfaexec_main, so
     dfaexec never writes a sentinel into shared read-only memory and
     needs zero buffer copies.  Since allow_nl is false, eolbyte never
     transitions into a pattern match; any match in [BEGIN, END) is
     detected at p <= END - 1, and scanning stops as soon as p
     consumes *(END - 1) (p == END > END - 1).  */
  return dfaexec (d, begin, end - 1, 0, count, backref);
}

enum dfa_narrow_status
{
  DFA_NARROW_NO_MATCH,
  DFA_NARROW_NEXT_LINE,
  DFA_NARROW_EXACT_LINE,
  DFA_NARROW_NEEDS_REGEX
};

/* Narrow the search range [*BEG_INOUT, BUFLIM) to a single candidate line
   [*BEG_INOUT, *END_INOUT) using the 3-stage pipeline:
   (1) kwset must-literal pre-filter + optional must2 check,
   (2) superset DFA fast scan, and
   (3) full DFA scan.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE enum dfa_narrow_status
dfa_narrow_candidate_line (struct dfa_comp *dc, char const *buf,
                           char const *buflim, char const **beg_inout,
                           char const **end_inout, char const **mb_start_inout)
{
  char eol = eolbyte;
  char const *beg = *beg_inout;
  char const *end = *end_inout;
  char const *dfa_beg = beg;
  idx_t count = 0;
  bool exact_kwset_match = false;
  bool backref = false;

  if (dc->kwset)
    {
      struct kwsmatch kwsm;
      ptrdiff_t offset = kwsexec (dc->kwset, beg - dc->begline,
                                  buflim - beg + dc->begline, &kwsm, true);
      if (offset < 0)
        return DFA_NARROW_NO_MATCH;
      char const *match = beg + offset;

      beg = memrchr (buf, eol, match - buf);
      beg = beg ? beg + 1 : buf;
      dfa_beg = beg;
      end = rawmemchr (match, eol);
      end++;

      exact_kwset_match = kwsm.index < dc->kwset_exact_matches;
      if (exact_kwset_match)
        {
          if (!localeinfo.multibyte | localeinfo.using_utf8)
            {
              *beg_inout = beg;
              *end_inout = end;
              return DFA_NARROW_EXACT_LINE;
            }
          if (*mb_start_inout < beg)
            *mb_start_inout = beg;
          if (mb_goback (mb_start_inout, nullptr, match, buflim) == 0)
            {
              *beg_inout = beg;
              *end_inout = end;
              return DFA_NARROW_EXACT_LINE;
            }
          dfa_beg = *mb_start_inout;
        }
      else if (dc->must2_len
               && !memmem (beg, end - 1 - beg, dc->must2, dc->must2_len))
        {
          *beg_inout = beg;
          *end_inout = end;
          return DFA_NARROW_NEXT_LINE;
        }
    }

  if (!dc->dfa_compiled)
    {
      suppress_dfawarn = true;
      dfaparse (dc->dfa_pattern, dc->dfa_pattern_size, dc->dfa);
      dfacomp (nullptr, 0, dc->dfa, 1);
      suppress_dfawarn = false;
      dc->dfa_compiled = true;
    }

  struct dfa *superset = dfasuperset (dc->dfa);
  if (superset && !exact_kwset_match)
    {
      char const *next_beg = safe_dfaexec (superset, dfa_beg, (char *) end,
                                           buflim, &count, nullptr);
      if (!next_beg)
        {
          *beg_inout = beg;
          *end_inout = end;
          return DFA_NARROW_NEXT_LINE;
        }
      if (count != 0)
        {
          beg = memrchr (buf, eol, next_beg - buf);
          beg++;
          dfa_beg = beg;
        }
      end = rawmemchr (next_beg, eol);
      end++;
      count = 0;
    }

  char const *next_beg = safe_dfaexec (dc->dfa, dfa_beg, (char *) end, buflim,
                                       &count, &backref);
  if (!next_beg)
    {
      *beg_inout = beg;
      *end_inout = end;
      return DFA_NARROW_NEXT_LINE;
    }
  if (count != 0)
    {
      beg = memrchr (buf, eol, next_beg - buf);
      beg++;
    }
  end = rawmemchr (next_beg, eol);
  end++;

  *beg_inout = beg;
  *end_inout = end;
  return backref ? DFA_NARROW_NEEDS_REGEX : DFA_NARROW_EXACT_LINE;
}

/* Refine a GNU regex match [*MATCH_INOUT, *MATCH_INOUT + *LEN_INOUT) within
   line [BEG, END - 1) until it aligns with -w word boundaries.  Return true
   if a word-bounded match is found at or before BEST_MATCH.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE bool
regex_shrink_to_word_boundaries (struct dfa_comp *dc, idx_t pat_idx,
                                 char const *beg, char const *end,
                                 char const *ptr, char const *best_match,
                                 char const **match_inout, idx_t *len_inout)
{
  char const *match = *match_inout;
  idx_t len = *len_inout;

  while (match <= best_match)
    {
      regoff_t shorter_len = 0;
      if (!wordchar_next (match + len, end - 1)
          && !wordchar_prev (beg, match, end - 1))
        {
          *match_inout = match;
          *len_inout = len;
          return true;
        }
      if (len > 0)
        {
          --len;
          dc->patterns[pat_idx].not_eol = 1;
          shorter_len = re_match (&dc->patterns[pat_idx], beg,
                                  match + len - ptr, match - beg, &dc->regs);
          if (shorter_len < -1)
            xalloc_die ();
        }
      if (0 < shorter_len)
        len = shorter_len;
      else
        {
          if (match == end - 1)
            break;
          match++;
          dc->patterns[pat_idx].not_eol = 0;
          regoff_t start = re_search (&dc->patterns[pat_idx], beg,
                                      end - beg - 1, match - beg,
                                      end - match - 1, &dc->regs);
          if (start < 0)
            {
              if (start < -1)
                xalloc_die ();
              break;
            }
          len = dc->regs.end[0] - start;
          match = beg + start;
        }
    }
  return false;
}

ptrdiff_t
EGexecute (void *vdc, char const *buf, idx_t size, idx_t *match_size,
           char const *start_ptr)
{
  char const *buflim, *beg, *end, *ptr, *match, *best_match, *mb_start;
  regoff_t start;
  idx_t len, best_len;
  idx_t i;
  struct dfa_comp *dc = vdc;

  mb_start = buf;
  buflim = buf + size;

  for (beg = end = buf; end < buflim; beg = end)
    {
      end = buflim;

      if (!start_ptr)
        {
          enum dfa_narrow_status status
            = dfa_narrow_candidate_line (dc, buf, buflim, &beg, &end,
                                         &mb_start);
          if (status == DFA_NARROW_NO_MATCH)
            return -1;
          if (status == DFA_NARROW_NEXT_LINE)
            continue;
          if (status == DFA_NARROW_EXACT_LINE)
            goto success;
          ptr = beg;
        }
      else
        ptr = start_ptr;

      /* If the "line" is longer than the maximum regexp offset,
         die as if we've run out of memory.  */
      if (TYPE_MAXIMUM (regoff_t) < end - beg - 1)
        xalloc_die ();

      /* Run the possible match through Regex.  */
      best_match = end;
      best_len = 0;
      for (i = 0; i < dc->pcount; i++)
        {
          if (dc->bref_filters && dc->bref_filters[i].enabled
              && !backref_line_can_match (&dc->bref_filters[i], beg,
                                          end - beg - 1))
            continue;

          dc->patterns[i].not_eol = 0;
          dc->patterns[i].newline_anchor = eolbyte == '\n';
          if (dc->regs.num_regs > 0)
            dc->patterns[i].regs_allocated = REGS_REALLOCATE;
          start = re_search (&dc->patterns[i], beg, end - beg - 1,
                             ptr - beg, end - ptr - 1, &dc->regs);
          if (start < -1)
            xalloc_die ();
          else if (0 <= start)
            {
              len = dc->regs.end[0] - start;
              match = beg + start;
              if (match > best_match)
                continue;
              if (start_ptr && !match_words)
                goto assess_pattern_match;
              if ((!match_lines && !match_words)
                  || (match_lines && len == end - ptr - 1))
                {
                  match = ptr;
                  len = end - ptr;
                  goto assess_pattern_match;
                }
              if (!match_lines && match_words
                  && regex_shrink_to_word_boundaries (dc, i, beg, end, ptr,
                                                      best_match, &match,
                                                      &len))
                goto assess_pattern_match;
              continue;

            assess_pattern_match:
              if (!start_ptr)
                goto success;
              if (match < best_match || (match == best_match && len > best_len))
                {
                  best_match = match;
                  best_len = len;
                }
            }
        }
      if (best_match < end)
        {
          beg = best_match;
          len = best_len;
          goto success_in_len;
        }
    }

  return -1;

 success:
  len = end - beg;
 success_in_len:;
  *match_size = len;
  return beg - buf;
}

void
EGfree (void *vdc)
{
  if (!vdc)
    return;
  struct dfa_comp *dc = vdc;
  if (dc->dfa)
    {
      dfafree (dc->dfa);
      free (dc->dfa);
    }
  if (dc->bref_filters_alloc)
    free (dc->bref_filters_alloc);
  if (dc->shared_kwset)
    {
      struct shared_dfa_kwset *sk = dc->shared_kwset;
      if (atomic_fetch_sub (&sk->refs, 1) == 1)
        {
          if (dfa_kwset_cache == sk)
            dfa_kwset_cache = nullptr;
          if (sk->kwset)
            kwsfree (sk->kwset);
          free (sk->pattern);
          free (sk);
        }
    }
  if (dc->patterns_alloc)
    {
      for (idx_t i = 0; i < dc->pcount; i++)
        {
          regfree (&dc->patterns[i]);
          free (dc->patterns[i].fastmap);
        }
      free (dc->patterns_alloc);
    }
  free (dc->regs.start);
  free (dc->regs.end);
  free (dc);
}
