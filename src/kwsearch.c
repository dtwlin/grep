/* kwsearch.c - searching subroutines using kwset for grep.
   Copyright 1992, 1998, 2000, 2007, 2009-2026 Free Software Foundation, Inc.

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
#include <search.h>

#include <stdatomic.h>

/* A kwset compiled from a pattern list.  All compilations of the same
   pattern list with the same options share it, as it is read-only
   after kwsprep; this saves each worker thread from building its own
   copy, which can be costly for large pattern lists.  */

struct shared_kwset
{
  kwset_t kwset;

  /* The number of words in KWSET.  */
  idx_t words;

  /* The number of compiled patterns that use KWSET, plus one if
     KWSET is in the cache.  */
  atomic_ptrdiff_t refs;

  /* The pattern list and options KWSET was compiled from.  */
  char *pattern;
  idx_t size;
  bool match_icase, match_words, match_lines, exact;
  char eolbyte;
};

/* The first shared kwset, kept for the life of the process.  It is set
   before any worker thread starts, and the worker threads only read it.  */
static struct shared_kwset *kwset_cache;

/* A compiled -F pattern list.  */

struct kwsearch
{
  /* The kwset for this pattern list, and its shared owner.  */
  kwset_t kwset;
  struct shared_kwset *shared;

  /* The number of user-specified patterns.  This is less than
     'kwswords (kwset)' when some extra one-character words have been
     appended, one for each troublesome character that will require a
     DFA search.  */
  idx_t words;

  /* The user's pattern and its size in bytes.  */
  char *pattern;
  idx_t size;

  /* The user's pattern compiled as a regular expression,
     or null if it has not been compiled.  */
  void *re;
};

/* Return a shared kwset for the -F style PATTERN, containing SIZE bytes
   that are followed by '\n', compiled with the current options and
   EXACT.  The caller owns one reference to it.  */

static struct shared_kwset *
shared_kwset_get (char const *pattern, idx_t size, bool exact)
{
  struct shared_kwset *sk = kwset_cache;
  if (sk && sk->size == size && memcmp (sk->pattern, pattern, size) == 0
      && sk->match_icase == match_icase && sk->match_words == match_words
      && sk->match_lines == match_lines && sk->exact == exact
      && sk->eolbyte == eolbyte)
    {
      atomic_fetch_add (&sk->refs, 1);
      return sk;
    }

  kwset_t kwset;
  char *buf = nullptr;
  idx_t bufalloc = 0;

  kwset = kwsinit (true);

  char const *p = pattern;
  do
    {
      char const *sep = rawmemchr (p, '\n');
      idx_t len = sep - p;

      if (match_lines)
        {
          if (eolbyte == '\n' && pattern < p)
            p--;
          else
            {
              if (bufalloc < len + 2)
                {
                  free (buf);
                  bufalloc = len;
                  buf = xpalloc (nullptr, &bufalloc, 2, -1, 1);
                  buf[0] = eolbyte;
                }
              memcpy (buf + 1, p, len);
              buf[len + 1] = eolbyte;
              p = buf;
            }
          len += 2;
        }
      kwsincr (kwset, p, len);

      p = sep + 1;
    }
  while (p <= pattern + size);

  free (buf);

  sk = xmalloc (sizeof *sk);
  sk->words = kwswords (kwset);
  kwsprep (kwset);
  sk->kwset = kwset;
  sk->pattern = ximemdup (pattern, size);
  sk->size = size;
  sk->match_icase = match_icase;
  sk->match_words = match_words;
  sk->match_lines = match_lines;
  sk->exact = exact;
  sk->eolbyte = eolbyte;
  bool cache = !kwset_cache;
  atomic_init (&sk->refs, 1 + cache);
  if (cache)
    kwset_cache = sk;
  return sk;
}

/* Compile the -F style PATTERN, containing SIZE bytes that are
   followed by '\n'.  Return a description of the compiled pattern.  */

void *
Fcompile (char *pattern, idx_t size, reg_syntax_t ignored, bool exact)
{
  struct shared_kwset *sk = shared_kwset_get (pattern, size, exact);

  struct kwsearch *kwsearch = xmalloc (sizeof *kwsearch);
  kwsearch->kwset = sk->kwset;
  kwsearch->shared = sk;
  kwsearch->words = sk->words;
  kwsearch->pattern = ximemdup0 (pattern, size);
  kwsearch->size = size;
  kwsearch->re = nullptr;

  if (!localeinfo.multibyte && match_words)
    {
      fgrep_to_grep_pattern (&kwsearch->pattern, &kwsearch->size);
      kwsearch->re = GEAcompile (kwsearch->pattern,
                                 kwsearch->size,
                                 RE_SYNTAX_GREP, exact);
    }

  return kwsearch;
}

