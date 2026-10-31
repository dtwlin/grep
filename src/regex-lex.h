/* Shared compile-time regex lexer primitives for prefilter analysis.
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

#ifndef GREP_REGEX_LEX_H
#define GREP_REGEX_LEX_H 1

#include <stdckdint.h>
#include <stddef.h>
#include <wchar.h>

#include "idx.h"

/* Byte length of the UTF-8 lead byte at P plus all continuation bytes
   (10xxxxxx) that immediately follow it before LIM.  */
static inline idx_t _GL_ATTRIBUTE_PURE
utf8_byte_run_len (char const *p, char const *lim)
{
  idx_t n = 1;
  if ((unsigned char) *p >= 0x80)
    while (p + n < lim && ((unsigned char) p[n] & 0xC0) == 0x80)
      n++;
  return n;
}

/* mbrlen on [P, LIM) with a fresh conversion state; callers decide how
   to treat (size_t) -1, (size_t) -2, and 0.  */
static inline size_t
utf8_char_len (char const *p, char const *lim)
{
  mbstate_t mbs = { 0 };
  return mbrlen (p, lim - p, &mbs);
}

enum
{
  /* Expect '\{' and '\}' rather than '{' and '}'.  */
  REGEX_IV_BACKSLASH = 1 << 0,
  /* Accept '{,n}' (empty lower bound with at least one upper digit).  */
  REGEX_IV_ALLOW_EMPTY_MIN = 1 << 1,
  /* Accept '{,}' (both bounds empty when a comma is present).  */
  REGEX_IV_ALLOW_EMPTY_BOTH = 1 << 2
};

struct regex_interval
{
  idx_t min_rep;        /* lower bound (0 if empty; IDX_MAX on overflow) */
  bool has_comma;       /* true when a comma appears inside the braces */
  char const *end;      /* byte immediately after closing '}' or '\}' */
};

/* Parse an interval at P ("{" or "\{" per FLAGS) bounded by LIM.
   Return true and fill *OUT if it is a valid interval under FLAGS.  */
static inline bool
regex_parse_interval (char const *p, char const *lim, int flags,
                      struct regex_interval *out)
{
  bool bk = (flags & REGEX_IV_BACKSLASH) != 0;
  if (bk)
    {
      if (lim - p < 2 || p[0] != '\\' || p[1] != '{')
        return false;
      p += 2;
    }
  else
    {
      if (p >= lim || *p != '{')
        return false;
      p++;
    }

  idx_t min_rep = 0;
  idx_t min_digits = 0;
  while (p < lim && '0' <= *p && *p <= '9')
    {
      int d = *p - '0';
      if (ckd_mul (&min_rep, min_rep, 10) || ckd_add (&min_rep, min_rep, d))
        min_rep = IDX_MAX;
      min_digits++;
      p++;
    }

  bool has_comma = false;
  idx_t max_digits = 0;
  if (p < lim && *p == ',')
    {
      has_comma = true;
      p++;
      while (p < lim && '0' <= *p && *p <= '9')
        {
          max_digits++;
          p++;
        }
    }

  if (min_digits == 0)
    {
      if (!has_comma)
        return false;
      if (max_digits > 0)
        {
          if (!(flags & REGEX_IV_ALLOW_EMPTY_MIN))
            return false;
        }
      else if (!(flags & REGEX_IV_ALLOW_EMPTY_BOTH))
        return false;
    }

  if (bk)
    {
      if (lim - p < 2 || p[0] != '\\' || p[1] != '}')
        return false;
      p += 2;
    }
  else
    {
      if (p >= lim || *p != '}')
        return false;
      p++;
    }

  out->min_rep = min_rep;
  out->has_comma = has_comma;
  out->end = p;
  return true;
}

/* End of the '[:', '[.', or '[=' class token at P inside a bracket
   expression bounded by LIM, or nullptr if unclosed before ']' or LIM.  */
static inline char const * _GL_ATTRIBUTE_PURE
regex_bracket_class_end (char const *p, char const *lim)
{
  if (lim - p < 2 || p[0] != '['
      || (p[1] != ':' && p[1] != '.' && p[1] != '='))
    return nullptr;
  char ctype = p[1];
  char const *q = p + 2;
  while (lim - q >= 2 && *q != ']' && !(q[0] == ctype && q[1] == ']'))
    q++;
  if (lim - q >= 2 && q[0] == ctype && q[1] == ']')
    return q + 2;
  return nullptr;
}

#endif /* GREP_REGEX_LEX_H */
