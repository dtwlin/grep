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
  bool begline;
  atomic_ptrdiff_t refs;
  char *pattern;
  idx_t size;
  bool match_icase, match_words, match_lines, exact;
  char eolbyte;
  reg_syntax_t syntax_bits;
  bool dfa_supported;
  bool dfa_fast;
};

static struct shared_dfa_kwset *dfa_kwset_cache;

struct dfa_comp
{
  /* KWset compiled pattern.  For GEAcompile, we compile
     a list of strings, at least one of which is known to occur in
     any string matching the regexp. */
  kwset_t kwset;
  struct shared_dfa_kwset *shared_kwset;

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
is_quantifier (char const *p, char const *lim, reg_syntax_t syntax_bits)
{
  if (p >= lim)
    return false;
  if (syntax_bits & RE_NO_BK_VBAR) /* ERE */
    {
      if (*p == '*' || *p == '+' || *p == '?')
        return true;
      if (*p == '{')
        return true;
      return false;
    }
  else /* BRE */
    {
      if (*p == '*')
        return true;
      if (p + 1 < lim && *p == '\\')
        {
          if (p[1] == '+' || p[1] == '?' || p[1] == '{')
            return true;
        }
      return false;
    }
}

struct branch_info
{
  char *best_lit;
  idx_t best_len;
  bool is_exact;
};

static void
kwsmusts_multi (struct dfa_comp *dc, char const *raw_pat, idx_t raw_size,
                reg_syntax_t syntax_bits)
{
  char const *p = raw_pat;
  char const *lim = raw_pat + raw_size;

  idx_t branch_cap = 16;
  idx_t nbranches = 0;
  struct branch_slice
  {
    char const *start;
    idx_t len;
  } *slices = xnmalloc (branch_cap, sizeof *slices);

  char const *b_start = p;
  idx_t paren_depth = 0;
  bool in_bracket = false;

  while (p < lim)
    {
      if (in_bracket)
        {
          if (*p == '[' && p + 1 < lim && (p[1] == ':' || p[1] == '=' || p[1] == '.'))
            {
              char delim = p[1];
              p += 2;
              while (p + 1 < lim && !(p[0] == delim && p[1] == ']'))
                p++;
              if (p + 1 < lim)
                p += 2;
              continue;
            }
          if (*p == ']')
            {
              in_bracket = false;
              p++;
              continue;
            }
          p++;
          continue;
        }

      /* !in_bracket */
      if (*p == '[')
        {
          in_bracket = true;
          p++;
          if (p < lim && *p == '^')
            p++;
          if (p < lim && *p == ']')
            p++;
          continue;
        }

      /* Check parens */
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
              if (paren_depth == 0)
                goto fail_slices;
              paren_depth--;
              p++;
              continue;
            }
          if (*p == '\\')
            {
              if (p + 1 >= lim)
                goto fail_slices;
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
                goto fail_slices;
              paren_depth--;
              p += 2;
              continue;
            }
          if (*p == '\\')
            {
              if (p + 1 >= lim)
                goto fail_slices;
              p += 2;
              continue;
            }
        }

