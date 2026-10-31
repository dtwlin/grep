/* Compile-time literal and back-reference prefilters for dfasearch.c.
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

#ifndef GREP_DFA_PREFILTER_H
#define GREP_DFA_PREFILTER_H 1

#include <regex.h>
#include <stddef.h>
#include <string.h>

#include "idx.h"

struct backref_filter
{
  bool enabled;
  idx_t min_len;
  idx_t sep_len;
  char sep[8];
};

struct prefilter_branch
{
  char *lit;    /* malloc'd longest literal that the branch requires */
  idx_t len;
  bool exact;   /* finding LIT in a line means the line matches */
};

/* Split PAT into top-level branches and find each branch's literal.
   Return the number of branches, or -1 if some branch has none.  */
extern ptrdiff_t prefilter_branch_literals (char const *pat, idx_t size,
                                            reg_syntax_t syntax_bits,
                                            bool utf8, bool words_or_lines,
                                            struct prefilter_branch **out);
extern void prefilter_branches_free (struct prefilter_branch *branches,
                                     ptrdiff_t n);

/* Store in MUST2 the longest literal (at least 2 bytes) that every
   match of PAT contains and PRIMARY does not; return its length or 0.  */
extern idx_t prefilter_secondary_must (char const *pat, idx_t size,
                                       reg_syntax_t syntax_bits, bool utf8,
                                       char const *primary, char must2[64]);

/* Fill *BF for the back-reference pattern PAT.  */
extern void prefilter_backref_filter (char const *pat, idx_t len,
                                      reg_syntax_t syntax_bits, bool utf8,
                                      struct backref_filter *bf);

/* Return false if LINE[0..LEN) cannot match the back-reference pattern
   described by *BF.  EGexecute calls this per line, so it stays inline
   in dfasearch.c's translation unit.  */
static inline bool _GL_ATTRIBUTE_PURE
backref_line_can_match (struct backref_filter const *bf, char const *line,
                        idx_t len)
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

          if (bf->sep_len > 1
              && memcmp (pos + 1, bf->sep + 1, bf->sep_len - 1) != 0)
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

#endif /* GREP_DFA_PREFILTER_H */
