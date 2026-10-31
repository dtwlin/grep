/* dfa-prefilter.c - compile-time literal and back-reference prefilters.
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

#include <config.h>

#include "dfa-prefilter.h"

#include <stdlib.h>
#include <wchar.h>

#include "xalloc.h"

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
      if (lim - p < 2 || p[0] != '\\' || p[1] != '{')
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
    return lim - p >= 2 && p[0] == '\\' && p[1] == '}';
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
      if (lim - p >= 2 && *p == '\\')
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
          if (lim - p >= 2 && *p == '\\')
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

/* Advance S past a bracket expression '[...]' bounded by LIM.  */
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
          while (lim - q >= 2 && *q != ']'
                 && !(q[0] == ctype && q[1] == ']'))
            q++;
          if (lim - q >= 2 && q[0] == ctype && q[1] == ']')
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

/* Split RAW_PAT (of length RAW_SIZE) into top-level alternation branches.
   Return a newly allocated array of at least 2 slices and store its length
   in *NBRANCHES_OUT, or return nullptr if RAW_PAT cannot be split cleanly.  */
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
              p += lim - p >= 2 ? 2 : 1;
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
              p += lim - p >= 2 ? 2 : 1;
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
                             reg_syntax_t syntax_bits, bool utf8,
                             bool words_or_lines,
                             struct prefilter_branch *out)
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
          char c = bp + 1 < blim ? bp[1] : '\0';
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
                  if (esc_len == 2 && utf8
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
      if (utf8 && (unsigned char) *bp >= 0x80)
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

  out->lit = best_lit;
  out->len = best_len;
  out->exact = branch_is_pure && !words_or_lines;
  return true;
}

ptrdiff_t
prefilter_branch_literals (char const *pat, idx_t size,
                           reg_syntax_t syntax_bits, bool utf8,
                           bool words_or_lines,
                           struct prefilter_branch **out)
{
  *out = nullptr;
  idx_t nbranches = 0;
  struct branch_slice *slices
    = split_top_level_branches (pat, size, syntax_bits, &nbranches);
  if (!slices)
    return -1;

  struct prefilter_branch *branches = xnmalloc (nbranches, sizeof *branches);
  for (idx_t i = 0; i < nbranches; i++)
    if (!extract_branch_best_literal (slices[i], syntax_bits, utf8,
                                      words_or_lines, &branches[i]))
      {
        prefilter_branches_free (branches, i);
        free (slices);
        return -1;
      }

  free (slices);
  *out = branches;
  return nbranches;
}

void
prefilter_branches_free (struct prefilter_branch *branches, ptrdiff_t n)
{
  for (ptrdiff_t i = 0; i < n; i++)
    free (branches[i].lit);
  free (branches);
}

idx_t
prefilter_secondary_must (char const *pat, idx_t size,
                          reg_syntax_t syntax_bits, bool utf8,
                          char const *primary, char must2[64])
{
  char const *lim = pat + size;
  char best_lit[64];
  char cur_lit[64];
  idx_t best_len = 0, cur_len = 0;

  for (char const *bp = pat; bp < lim; )
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
      if (!is_meta && utf8 && (unsigned char) *bp >= 0x80)
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
              if (utf8 && (unsigned char) bp[1] >= 0x80)
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
              && !strstr (primary, cur_lit))
            {
              memcpy (best_lit, cur_lit, cur_len + 1);
              best_len = cur_len;
            }
        }
      bp += clen;
    }
  if (best_len >= 2)
    {
      memcpy (must2, best_lit, best_len);
      return best_len;
    }
  return 0;
}

/* Locate the first top-level capture group [G1_START_OUT, G1_END_OUT) in
   [P, LIM) and verify that neither the top-level pattern nor group 1
   contains alternation or nested groups.  Return true on success.  */
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
          if (s >= lim)
            break;
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
          if (s >= g1_end)
            break;
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
  return (*s == '\\' && lim - s >= 2
          && (s[1] == '?' || s[1] == '+' || s[1] == '{'));
}

/* Parse an optional quantifier starting at *SP (bounded by G1_END) and
   store its minimum repetition count in *QMIN_OUT.  Return false if a
   malformed or chained quantifier is encountered.  */
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
               || (!no_bk_parens && *s == '\\'
                   && s + 1 < g1_end && s[1] == '{'))
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

void
prefilter_backref_filter (char const *pat, idx_t len,
                          reg_syntax_t syntax_bits, bool utf8,
                          struct backref_filter *bf)
{
  bf->enabled = false;
  bf->min_len = 0;
  bf->sep_len = 0;

  if (!utf8 && MB_CUR_MAX > 1)
    return;

  bool no_bk_parens = (syntax_bits & RE_NO_BK_PARENS) != 0;
  char const *lim = pat + len;
  char const *g1_start = nullptr;
  char const *g1_end = nullptr;

  if (!find_first_capture_group (pat, lim, syntax_bits, &g1_start, &g1_end))
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
          if (utf8 && (unsigned char) atom_start[0] >= 0x80)
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
  if (lim - bref < 2 || bref[0] != '\\' || bref[1] != '1')
    return;

  if (is_backref_quantifier_start (bref + 2, lim, no_bk_parens))
    return;

  if (bf->min_len >= 2 || (bf->sep_len >= 1 && bf->min_len >= 1))
    bf->enabled = true;
}