/* Use the compiled pattern VCP to search the buffer BUF of size SIZE.
   If found, return the offset of the first match and store its
   size into *MATCH_SIZE.  If not found, return -1.
   If START_PTR is nonnull, start searching there.  */
ptrdiff_t
Fexecute (void *vcp, char const *buf, idx_t size, idx_t *match_size,
          char const *start_ptr)
{
  char const *beg, *end, *mb_start;
  idx_t len;
  char eol = eolbyte;
  struct kwsearch *kwsearch = vcp;
  kwset_t kwset = kwsearch->kwset;
  bool mb_check = localeinfo.multibyte & !localeinfo.using_utf8 & !match_lines;
  bool longest = (mb_check | !!start_ptr | match_words) & !match_lines;

  for (mb_start = beg = start_ptr ? start_ptr : buf; beg <= buf + size; beg++)
    {
      struct kwsmatch kwsmatch;
      ptrdiff_t offset = kwsexec (kwset, beg - match_lines,
                                  buf + size - beg + match_lines, &kwsmatch,
                                  longest);
      if (offset < 0)
        break;
      len = kwsmatch.size - 2 * match_lines;

      idx_t mbclen = 0;
      if (mb_check
          && mb_goback (&mb_start, &mbclen, beg + offset, buf + size) != 0)
        {
          /* We have matched a single byte that is not at the beginning of a
             multibyte character.  mb_goback has advanced MB_START past that
             multibyte character.  Now, we want to position BEG so that the
             next kwsexec search starts there.  Thus, to compensate for the
             for-loop's BEG++, above, subtract one here.  This code is
             unusually hard to reach, and exceptionally, let's show how to
             trigger it here:

               printf '\203AA\n'|LC_ALL=ja_JP.SHIFT_JIS src/grep -F A

             That assumes the named locale is installed.
             Note that your system's shift-JIS locale may have a different
             name, possibly including "sjis".  */
          beg = mb_start - 1;
          continue;
        }
      beg += offset;
      if (!!start_ptr & !match_words)
        goto success_in_beg_and_len;
      if (match_lines)
        {
          len += !start_ptr;
          goto success_in_beg_and_len;
        }
      if (! match_words)
        goto success;

      /* We need a preceding mb_start pointer.  Use the beginning of line
         if there is a preceding newline.  */
      if (mbclen == 0)
        {
          char const *nl = memrchr (mb_start, eol, beg - mb_start);
          if (nl)
            mb_start = nl + 1;
        }

      /* Succeed if neither the preceding nor the following character is a
         word constituent.  If the preceding is not, yet the following
         character IS a word constituent, keep trying with shorter matches.  */
      if (mbclen > 0
          ? ! wordchar_next (beg - mbclen, buf + size)
          : ! wordchar_prev (mb_start, beg, buf + size))
        for (;;)
          {
            if (! wordchar_next (beg + len, buf + size))
              {
                if (start_ptr)
                  goto success_in_beg_and_len;
                else
                  goto success;
              }
            if (!start_ptr && !localeinfo.multibyte)
              {
                if (! kwsearch->re)
                  {
                    fgrep_to_grep_pattern (&kwsearch->pattern, &kwsearch->size);
                    kwsearch->re = GEAcompile (kwsearch->pattern,
                                               kwsearch->size,
                                               RE_SYNTAX_GREP, !!start_ptr);
                  }
                if (beg + len < buf + size)
                  {
                    end = rawmemchr (beg + len, eol);
                    end++;
                  }
                else
                  end = buf + size;

                if (0 <= EGexecute (kwsearch->re, beg, end - beg,
                                    match_size, nullptr))
                  goto success_match_words;
                beg = end - 1;
                break;
              }
            if (!len)
              break;

            struct kwsmatch shorter_match;
            if (kwsexec (kwset, beg, --len, &shorter_match, true) != 0)
              break;
            len = shorter_match.size;
          }

      /* No word match was found at BEG.  Skip past word constituents,
         since they cannot precede the next match and not skipping
         them could make things much slower.  */
      beg += wordchars_size (beg, buf + size);
      mb_start = beg;
    }

  return -1;

 success:
  if (beg + len < buf + size)
    {
      end = rawmemchr (beg + len, eol);
      end++;
    }
  else
    end = buf + size;
 success_match_words:
  beg = memrchr (buf, eol, beg - buf);
  beg = beg ? beg + 1 : buf;
  len = end - beg;
 success_in_beg_and_len:;
  *match_size = len;
  return beg - buf;
}

void
Ffree (void *vcp)
{
  if (!vcp)
    return;
  struct kwsearch *kwsearch = vcp;
  struct shared_kwset *sk = kwsearch->shared;
  if (atomic_fetch_sub (&sk->refs, 1) == 1)
    {
      kwsfree (sk->kwset);
      free (sk->pattern);
      free (sk);
    }
  free (kwsearch->pattern);
  if (kwsearch->re)
    EGfree (kwsearch->re);
  free (kwsearch);
}