      /* Top-level branch separators */
      if (paren_depth == 0)
        {
          idx_t sep_len = 0;
          if (*p == '\n')
            sep_len = 1;
          else if ((syntax_bits & RE_NO_BK_VBAR) && *p == '|')
            sep_len = 1;
          else if (!(syntax_bits & RE_NO_BK_VBAR) && !(syntax_bits & RE_LIMITED_OPS)
                   && *p == '\\' && p + 1 < lim && p[1] == '|')
            sep_len = 2;

          if (sep_len > 0)
            {
              idx_t blen = p - b_start;
              if (blen == 0)
                goto fail_slices;
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

      p++;
    }

  if (in_bracket || paren_depth != 0)
    goto fail_slices;

  if (b_start < lim)
    {
      if (nbranches == branch_cap)
        slices = xpalloc (slices, &branch_cap, 1, -1, sizeof *slices);
      slices[nbranches].start = b_start;
      slices[nbranches].len = lim - b_start;
      nbranches++;
    }
  else
    goto fail_slices;

  if (nbranches < 2)
    goto fail_slices;

  /* Now parse each branch */
  struct branch_info *branches = xnmalloc (nbranches, sizeof *branches);
  memset (branches, 0, nbranches * sizeof *branches);

  for (idx_t i = 0; i < nbranches; i++)
    {
      char const *bp = slices[i].start;
      char const *blim = slices[i].start + slices[i].len;
      idx_t blen = slices[i].len;

      char *cur_lit = xmalloc (blen + 1);
      char *best_lit = xmalloc (blen + 1);
      idx_t cur_len = 0;
      idx_t best_len = 0;
      bool branch_is_pure = true;
      idx_t b_paren = 0;
      bool b_bracket = false;

      while (bp < blim)
        {
          if (b_bracket)
            {
              branch_is_pure = false;
              if (cur_len > best_len)
                {
                  memcpy (best_lit, cur_lit, cur_len);
                  best_len = cur_len;
                }
              cur_len = 0;

              if (*bp == '[' && bp + 1 < blim && (bp[1] == ':' || bp[1] == '=' || bp[1] == '.'))
                {
                  char delim = bp[1];
                  bp += 2;
                  while (bp + 1 < blim && !(bp[0] == delim && bp[1] == ']'))
                    bp++;
                  if (bp + 1 < blim)
                    bp += 2;
                  continue;
                }
              if (*bp == ']')
                {
                  b_bracket = false;
                  bp++;
                  continue;
                }
              bp++;
              continue;
            }

          /* !b_bracket */
          if (*bp == '[')
            {
              branch_is_pure = false;
              if (cur_len > best_len)
                {
                  memcpy (best_lit, cur_lit, cur_len);
                  best_len = cur_len;
                }
              cur_len = 0;
              b_bracket = true;
              bp++;
              if (bp < blim && *bp == '^')
                bp++;
              if (bp < blim && *bp == ']')
                bp++;
              continue;
            }

          /* Parens */
          if (syntax_bits & RE_NO_BK_PARENS)
            {
              if (*bp == '(')
                {
                  branch_is_pure = false;
                  if (cur_len > best_len)
                    {
                      memcpy (best_lit, cur_lit, cur_len);
                      best_len = cur_len;
                    }
                  cur_len = 0;
                  b_paren++;
                  bp++;
                  continue;
                }
              if (*bp == ')')
                {
                  branch_is_pure = false;
                  if (cur_len > best_len)
                    {
                      memcpy (best_lit, cur_lit, cur_len);
                      best_len = cur_len;
                    }
                  cur_len = 0;
                  if (b_paren > 0)
                    b_paren--;
                  bp++;
                  continue;
                }
            }
          else
            {
              if (*bp == '\\' && bp + 1 < blim && bp[1] == '(')
                {
                  branch_is_pure = false;
                  if (cur_len > best_len)
                    {
                      memcpy (best_lit, cur_lit, cur_len);
                      best_len = cur_len;
                    }
                  cur_len = 0;
                  b_paren++;
                  bp += 2;
                  continue;
                }
              if (*bp == '\\' && bp + 1 < blim && bp[1] == ')')
                {
                  branch_is_pure = false;
                  if (cur_len > best_len)
                    {
                      memcpy (best_lit, cur_lit, cur_len);
                      best_len = cur_len;
                    }
                  cur_len = 0;
                  if (b_paren > 0)
                    b_paren--;
                  bp += 2;
                  continue;
                }
            }

          if (b_paren > 0)
            {
              branch_is_pure = false;
              if (cur_len > best_len)
                {
                  memcpy (best_lit, cur_lit, cur_len);
                  best_len = cur_len;
                }
              cur_len = 0;
              if (*bp == '\\' && bp + 1 < blim)
                bp += 2;
              else
                bp++;
              continue;
            }

          /* At b_paren == 0 && !b_bracket: check if unquantified literal byte */
          if (*bp == '\\')
            {
              if (bp + 1 >= blim)
                {
                  branch_is_pure = false;
                  if (cur_len > best_len)
                    {
                      memcpy (best_lit, cur_lit, cur_len);
                      best_len = cur_len;
                    }
                  cur_len = 0;
                  break;
                }
              char c = bp[1];
              if (strchr ("^.[$()|*+?{\\", c) != nullptr)
                {
                  if (is_quantifier (bp + 2, blim, syntax_bits))
                    {
                      branch_is_pure = false;
                      if (cur_len > best_len)
                        {
                          memcpy (best_lit, cur_lit, cur_len);
                          best_len = cur_len;
                        }
                      cur_len = 0;
                      bp += 2;
                    }
                  else
                    {
                      cur_lit[cur_len++] = c;
                      bp += 2;
                    }
                }
              else
                {
                  /* Special escape (\b, \w, \s, etc.) */
                  branch_is_pure = false;
                  if (cur_len > best_len)
                    {
                      memcpy (best_lit, cur_lit, cur_len);
                      best_len = cur_len;
                    }
                  cur_len = 0;
                  bp += 2;
                }
              continue;
            }

          /* Check metacharacters */
          bool is_meta = false;
          if (syntax_bits & RE_NO_BK_VBAR) /* ERE */
            {
              if (*bp == '^' || *bp == '$' || *bp == '.' || *bp == '*'
                  || *bp == '+' || *bp == '?' || *bp == '{' || *bp == '|')
                is_meta = true;
            }
          else /* BRE */
            {
              if (*bp == '^' || *bp == '$' || *bp == '.' || *bp == '*')
                is_meta = true;
            }

          if (is_meta)
            {
              branch_is_pure = false;
              if (cur_len > best_len)
                {
                  memcpy (best_lit, cur_lit, cur_len);
                  best_len = cur_len;
                }
              cur_len = 0;
              bp++;
              continue;
            }

          /* Ordinary character */
          if (is_quantifier (bp + 1, blim, syntax_bits))
            {
              branch_is_pure = false;
              if (cur_len > best_len)
                {
                  memcpy (best_lit, cur_lit, cur_len);
                  best_len = cur_len;
                }
              cur_len = 0;
              bp++;
            }
          else
            {
              cur_lit[cur_len++] = *bp;
              bp++;
            }
        }

      if (cur_len > best_len)
        {
          memcpy (best_lit, cur_lit, cur_len);
          best_len = cur_len;
        }
      free (cur_lit);

      if (best_len < 2)
        {
          free (best_lit);
          for (idx_t j = 0; j < i; j++)
            free (branches[j].best_lit);
          free (branches);
          goto fail_slices;
        }

      branches[i].best_lit = best_lit;
      branches[i].best_len = best_len;
      branches[i].is_exact = branch_is_pure && !match_words && !match_lines;
    }

  /* Success! Initialize kwset with kwsinit(true) */
  dc->kwset = kwsinit (true);

  /* Pass 1: exact keywords */
  for (idx_t i = 0; i < nbranches; i++)
    {
      if (branches[i].is_exact)
        {
          kwsincr (dc->kwset, branches[i].best_lit, branches[i].best_len);
          dc->kwset_exact_matches++;
        }
    }

  /* Pass 2: non-exact keywords */
  for (idx_t i = 0; i < nbranches; i++)
    {
      if (!branches[i].is_exact)
        kwsincr (dc->kwset, branches[i].best_lit, branches[i].best_len);
    }

  kwsprep (dc->kwset);

  for (idx_t i = 0; i < nbranches; i++)
    free (branches[i].best_lit);
  free (branches);

 fail_slices:
  free (slices);
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
  if (!dm)
    {
      if (dc->pcount == 0
          && (!localeinfo.multibyte || localeinfo.using_utf8)
          && (!match_icase || !localeinfo.multibyte))
        kwsmusts_multi (dc, raw_pat, raw_size, syntax_bits);
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
      kwsincr (dc->kwset, must, new_len);
      free (must);
    }
  else
    {
      /* Otherwise, filtering with this substring should help reduce the
         search space, but we'll still have to use the regexp matcher.  */
      kwsincr (dc->kwset, dm->must, strlen (dm->must));
    }
  kwsprep (dc->kwset);
  dfamustfree (dm);
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

static char const * _GL_ATTRIBUTE_PURE
skip_bracket_expr (char const *s, char const *lim)
{
  /* Skip '['.  */
  s++;
  if (s < lim && *s == '^')
    s++;
  if (s < lim && *s == ']')
    s++;
  while (s < lim && *s != ']')
    {
      if (s + 1 < lim && *s == '['
          && (s[1] == ':' || s[1] == '.' || s[1] == '='))
        {
          char ctype = s[1];
          s += 2;
          while (s + 1 < lim && !(s[0] == ctype && s[1] == ']'))
            s++;
          if (s + 1 < lim)
            {
              s += 2;
              continue;
            }
          return lim;
        }
      s++;
    }
  if (s < lim)
    s++;
  return s;
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

  int depth = 0;
  char const *g1_start = nullptr;
  char const *g1_end = nullptr;

  for (char const *s = p; s < lim; s++)
    {
      if (*s == '[')
        {
          s = skip_bracket_expr (s, lim) - 1;
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
                return;
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
              depth--;
              if (depth == 0 && g1_start && !g1_end)
                g1_end = s;
            }
          else if (*s == '|' && depth == 0)
            return;
        }
    }

  if (!g1_start || !g1_end || g1_start > g1_end)
    return;

  for (char const *s = g1_start; s < g1_end; s++)
    {
      if (*s == '[')
        {
          s = skip_bracket_expr (s, g1_end) - 1;
          continue;
        }
      if (*s == '\\')
        {
          s++;
          if (s >= g1_end)
            break;
          if (!no_bk_parens && (*s == '(' || *s == ')' || *s == '|'))
            return;
          continue;
        }
      if (no_bk_parens && (*s == '(' || *s == ')' || *s == '|'))
        return;
    }

  idx_t min_len = 0;
  char const *s = g1_start;
  while (s < g1_end)
    {
      idx_t atom_base = 1;
      if (*s == '[')
        {
          s = skip_bracket_expr (s, g1_end);
        }
      else if (*s == '\\')
        {
          s++;
          if (s < g1_end)
            {
              if (*s == 'b' || *s == 'B' || *s == '<' || *s == '>'
                  || *s == '\'' || *s == '`')
                atom_base = 0;
              s++;
            }
        }
      else if (*s == '^' || *s == '$')
        {
          atom_base = 0;
          s++;
        }
      else
        {
          char const *atom_start = s;
          s++;
          if (localeinfo.using_utf8 && (unsigned char) atom_start[0] >= 0x80)
            {
              while (s < g1_end && ((unsigned char) *s & 0xC0) == 0x80)
                s++;
            }
          atom_base = (*atom_start == '.') ? 1 : (s - atom_start);
        }

      idx_t qmin = 1;
      if (s < g1_end)
        {
          if (no_bk_parens)
            {
              if (*s == '*')
                {
                  qmin = 0;
                  s++;
                }
              else if (*s == '?')
                {
                  qmin = 0;
                  s++;
                }
              else if (*s == '+')
                {
                  qmin = 1;
                  s++;
                }
              else if (*s == '{')
                {
                  s++;
                  qmin = 0;
                  while (s < g1_end && *s >= '0' && *s <= '9')
                    {
                      qmin = qmin * 10 + (*s - '0');
                      s++;
                    }
                  while (s < g1_end && *s != '}')
                    s++;
                  if (s < g1_end)
                    s++;
                }
            }
          else
            {
              if (*s == '*')
                {
                  qmin = 0;
                  s++;
                }
              else if (*s == '\\' && s + 1 < g1_end)
                {
                  if (s[1] == '?')
                    {
                      qmin = 0;
                      s += 2;
                    }
                  else if (s[1] == '+')
                    {
                      qmin = 1;
                      s += 2;
                    }
                  else if (s[1] == '{')
                    {
                      s += 2;
                      qmin = 0;
                      while (s < g1_end && *s >= '0' && *s <= '9')
                        {
                          qmin = qmin * 10 + (*s - '0');
                          s++;
                        }
                      while (s + 1 < g1_end && !(s[0] == '\\' && s[1] == '}'))
                        s++;
                      if (s + 1 < g1_end)
                        s += 2;
                    }
                }
            }
        }
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

  char const *after_1 = bref + 2;
  if (after_1 < lim)
    {
      if (no_bk_parens)
        {
          if (*after_1 == '?' || *after_1 == '*')
            return;
          if (*after_1 == '{' && after_1 + 1 < lim && after_1[1] == '0')
            return;
        }
      else
        {
          if (*after_1 == '*')
            return;
          if (*after_1 == '\\' && after_1 + 1 < lim)
            {
              if (after_1[1] == '?')
                return;
              if (after_1[1] == '{' && after_1 + 2 < lim && after_1[2] == '0')
                return;
            }
        }
    }

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
          if (rem < bf->min_len)
            break;
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
  char const *pat_filename
    = lineno < 0 ? "" : pattern_file_name (lineno, &pat_lineno);

  if (*pat_filename == '\0')
    error (0, 0, "%s", err);
  else
    {
      ptrdiff_t n = pat_lineno;
      error (0, 0, "%s:%td: %s", pat_filename, n, err);
    }

  return false;
}

/* Compile PATTERN, containing SIZE bytes that are followed by '\n'.
   SYNTAX_BITS specifies whether PATTERN uses style -G, -E, or -A.
   Return a description of the compiled pattern.  */

void *
GEAcompile (char *pattern, idx_t size, reg_syntax_t syntax_bits,
            bool exact)
{
  char *motif;
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

  /* For GNU regex, pass the patterns separately to detect errors like
     "[\nallo\n]\n", where the patterns are "[", "allo" and "]", and
     this should be a syntax error.  The same for backref, where the
     backref should be local to each pattern.  */
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

  /* Buffer containing back-reference-free patterns.  */
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
          dc->patterns_alloc = xpalloc (dc->patterns_alloc, &palloc, shortage, -1,
                                        sizeof *dc->patterns);
          dc->patterns = dc->patterns_alloc + 1;
          dc->bref_filters_alloc = xrealloc (dc->bref_filters_alloc,
                                             palloc * sizeof *dc->bref_filters);
          memset (dc->bref_filters_alloc + (dc->pcount + 1), 0,
                  (palloc - (dc->pcount + 1)) * sizeof *dc->bref_filters);
          dc->bref_filters = dc->bref_filters_alloc + 1;
        }

      if (!suppress_dfawarn || backref)
        {
          if (!regex_compile (dc, p, len, dc->pcount, lineno, syntax_bits,
                              !backref))
            compilation_failed = true;
        }

      char const *pat_line = p;
      p = sep + 1;
      lineno++;

      if (backref)
        {
          if (!match_icase)
            analyze_backref_filter (pat_line, len, syntax_bits,
                                    &dc->bref_filters[dc->pcount]);
          dc->pcount++;
          prev = p;
        }
    }
  while (p <= patlim);

  if (compilation_failed)
    exit (EXIT_TROUBLE);

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

  char const *raw_pat = pattern;
  idx_t raw_size = size;

  /* In the match_words and match_lines cases, we use a different pattern
     for the DFA matcher that will quickly throw out cases that won't work.
     Then if DFA succeeds we do some hairy stuff using the regex matcher
     to decide whether the match should really count. */
  if (match_words || match_lines)
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
      pattern = motif = n;
      size = total;
    }
  else
    motif = nullptr;

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
      atomic_fetch_add (&sk->refs, 1);

      if (suppress_dfawarn && sk->dfa_supported && !exact && dc->pcount == 0)
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
    }
  else
    {
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
          sk->pattern = ximemdup (pattern, size);
          sk->size = size;
          sk->match_icase = match_icase;
          sk->match_words = match_words;
          sk->match_lines = match_lines;
          sk->exact = exact;
          sk->eolbyte = eolbyte;
          sk->syntax_bits = syntax_bits;
          sk->dfa_supported = dfasupported (dc->dfa);
          sk->dfa_fast = dfaisfast (dc->dfa);
          atomic_init (&sk->refs, 2);
          dfa_kwset_cache = sk;
          dc->shared_kwset = sk;
        }
    }

  if (buf)
    {
      if (exact || !(dc->shared_kwset ? dc->shared_kwset->dfa_supported : dfasupported (dc->dfa)))
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
              char const *buflim, idx_t *count, bool *backref)
{
  idx_t len = end - begin;
  if (end < buflim && *(unsigned char const *) end == (unsigned char) eolbyte)
    return dfaexec (d, begin, end, 0, count, backref);

  char stackbuf[4096];

  if (len < 0)
    {
      /* When EGexecute prefers the DFA to the keyword matcher, END can
         be a byte or two before BEGIN.  dfaexec then examines only the
         line that starts at BEGIN.  Do the same on a private copy that
         keeps END where it was relative to BEGIN, so that the sentinel
         is not stored into a buffer that other threads may be reading.  */
      char const *line_end = rawmemchr (begin, eolbyte);
      idx_t pre = begin - end;
      idx_t n = line_end + 1 - end;
      char *copy = n < (idx_t) sizeof stackbuf ? stackbuf : xmalloc (n + 1);
      memcpy (copy, end, n);
      copy[n] = eolbyte;
      char *res = dfaexec (d, copy + pre, copy, 0, count, backref);
      char *ret = res ? (char *) begin + (res - (copy + pre)) : nullptr;
      if (copy != stackbuf)
        free (copy);
      return ret;
    }

  if (len >= sizeof stackbuf && end > begin + 1)
    {
      char *prev_nl = memrchr (begin, eolbyte, (end - 1) - begin);
      if (prev_nl)
        {
          char *split = prev_nl + 1;
          if (split < buflim)
            {
              char *r1 = dfaexec (d, begin, split, 0, count, backref);
              if (r1 && r1 < split)
                return r1;
              begin = split;
              len = end - begin;
            }
        }
    }

  char *copy = (len < sizeof stackbuf) ? stackbuf : xmalloc (len + 1);
  memcpy (copy, begin, len);
  copy[len] = eolbyte;

  char *res = dfaexec (d, copy, copy + len, 0, count, backref);
  char *ret;
  if (!res)
    ret = nullptr;
  else if (res == copy + len)
    ret = end;
  else
    ret = (char *) begin + (res - copy);

  if (copy != stackbuf)
    free (copy);
  return ret;
}

ptrdiff_t
EGexecute (void *vdc, char const *buf, idx_t size, idx_t *match_size,
           char const *start_ptr)
{
  char const *buflim, *beg, *end, *ptr, *match, *best_match, *mb_start;
  char eol = eolbyte;
  regoff_t start;
  idx_t len, best_len;
  struct kwsmatch kwsm;
  idx_t i;
  struct dfa_comp *dc = vdc;
  bool dfafast = dc->shared_kwset ? dc->shared_kwset->dfa_fast
                                  : (dc->dfa_compiled ? dfaisfast (dc->dfa) : false);

  mb_start = buf;
  buflim = buf + size;

  for (beg = end = buf; end < buflim; beg = end)
    {
      end = buflim;

      if (!start_ptr)
        {
          char const *next_beg, *dfa_beg = beg;
          idx_t count = 0;
          bool exact_kwset_match = false;
          bool backref = false;

          /* Try matching with KWset, if it's defined.  */
          if (dc->kwset)
            {
              char const *prev_beg;

              /* Find a possible match using the KWset matcher.  */
              ptrdiff_t offset = kwsexec (dc->kwset, beg - dc->begline,
                                          buflim - beg + dc->begline,
                                          &kwsm, true);
              if (offset < 0)
                return offset;
              match = beg + offset;
              prev_beg = beg;

              /* Narrow down to the line containing the possible match.  */
              beg = memrchr (buf, eol, match - buf);
              beg = beg ? beg + 1 : buf;
              dfa_beg = beg;

              /* Determine the end pointer to give the DFA next.  Typically
                 this is after the first newline after MATCH; but if the KWset
                 match is not exact, the DFA is fast, and the offset from
                 PREV_BEG is less than 64 or (MATCH - PREV_BEG), this is the
                 greater of the latter two values; this temporarily prefers
                 the DFA to KWset.  */
              exact_kwset_match = kwsm.index < dc->kwset_exact_matches;
              if (exact_kwset_match || !dfafast
                  || MAX (16, match - beg) < (match - prev_beg) >> 2)
                {
                  end = rawmemchr (match, eol);
                  end++;
                }
              else if (MAX (16, match - beg) < (buflim - prev_beg) >> 2)
                {
                  end = rawmemchr (prev_beg + 4 * MAX (16, match - beg), eol);
                  end++;
                }
              else
                end = buflim;

              if (exact_kwset_match)
                {
                  if (!localeinfo.multibyte | localeinfo.using_utf8)
                    goto success;
                  if (mb_start < beg)
                    mb_start = beg;
                  if (mb_goback (&mb_start, nullptr, match, buflim) == 0)
                    goto success;
                  /* The matched line starts in the middle of a multibyte
                     character.  Perform the DFA search starting from the
                     beginning of the next character.  */
                  dfa_beg = mb_start;
                }
            }

          if (!dc->dfa_compiled)
            {
              dfaparse (dc->dfa_pattern, dc->dfa_pattern_size, dc->dfa);
              dfacomp (nullptr, 0, dc->dfa, 1);
              dc->dfa_compiled = true;
            }
          struct dfa *superset = dfasuperset (dc->dfa);

          /* Try matching with the superset of DFA, if it's defined.  */
          if (superset && !exact_kwset_match)
            {
              /* Keep using the superset while it reports multiline
                 potential matches; this is more likely to be fast
                 than falling back to KWset would be.  */
              next_beg = safe_dfaexec (superset, dfa_beg, (char *) end,
                                       buflim, &count, nullptr);
              if (!next_beg || next_beg == end)
                continue;

              /* Narrow down to the line we've found.  */
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

          /* Try matching with DFA.  */
          next_beg = safe_dfaexec (dc->dfa, dfa_beg, (char *) end, buflim,
                                   &count, &backref);

          /* If there's no match, or if we've matched the sentinel,
             we're done.  */
          if (!next_beg || next_beg == end)
            continue;

          /* Narrow down to the line we've found.  */
          if (count != 0)
            {
              beg = memrchr (buf, eol, next_beg - buf);
              beg++;
            }
          end = rawmemchr (next_beg, eol);
          end++;

          /* Successful, no back-references encountered! */
          if (!backref)
            goto success;
          ptr = beg;
        }
      else
        {
          /* We are looking for the leftmost (then longest) exact match.
             We will go through the outer loop only once.  */
          ptr = start_ptr;
        }

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
              && !backref_line_can_match (&dc->bref_filters[i], beg, end - beg - 1))
            continue;

          dc->patterns[i].not_eol = 0;
          dc->patterns[i].newline_anchor = eolbyte == '\n';
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
              /* If -w and not -x, check whether the match aligns with
                 word boundaries.  Do this iteratively because:
                 (a) the line may contain more than one occurrence of the
                 pattern, and
                 (b) Several alternatives in the pattern might be valid at a
                 given point, and we may need to consider a shorter one to
                 find a word boundary.  */
              if (!match_lines && match_words)
                while (match <= best_match)
                  {
                    regoff_t shorter_len = 0;
                    if (! wordchar_next (match + len, end - 1)
                        && ! wordchar_prev (beg, match, end - 1))
                      goto assess_pattern_match;
                    if (len > 0)
                      {
                        /* Try a shorter length anchored at the same place. */
                        --len;
                        dc->patterns[i].not_eol = 1;
                        shorter_len = re_match (&dc->patterns[i], beg,
                                                match + len - ptr, match - beg,
                                                &dc->regs);
                        if (shorter_len < -1)
                          xalloc_die ();
                      }
                    if (0 < shorter_len)
                      len = shorter_len;
                    else
                      {
                        /* Try looking further on. */
                        if (match == end - 1)
                          break;
                        match++;
                        dc->patterns[i].not_eol = 0;
                        start = re_search (&dc->patterns[i], beg, end - beg - 1,
                                           match - beg, end - match - 1,
                                           &dc->regs);
                        if (start < 0)
                          {
                            if (start < -1)
                              xalloc_die ();
                            break;
                          }
                        len = dc->regs.end[0] - start;
                        match = beg + start;
                      }
                  } /* while (match <= best_match) */
              continue;
            assess_pattern_match:
              if (!start_ptr)
                {
                  /* Good enough for a non-exact match.
                     No need to look at further patterns, if any.  */
                  goto success;
                }
              if (match < best_match || (match == best_match && len > best_len))
                {
                  /* Best exact match:  leftmost, then longest.  */
                  best_match = match;
                  best_len = len;
                }
            } /* if re_search >= 0 */
        } /* for Regex patterns.  */
        if (best_match < end)
          {
            /* We have found an exact match.  We were just
               waiting for the best one (leftmost then longest).  */
            beg = best_match;
            len = best_len;
            goto success_in_len;
          }
    } /* for (beg = end ..) */

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
          if (sk->kwset)
            kwsfree (sk->kwset);
          free (sk->pattern);
          free (sk);
        }
    }
  else if (dc->kwset)
    kwsfree (dc->kwset);
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
