/* grep.c - main driver file for grep.
   Copyright (C) 1992, 1997-2002, 2004-2026 Free Software Foundation, Inc.

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

/* Written July 1992 by Mike Haertel.  */

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <uchar.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <dirent.h>
#if defined __linux__ && HAVE_SYS_VFS_H && HAVE_FSTATFS
# include <sys/vfs.h>
#endif
#ifdef __GLIBC__
# include <stdio_ext.h>
#endif
#if defined __AVX2__
# include <immintrin.h>
#endif
#include "system.h"

#include "argmatch.h"
#include "c-ctype.h"
#include "c-stack.h"
#include "closeout.h"
#include "colorize.h"
#include "die.h"
#include <error.h>
#include "exclude.h"
#include "exitfail.h"
#include "fcntl-safer.h"
#include "fts_.h"
#include <getopt.h>
#include "grep.h"
#include "hash.h"
#include "intprops.h"
#include "safe-read.h"
#include <search.h>
#include "c-strcase.h"
#include "version-etc.h"
#include "xalloc.h"
#include "xbinary-io.h"
#include "xstrtol.h"

enum { SEP_CHAR_SELECTED = ':' };
enum { SEP_CHAR_REJECTED = '-' };
static char const SEP_STR_GROUP[] = "--";

/* When stdout is connected to a regular file, save its stat
   information here, so that we can automatically skip it, thus
   avoiding a potential (racy) infinite loop.  */
static struct stat out_stat;

/* if non-zero, display usage information and exit */
static int show_help;

/* Print the version on standard output and exit.  */
static bool show_version;

/* Suppress diagnostics for nonexistent or unreadable files.  */
static bool suppress_errors;

/* If nonzero, use color markers.  */
static int color_option;

/* Show only the part of a line matching the expression. */
static bool only_matching;

/* If nonzero, make sure first content char in a line is on a tab stop. */
static bool align_tabs;

/* Print width of line numbers and byte offsets.  Nonzero if ALIGN_TABS.  */
static _Thread_local int offset_width;

/* An entry in the PATLOC array saying where patterns came from.  */
struct patloc
  {
    /* Line number of the pattern in PATTERN_ARRAY.  Line numbers
       start at 0, and each pattern is terminated by '\n'.  */
    idx_t lineno;

    /* Input location of the pattern.  The FILENAME "-" represents
       standard input, and "" represents the command line.  FILELINE is
       origin-1 for files and is irrelevant for the command line.  */
    char const *filename;
    idx_t fileline;
  };

/* The array of pattern locations.  The concatenation of all patterns
   is stored in a single array, KEYS.  Given the invocation
   'grep -f <(seq 5) -f <(seq 6) -f <(seq 3)', there will initially be
   28 bytes in KEYS.  After duplicate patterns are removed, KEYS
   will have 12 bytes and PATLOC will be {0,x,1}, {10,y,1}
   where x, y and z are just place-holders for shell-generated names
   since and z is omitted as it contains only duplicates.  Sometimes
   removing duplicates will grow PATLOC, since each run of
   removed patterns not at a file start or end requires another
   PATLOC entry for the first non-removed pattern.  */
static struct patloc *patloc;
static idx_t patlocs_allocated, patlocs_used;

/* Pointer to the array of patterns, each terminated by newline.  */
static char *pattern_array;

/* The number of unique patterns seen so far.  */
static idx_t n_patterns;

/* Hash table of patterns seen so far.  */
static Hash_table *pattern_table;

/* Hash and compare newline-terminated patterns for textual equality.
   Patterns are represented by origin-1 offsets into PATTERN_ARRAY,
   cast to void *.  The origin-1 is so that the first pattern offset
   does not appear to be a null pointer when cast to void *.  */
static size_t _GL_ATTRIBUTE_PURE
hash_pattern (void const *pat, size_t n_buckets)
{
  /* This uses the djb2 algorithm, except starting with a larger prime
     in place of djb2's 5381, if size_t is wide enough.  The primes
     are taken from the primeth recurrence sequence
     <https://oeis.org/A007097>.  h15, h32 and h64 are the largest
     sequence members that fit into 15, 32 and 64 bits, respectively.
     Since any H will do, hashing works correctly on oddball machines
     where size_t has some other width.  */
  uint_fast64_t h15 = 5381, h32 = 3657500101, h64 = 4123221751654370051;
  size_t h = h64 <= SIZE_MAX ? h64 : h32 <= SIZE_MAX ? h32 : h15;
  intptr_t pat_offset = (intptr_t) pat - 1;
  unsigned char const *s = (unsigned char const *) pattern_array + pat_offset;
  for ( ; *s != '\n'; s++)
    h = h * 33 ^ *s;
  return h % n_buckets;
}
static bool _GL_ATTRIBUTE_PURE
compare_patterns (void const *a, void const *b)
{
  intptr_t a_offset = (intptr_t) a - 1;
  intptr_t b_offset = (intptr_t) b - 1;
  char const *p = pattern_array + a_offset;
  char const *q = pattern_array + b_offset;
  for (; *p == *q; p++, q++)
    if (*p == '\n')
      return true;
  return false;
}

/* Update KEYS to remove duplicate patterns, and return the number of
   bytes in the resulting KEYS.  KEYS contains a sequence of patterns
   each terminated by '\n'.  The first DUPFREE_SIZE bytes are a
   sequence of patterns with no duplicates; SIZE is the total number
   of bytes in KEYS.  If some patterns past the first DUPFREE_SIZE
   bytes are not duplicates, update PATLOCS accordingly.  */
static idx_t
update_patterns (char *keys, idx_t dupfree_size, idx_t size,
                 char const *filename)
{
  char *dst = keys + dupfree_size;
  idx_t fileline = 1;
  int prev_inserted = 0;

  char const *srclim = keys + size;
  idx_t patsize;
  for (char const *src = keys + dupfree_size; src < srclim; src += patsize)
    {
      char const *patend = rawmemchr (src, '\n');
      patsize = patend + 1 - src;
      memmove (dst, src, patsize);

      intptr_t dst_offset_1 = dst - keys + 1;
      int inserted = hash_insert_if_absent (pattern_table,
                                            (void *) dst_offset_1, nullptr);
      if (inserted)
        {
          if (inserted < 0)
            xalloc_die ();
          dst += patsize;

          /* Add a PATLOCS entry unless this input line is simply the
             next one in the same file.  */
          if (!prev_inserted)
            {
              if (patlocs_used == patlocs_allocated)
                patloc = xpalloc (patloc, &patlocs_allocated, 1, -1,
                                  sizeof *patloc);
              patloc[patlocs_used++]
                = (struct patloc) { .lineno = n_patterns,
                                    .filename = filename,
                                    .fileline = fileline };
            }
          n_patterns++;
        }

      prev_inserted = inserted;
      fileline++;
    }

  return dst - keys;
}

/* Map LINENO, the origin-0 line number of one of the input patterns,
   to the name of the file from which it came.  Return "-" if it was
   read from stdin, "" if it was specified on the command line.
   Set *NEW_LINENO to the origin-1 line number of PATTERN in the file,
   or to an unspecified value if PATTERN came from the command line.  */
char const * _GL_ATTRIBUTE_PURE
pattern_file_name (idx_t lineno, idx_t *new_lineno)
{
  idx_t i;
  for (i = 1; i < patlocs_used; i++)
    if (lineno < patloc[i].lineno)
      break;
  *new_lineno = lineno - patloc[i - 1].lineno + patloc[i - 1].fileline;
  return patloc[i - 1].filename;
}

#if HAVE_ASAN
/* Record the starting address and length of the sole poisoned region,
   so that we can unpoison it later, just before each following read.  */
static _Thread_local void const *poison_buf;
static _Thread_local idx_t poison_len;

static void
clear_asan_poison (void)
{
  if (poison_buf)
    __asan_unpoison_memory_region (poison_buf, poison_len);
}

static void
asan_poison (void const *addr, idx_t size)
{
  poison_buf = addr;
  poison_len = size;

  __asan_poison_memory_region (poison_buf, poison_len);
}
#else
static void clear_asan_poison (void) { }
static void asan_poison (void const volatile *addr, idx_t size) { }
#endif

/* The group separator used when context is requested. */
static const char *group_separator = SEP_STR_GROUP;

/* The context and logic for choosing default --color screen attributes
   (foreground and background colors, etc.) are the following.
      -- There are eight basic colors available, each with its own
         nominal luminosity to the human eye and foreground/background
         codes (black [0 %, 30/40], blue [11 %, 34/44], red [30 %, 31/41],
         magenta [41 %, 35/45], green [59 %, 32/42], cyan [70 %, 36/46],
         yellow [89 %, 33/43], and white [100 %, 37/47]).
      -- Sometimes, white as a background is actually implemented using
         a shade of light gray, so that a foreground white can be visible
         on top of it (but most often not).
      -- Sometimes, black as a foreground is actually implemented using
         a shade of dark gray, so that it can be visible on top of a
         background black (but most often not).
      -- Sometimes, more colors are available, as extensions.
      -- Other attributes can be selected/deselected (bold [1/22],
         underline [4/24], standout/inverse [7/27], blink [5/25], and
         invisible/hidden [8/28]).  They are sometimes implemented by
         using colors instead of what their names imply; e.g., bold is
         often achieved by using brighter colors.  In practice, only bold
         is really available to us, underline sometimes being mapped by
         the terminal to some strange color choice, and standout best
         being left for use by downstream programs such as less(1).
      -- We cannot assume that any of the extensions or special features
         are available for the purpose of choosing defaults for everyone.
      -- The most prevalent default terminal backgrounds are pure black
         and pure white, and are not necessarily the same shades of
         those as if they were selected explicitly with SGR sequences.
         Some terminals use dark or light pictures as default background,
         but those are covered over by an explicit selection of background
         color with an SGR sequence; their users will appreciate their
         background pictures not be covered like this, if possible.
      -- Some uses of colors attributes is to make some output items
         more understated (e.g., context lines); this cannot be achieved
         by changing the background color.
      -- For these reasons, the grep color defaults should strive not
         to change the background color from its default, unless it's
         for a short item that should be highlighted, not understated.
      -- The grep foreground color defaults (without an explicitly set
         background) should provide enough contrast to be readable on any
         terminal with either a black (dark) or white (light) background.
         This only leaves red, magenta, green, and cyan (and their bold
         counterparts) and possibly bold blue.  */
/* The color strings used for matched text.
   The user can overwrite them using the GREP_COLORS environment variable.  */
static const char *selected_match_color = "01;31";	/* bold red */
static const char *context_match_color  = "01;31";	/* bold red */

/* Other colors.  Defaults look damn good.  */
static const char *filename_color = "35";	/* magenta */
static const char *line_num_color = "32";	/* green */
static const char *byte_num_color = "32";	/* green */
static const char *sep_color      = "36";	/* cyan */
static const char *selected_line_color = "";	/* default color pair */
static const char *context_line_color  = "";	/* default color pair */

/* Select Graphic Rendition (SGR, "\33[...m") strings.  */
/* Also Erase in Line (EL) to Right ("\33[K") by default.  */
/*    Why have EL to Right after SGR?
         -- The behavior of line-wrapping when at the bottom of the
            terminal screen and at the end of the current line is often
            such that a new line is introduced, entirely cleared with
            the current background color which may be different from the
            default one (see the boolean back_color_erase terminfo(5)
            capability), thus scrolling the display by one line.
            The end of this new line will stay in this background color
            even after reverting to the default background color with
            "\33[m', unless it is explicitly cleared again with "\33[K"
            (which is the behavior the user would instinctively expect
            from the whole thing).  There may be some unavoidable
            background-color flicker at the end of this new line because
            of this (when timing with the monitor's redraw is just right).
         -- The behavior of HT (tab, "\t") is usually the same as that of
            Cursor Forward Tabulation (CHT) with a default parameter
            of 1 ("\33[I"), i.e., it performs pure movement to the next
            tab stop, without any clearing of either content or screen
            attributes (including background color); try
               printf 'asdfqwerzxcv\rASDF\tZXCV\n'
            in a bash(1) shell to demonstrate this.  This is not what the
            user would instinctively expect of HT (but is ok for CHT).
            The instinctive behavior would include clearing the terminal
            cells that are skipped over by HT with blank cells in the
            current screen attributes, including background color;
            the boolean dest_tabs_magic_smso terminfo(5) capability
            indicates this saner behavior for HT, but only some rare
            terminals have it (although it also indicates a special
            glitch with standout mode in the Teleray terminal for which
            it was initially introduced).  The remedy is to add "\33K"
            after each SGR sequence, be it START (to fix the behavior
            of any HT after that before another SGR) or END (to fix the
            behavior of an HT in default background color that would
            follow a line-wrapping at the bottom of the screen in another
            background color, and to complement doing it after START).
            Piping grep's output through a pager such as less(1) avoids
            any HT problems since the pager performs tab expansion.

      Generic disadvantages of this remedy are:
         -- Some very rare terminals might support SGR but not EL (nobody
            will use "grep --color" on a terminal that does not support
            SGR in the first place).
         -- Having these extra control sequences might somewhat complicate
            the task of any program trying to parse "grep --color"
            output in order to extract structuring information from it.
      A specific disadvantage to doing it after SGR START is:
         -- Even more possible background color flicker (when timing
            with the monitor's redraw is just right), even when not at the
            bottom of the screen.
      There are no additional disadvantages specific to doing it after
      SGR END.

      It would be impractical for GNU grep to become a full-fledged
      terminal program linked against ncurses or the like, so it will
      not detect terminfo(5) capabilities.  */
static const char *sgr_start = "\33[%sm\33[K";
static const char *sgr_end   = "\33[m\33[K";

static void putchar_errno (int c);
static void fputs_errno (char const *s);
static void printf_errno (char const *format, ...) _GL_ATTRIBUTE_FORMAT_PRINTF_STANDARD (1, 2);

/* SGR utility functions.  */
static void
pr_sgr_start (char const *s)
{
  if (*s)
    printf_errno (sgr_start, s);
}
static void
pr_sgr_end (char const *s)
{
  if (*s)
    fputs_errno (sgr_end);
}
static void
pr_sgr_start_if (char const *s)
{
  if (color_option)
    pr_sgr_start (s);
}
static void
pr_sgr_end_if (char const *s)
{
  if (color_option)
    pr_sgr_end (s);
}

struct color_cap
  {
    const char *name;
    const char **var;
    void (*fct) (void);
  };

static void
color_cap_mt_fct (void)
{
  /* Our caller just set selected_match_color.  */
  context_match_color = selected_match_color;
}

static void
color_cap_rv_fct (void)
{
  /* By this point, it was 1 (or already -1).  */
  color_option = -1;  /* That's still != 0.  */
}

static void
color_cap_ne_fct (void)
{
  sgr_start = "\33[%sm";
  sgr_end   = "\33[m";
}

/* For GREP_COLORS.  */
static const struct color_cap color_dict[] =
  {
    { "mt", &selected_match_color, color_cap_mt_fct }, /* both ms/mc */
    { "ms", &selected_match_color, nullptr }, /* selected matched text */
    { "mc", &context_match_color,  nullptr }, /* context matched text */
    { "fn", &filename_color,       nullptr }, /* filename */
    { "ln", &line_num_color,       nullptr }, /* line number */
    { "bn", &byte_num_color,       nullptr }, /* byte (sic) offset */
    { "se", &sep_color,            nullptr }, /* separator */
    { "sl", &selected_line_color,  nullptr }, /* selected lines */
    { "cx", &context_line_color,   nullptr }, /* context lines */
    { "rv", nullptr,               color_cap_rv_fct }, /* -v reverses sl/cx */
    { "ne", nullptr,               color_cap_ne_fct }, /* no EL on SGR_* */
    { nullptr, nullptr,            nullptr }
  };

/* Saved errno value from failed output functions on stdout.
   prline polls this to decide whether to die.
   Setting it to nonzero just before exiting can prevent clean_up_stdout
   from misbehaving on a buggy OS where 'close (STDOUT_FILENO)' fails
   with EACCES.  */
static int stdout_errno;

struct membuf
{
  char *data;
  size_t len;
  size_t cap;
};

static _Thread_local bool buffering_enabled;
static _Thread_local struct membuf tls_out;
static _Thread_local struct membuf tls_err;
static _Thread_local bool tls_errseen;

/* True if the current buffered task called prtext without OUT_QUIET
   before any other prtext call.  Whether a group separator precedes
   that output depends on what was output before the task, which only
   the committer knows.  */
static _Thread_local bool task_sep;

/* True if output committed from worker buffers must reach standard
   output through the same sequence of stdio buffer states as in
   single-threaded grep.  This matters when standard output is a
   regular file that grep may read as input, which happens with -c,
   -l, -L or -m1, and is cheap since output is small then.  */
static bool exact_stdout;

/* While buffering in exact mode, each write of at least BIG_WRITE_MIN
   bytes is recorded in TLS_BIG as a pair of size_t values (offset in
   TLS_OUT, length), so that it can be replayed as one write.  Smaller
   writes never bypass a stdio buffer that is at least that large, so
   they can be replayed in any pieces that are smaller too.  */
enum { BIG_WRITE_MIN = 128 };
static _Thread_local struct membuf tls_big;

/* In exact mode, worker threads must not read the output file, since
   what they would read depends on how much output has been committed.
   If TLS_MAY_DEFER, grepdesc leaves such a file alone and sets
   TLS_DEFERRED; the committer then searches the file itself, when all
   the output that precedes it has been written.  */
static _Thread_local bool tls_may_defer;
static _Thread_local bool tls_deferred;

static void
membuf_init (struct membuf *mb, size_t initial_cap)
{
  mb->data = xmalloc (initial_cap);
  mb->len = 0;
  mb->cap = initial_cap;
}

static void
membuf_reset (struct membuf *mb)
{
  mb->len = 0;
}

static void
membuf_append (struct membuf *mb, void const *src, size_t n)
{
  if (n == 0)
    return;
  if (mb->len + n > mb->cap)
    {
      size_t new_cap = MAX (mb->cap * 2, mb->len + n + 65536);
      mb->data = xrealloc (mb->data, new_cap);
      mb->cap = new_cap;
    }
  memcpy (mb->data + mb->len, src, n);
  mb->len += n;
}

static void
membuf_free (struct membuf *mb)
{
  free (mb->data);
  mb->data = nullptr;
  mb->len = mb->cap = 0;
}

static void
putchar_errno (int c)
{
  if (buffering_enabled)
    {
      char ch = (char) c;
      membuf_append (&tls_out, &ch, 1);
      return;
    }
  if (putchar (c) < 0)
    stdout_errno = errno;
}

/* Record a buffered write of LEN bytes that is about to be appended to
   TLS_OUT, if exact mode needs to know about it.  */
static void
note_big_write (size_t len)
{
  if (exact_stdout && BIG_WRITE_MIN <= len)
    {
      size_t rec[2] = { tls_out.len, len };
      membuf_append (&tls_big, rec, sizeof rec);
    }
}

static void
fputs_errno (char const *s)
{
  if (buffering_enabled)
    {
      size_t len = strlen (s);
      note_big_write (len);
      membuf_append (&tls_out, s, len);
      return;
    }
  if (fputs (s, stdout) < 0)
    stdout_errno = errno;
}

static void _GL_ATTRIBUTE_FORMAT_PRINTF_STANDARD (1, 2)
printf_errno (char const *format, ...)
{
  va_list ap;
  va_start (ap, format);
  if (buffering_enabled)
    {
      char buf[512];
      va_list ap2;
      va_copy (ap2, ap);
      int n = vsnprintf (buf, sizeof buf, format, ap);
      if (n >= 0 && (size_t) n < sizeof buf)
        {
          membuf_append (&tls_out, buf, n);
        }
      else if (n >= 0)
        {
          char *dyn = xmalloc (n + 1);
          vsnprintf (dyn, n + 1, format, ap2);
          membuf_append (&tls_out, dyn, n);
          free (dyn);
        }
      va_end (ap2);
    }
  else
    {
      if (vfprintf (stdout, format, ap) < 0)
        stdout_errno = errno;
    }
  va_end (ap);
}

static void
fwrite_errno (void const *ptr, idx_t size, idx_t nmemb)
{
  if (buffering_enabled)
    {
      size_t total;
      if (!ckd_mul (&total, size, nmemb) && total > 0)
        {
          note_big_write (total);
          membuf_append (&tls_out, ptr, total);
        }
      return;
    }
  if (fwrite (ptr, size, nmemb, stdout) != nmemb)
    stdout_errno = errno;
}

static void
fflush_errno (void)
{
  if (buffering_enabled)
    return;
  if (fflush (stdout) != 0)
    stdout_errno = errno;
}

/* Return the result of formatting FMT and AP as a string.  If it fits
   in BUF of size BUFSIZE, store it there; otherwise allocate it.  */
static char * _GL_ATTRIBUTE_FORMAT_PRINTF_STANDARD (3, 0)
format_message (char *buf, size_t bufsize, char const *fmt, va_list ap)
{
  va_list ap2;
  va_copy (ap2, ap);
  int n = vsnprintf (buf, bufsize, fmt, ap);
  if (n < 0)
    {
      buf[0] = '\0';
      n = 0;
    }
  else if (bufsize <= (size_t) n)
    {
      buf = ximalloc ((idx_t) n + 1);
      vsnprintf (buf, (size_t) n + 1, fmt, ap2);
    }
  va_end (ap2);
  return buf;
}

/* The program name that error () outputs before each diagnostic.
   glibc uses argv[0] as given; gnulib's replacement uses getprogname.  */
static char const *
diag_program_name (void)
{
#ifdef __GLIBC__
  return program_invocation_name;
#else
  return getprogname ();
#endif
}

/* Return a newly allocated copy of the diagnostic that
   error (0, ERRNUM, "%s", MSG) would output, and store its length into
   *LENP.  */
static char *
diagnostic_text (int errnum, char const *msg, size_t *lenp)
{
  char const *prog = diag_program_name ();
  char const *err = errnum ? strerror (errnum) : nullptr;
  size_t proglen = strlen (prog), msglen = strlen (msg);
  size_t errlen = err ? strlen (err) : 0;
  size_t len = proglen + 2 + msglen + (err ? 2 + errlen : 0) + 1;
  char *text = ximalloc (len + 1);
  char *p = mempcpy (text, prog, proglen);
  p = mempcpy (p, ": ", 2);
  p = mempcpy (p, msg, msglen);
  if (err)
    {
      p = mempcpy (p, ": ", 2);
      p = mempcpy (p, err, errlen);
    }
  *p++ = '\n';
  *p = '\0';
  *lenp = len;
  return text;
}

/* A diagnostic captured while buffering.  It is stored in a membuf as
   this header followed by LEN bytes of text, and must be written to
   stderr after OUTPOS bytes of the same task's buffered output.
   AFTER_SEP tells whether it was issued after the point where a group
   separator decided by the committer would be output (see TASK_SEP).  */
struct errrec
{
  size_t outpos;
  size_t len;
  bool after_sep;
};

/* Append to MB a record for diagnostic TEXT of length LEN issued after
   OUTPOS bytes of output, and after the deferred group separator if
   AFTER_SEP.  */
static void
errrec_add (struct membuf *mb, size_t outpos, bool after_sep,
            char const *text, size_t len)
{
  struct errrec h = { .outpos = outpos, .len = len, .after_sep = after_sep };
  membuf_append (mb, &h, sizeof h);
  membuf_append (mb, text, len);
}

static void _GL_ATTRIBUTE_FORMAT_PRINTF_STANDARD (2, 3)
task_error_buffered (int errnum, char const *fmt, ...)
{
  char buf[1024];
  va_list ap;
  va_start (ap, fmt);
  char *msg = format_message (buf, sizeof buf, fmt, ap);
  va_end (ap);

  size_t len;
  char *text = diagnostic_text (errnum, msg, &len);
  errrec_add (&tls_err, tls_out.len, task_sep, text, len);
  free (text);
  if (msg != buf)
    free (msg);
}

#define task_error(errnum, ...) \
  do { \
    if (buffering_enabled) \
      task_error_buffered ((errnum), __VA_ARGS__); \
    else \
      error (0, (errnum), __VA_ARGS__); \
  } while (0)

static struct exclude *excluded_patterns[2];
static struct exclude *excluded_directory_patterns[2];
/* Short options.  */
static char const short_options[] =
"0123456789A:B:C:D:EFGHIPTUVX:abcd:e:f:hiLlm:noqRrsuvwxyZz";

/* Non-boolean long options that have no corresponding short equivalents.  */
enum
{
  BINARY_FILES_OPTION = CHAR_MAX + 1,
  COLOR_OPTION,
  EXCLUDE_DIRECTORY_OPTION,
  EXCLUDE_OPTION,
  EXCLUDE_FROM_OPTION,
  GROUP_SEPARATOR_OPTION,
  INCLUDE_OPTION,
  LINE_BUFFERED_OPTION,
  LABEL_OPTION,
  NO_IGNORE_CASE_OPTION
};

/* Long options equivalences. */
static struct option const long_options[] =
{
  {"basic-regexp",    no_argument, nullptr, 'G'},
  {"extended-regexp", no_argument, nullptr, 'E'},
  {"fixed-regexp",    no_argument, nullptr, 'F'},
  {"fixed-strings",   no_argument, nullptr, 'F'},
  {"perl-regexp",     no_argument, nullptr, 'P'},
  {"after-context", required_argument, nullptr, 'A'},
  {"before-context", required_argument, nullptr, 'B'},
  {"binary-files", required_argument, nullptr, BINARY_FILES_OPTION},
  {"byte-offset", no_argument, nullptr, 'b'},
  {"context", required_argument, nullptr, 'C'},
  {"color", optional_argument, nullptr, COLOR_OPTION},
  {"colour", optional_argument, nullptr, COLOR_OPTION},
  {"count", no_argument, nullptr, 'c'},
  {"devices", required_argument, nullptr, 'D'},
  {"directories", required_argument, nullptr, 'd'},
  {"exclude", required_argument, nullptr, EXCLUDE_OPTION},
  {"exclude-from", required_argument, nullptr, EXCLUDE_FROM_OPTION},
  {"exclude-dir", required_argument, nullptr, EXCLUDE_DIRECTORY_OPTION},
  {"file", required_argument, nullptr, 'f'},
  {"files-with-matches", no_argument, nullptr, 'l'},
  {"files-without-match", no_argument, nullptr, 'L'},
  {"group-separator", required_argument, nullptr, GROUP_SEPARATOR_OPTION},
  {"help", no_argument, &show_help, 1},
  {"include", required_argument, nullptr, INCLUDE_OPTION},
  {"ignore-case", no_argument, nullptr, 'i'},
  {"no-ignore-case", no_argument, nullptr, NO_IGNORE_CASE_OPTION},
  {"initial-tab", no_argument, nullptr, 'T'},
  {"label", required_argument, nullptr, LABEL_OPTION},
  {"line-buffered", no_argument, nullptr, LINE_BUFFERED_OPTION},
  {"line-number", no_argument, nullptr, 'n'},
  {"line-regexp", no_argument, nullptr, 'x'},
  {"max-count", required_argument, nullptr, 'm'},

  {"no-filename", no_argument, nullptr, 'h'},
  {"no-group-separator", no_argument, nullptr, GROUP_SEPARATOR_OPTION},
  {"no-messages", no_argument, nullptr, 's'},
  {"null", no_argument, nullptr, 'Z'},
  {"null-data", no_argument, nullptr, 'z'},
  {"only-matching", no_argument, nullptr, 'o'},
  {"quiet", no_argument, nullptr, 'q'},
  {"recursive", no_argument, nullptr, 'r'},
  {"dereference-recursive", no_argument, nullptr, 'R'},
  {"regexp", required_argument, nullptr, 'e'},
  {"invert-match", no_argument, nullptr, 'v'},
  {"silent", no_argument, nullptr, 'q'},
  {"text", no_argument, nullptr, 'a'},
  {"binary", no_argument, nullptr, 'U'},
  {"version", no_argument, nullptr, 'V'},
  {"with-filename", no_argument, nullptr, 'H'},
  {"word-regexp", no_argument, nullptr, 'w'},
  {nullptr, 0, nullptr, 0}
};

/* Define flags declared in grep.h. */
bool match_icase;
bool match_words;
bool match_lines;
char eolbyte;

/* For error messages. */
/* The input file name, or (if standard input) null or a --label argument.  */
static _Thread_local char const *filename;
/* Omit leading "./" from file names in diagnostics.  */
static bool omit_dot_slash;
static bool errseen;

/* True if output from the current input file has been suppressed
   because an output line had an encoding error.  */
static _Thread_local bool encoding_error_output;

enum directories_type
  {
    READ_DIRECTORIES = 2,
    RECURSE_DIRECTORIES,
    SKIP_DIRECTORIES
  };

/* How to handle directories.  */
static char const *const directories_args[] =
{
  "read", "recurse", "skip", nullptr
};
static enum directories_type const directories_types[] =
{
  READ_DIRECTORIES, RECURSE_DIRECTORIES, SKIP_DIRECTORIES
};
ARGMATCH_VERIFY (directories_args, directories_types);

static enum directories_type directories = READ_DIRECTORIES;

enum { basic_fts_options = FTS_CWDFD | FTS_NOSTAT | FTS_TIGHT_CYCLE_CHECK };
static int fts_options = basic_fts_options | FTS_COMFOLLOW | FTS_PHYSICAL;

/* How to handle devices. */
static enum
  {
    READ_COMMAND_LINE_DEVICES,
    READ_DEVICES,
    SKIP_DEVICES
  } devices = READ_COMMAND_LINE_DEVICES;

static bool grepfile (int, char const *, bool, bool);
static bool grepdesc (int, bool);

static bool
is_device_mode (mode_t m)
{
  return S_ISCHR (m) || S_ISBLK (m) || S_ISSOCK (m) || S_ISFIFO (m);
}

static bool
skip_devices (bool command_line)
{
  return (devices == SKIP_DEVICES
          || ((devices == READ_COMMAND_LINE_DEVICES) & !command_line));
}

/* Return if ST->st_size is defined.  Assume the file is not a
   symbolic link.  */
static bool
usable_st_size (struct stat const *st)
{
  return S_ISREG (st->st_mode) || S_TYPEISSHM (st) || S_TYPEISTMO (st);
}

/* Lame substitutes for SEEK_DATA and SEEK_HOLE on platforms lacking them.
   Do not rely on these finding data or holes if they equal SEEK_SET.  */
#ifndef SEEK_DATA
enum { SEEK_DATA = SEEK_SET };
#endif
#ifndef SEEK_HOLE
enum { SEEK_HOLE = SEEK_SET };
#endif

/* True if lseek with SEEK_CUR or SEEK_DATA failed on the current input.  */
static _Thread_local bool seek_failed;
static _Thread_local bool seek_data_failed;

/* Functions we'll use to search. */
typedef void *(*compile_fp_t) (char *, idx_t, reg_syntax_t, bool);
typedef ptrdiff_t (*execute_fp_t) (void *, char const *, idx_t, idx_t *,
                                   char const *);
static execute_fp_t execute;
static _Thread_local void *compiled_pattern;
static void *primary_compiled_pattern;
static char *raw_keys;
static idx_t raw_keycc;
static reg_syntax_t raw_matcher_syntax;
static int raw_matcher_index;
static bool raw_exact;

char const *
input_filename (void)
{
  if (!filename)
    filename = _("(standard input)");
  return filename;
}

/* Unless requested, diagnose an error about the input file.  */
static void
suppressible_error (int errnum)
{
  if (! suppress_errors)
    task_error (errnum, "%s", input_filename ());
  if (buffering_enabled)
    tls_errseen = true;
  else
    errseen = true;
}

/* If there has already been a write error, don't bother closing
   standard output, as that might elicit a duplicate diagnostic.  */
static void
clean_up_stdout (void)
{
  if (! stdout_errno)
    close_stdout ();
}

/* A cast to TYPE of VAL.  Use this when TYPE is a pointer type, VAL
   is properly aligned for TYPE, and 'gcc -Wcast-align' cannot infer
   the alignment and would otherwise complain about the cast.  */
#if 4 < __GNUC__ + (6 <= __GNUC_MINOR__)
# define CAST_ALIGNED(type, val)                           \
    ({ __typeof__ (val) val_ = val;                        \
       _Pragma ("GCC diagnostic push")                     \
       _Pragma ("GCC diagnostic ignored \"-Wcast-align\"") \
       (type) val_;                                        \
       _Pragma ("GCC diagnostic pop")                      \
    })
#else
# define CAST_ALIGNED(type, val) ((type) (val))
#endif

/* An unsigned type suitable for fast matching.  */
typedef uintmax_t uword;
static uword const uword_max = UINTMAX_MAX;
enum { uword_size = sizeof (uword) }; /* For when a signed size is wanted.  */

struct localeinfo localeinfo;

/* A mask to test for unibyte characters, with the pattern repeated to
   fill a uword.  For a multibyte character encoding where
   all bytes are unibyte characters, this is 0.  For UTF-8, this is
   0x808080....  For encodings where unibyte characters have no discerned
   pattern, this is all 1s.  The unsigned char C is a unibyte
   character if C & UNIBYTE_MASK is zero.  If the uword W is the
   concatenation of bytes, the bytes are all unibyte characters
   if W & UNIBYTE_MASK is zero.  */
static uword unibyte_mask;

static void
initialize_unibyte_mask (void)
{
  /* For each encoding error I that MASK does not already match,
     accumulate I's most significant 1 bit by ORing it into MASK.
     Although any 1 bit of I could be used, in practice high-order
     bits work better.  */
  unsigned char mask = 0;
  int ms1b = 1;
  for (int i = 1; i <= UCHAR_MAX; i++)
    if ((localeinfo.sbclen[i] != 1) & ! (mask & i))
      {
        while (ms1b * 2 <= i)
          ms1b *= 2;
        mask |= ms1b;
      }

  /* Now MASK will detect any encoding-error byte, although it may
     cry wolf and it may not be optimal.  Build a uword-length mask by
     repeating MASK.  */
  unibyte_mask = uword_max / UCHAR_MAX * mask;
}

/* Skip the easy bytes in a buffer that is guaranteed to have a sentinel
   that is not easy, and return a pointer to the first non-easy byte.
   The easy bytes all have UNIBYTE_MASK off.  */
static char const * _GL_ATTRIBUTE_PURE
skip_easy_bytes (char const *buf)
{
  /* Search a byte at a time until the pointer is aligned, then a
     uword at a time until a match is found, then a byte at a time to
     identify the exact byte.  The uword search may go slightly past
     the buffer end, but that's benign.  */
  char const *p;
  uword const *s;
  for (p = buf; (uintptr_t) p % uword_size != 0; p++)
    if (to_uchar (*p) & unibyte_mask)
      return p;
  for (s = CAST_ALIGNED (uword const *, p); ! (*s & unibyte_mask); s++)
    continue;
  for (p = (char const *) s; ! (to_uchar (*p) & unibyte_mask); p++)
    continue;
  return p;
}

/* Return true if BUF, of size SIZE, has an encoding error.
   BUF must be followed by at least uword_size bytes,
   the first of which may be modified.  */
static bool
buf_has_encoding_errors (char *buf, idx_t size)
{
  if (! unibyte_mask)
    return false;

  /* Fast path: if the line contains only ASCII / easy bytes, it cannot have encoding errors.
     Scan without modifying buf to avoid CoW page dirtying. */
  char const *p = buf;
  char const *lim = buf + size;
  while (p < lim && ! (to_uchar (*p) & unibyte_mask))
    p++;
  if (p == lim)
    return false;

  mbstate_t mbs; mbszero (&mbs);
  ptrdiff_t clen;

  char saved = buf[size];
  if (saved != -1)
    buf[size] = -1;
  for (p = buf; (p = skip_easy_bytes (p)) < buf + size; p += clen)
    {
      clen = imbrlen (p, buf + size - p, &mbs);
      if (clen < 0)
        {
          if (saved != -1)
            buf[size] = saved;
          return true;
        }
    }

  if (saved != -1)
    buf[size] = saved;
  return false;
}


/* Return true if BUF, of size SIZE, has a null byte.
   BUF must be followed by at least one byte,
   which may be arbitrarily written to or read from.  */
static bool
buf_has_nulls (char *buf, idx_t size)
{
  buf[size] = 0;
  return memchr (buf, '\0', size) != nullptr;
}

/* Return true if a file is known to contain null bytes.
   SIZE bytes have already been read from the file
   with descriptor FD and status ST.  */
static bool
file_must_have_nulls (idx_t size, int fd, struct stat const *st)
{
  /* If the file has holes, it must contain a null byte somewhere.  */
  if (SEEK_HOLE != SEEK_SET && !seek_failed
      && usable_st_size (st) && size < st->st_size)
    {
      off_t cur = size;
      if (O_BINARY || fd == STDIN_FILENO)
        {
          cur = lseek (fd, 0, SEEK_CUR);
          if (cur < 0)
            return false;
        }

      /* Look for a hole after the current location.  */
      off_t hole_start = lseek (fd, cur, SEEK_HOLE);
      if (0 <= hole_start)
        {
          if (lseek (fd, cur, SEEK_SET) < 0)
            suppressible_error (errno);
          if (hole_start < st->st_size)
            return true;
        }
    }

  return false;
}

/* Convert STR to a nonnegative integer, storing the result in *OUT.
   STR must be a valid context length argument; report an error if it
   isn't.  Silently ceiling *OUT at the maximum value, as that is
   practically equivalent to infinity for grep's purposes.  */
static void
context_length_arg (char const *str, intmax_t *out)
{
  switch (xstrtoimax (str, nullptr, 10, out, ""))
    {
    case LONGINT_OK:
    case LONGINT_OVERFLOW:
      if (0 <= *out)
        break;
      FALLTHROUGH;
    default:
      die (EXIT_TROUBLE, 0, "%s: %s", str,
           _("invalid context length argument"));
    }
}

/* Return the add_exclude options suitable for excluding a file name.
   If COMMAND_LINE, it is a command-line file name.  */
static int
exclude_options (bool command_line)
{
  return EXCLUDE_WILDCARDS | (command_line ? 0 : EXCLUDE_ANCHORED);
}

/* Return true if the file with NAME should be skipped.
   If COMMAND_LINE, it is a command-line argument.
   If IS_DIR, it is a directory.  */
static bool
skipped_file (char const *name, bool command_line, bool is_dir)
{
  struct exclude **pats;
  if (! is_dir)
    pats = excluded_patterns;
  else if (directories == SKIP_DIRECTORIES)
    return true;
  else if (command_line && omit_dot_slash)
    return false;
  else
    pats = excluded_directory_patterns;
  return pats[command_line] && excluded_file_name (pats[command_line], name);
}

/* Hairy buffering mechanism for grep.  The intent is to keep
   all reads aligned on a page boundary and multiples of the
   page size, unless a read yields a partial page.  */

static _Thread_local char *buffer;		/* Base of buffer. */
static _Thread_local idx_t bufalloc;		/* Allocated buffer size, counting slop. */
static _Thread_local int bufdesc;		/* File descriptor. */
static _Thread_local char *bufbeg;		/* Beginning of user-visible stuff. */
static _Thread_local char *buflim;		/* Limit of user-visible stuff. */
static idx_t pagesize;		/* alignment of memory pages */
static _Thread_local idx_t good_readsize;	/* good size to pass to 'read' */
static _Thread_local off_t bufoffset;		/* Read offset.  */
static _Thread_local off_t after_last_match;	/* Pointer after last matching line that
                                   would have been output if we were
                                   outputting characters. */
static _Thread_local bool skip_nuls;		/* Skip '\0' in data.  */
static bool skip_empty_lines;	/* Skip empty lines in data.  */
static _Thread_local intmax_t totalnl;	/* Total newline count before lastnl. */
static _Thread_local bool buffer_is_mmap;

/* True if the current input is known to be at end of file, because a
   single read returned the whole of a nonempty regular file and left
   room in the buffer.  The read that would return 0 is then skipped.  */
static _Thread_local bool eof_known;

/* Minimum value for good_readsize.
   If it's too small, there are more syscalls;
   if too large, it wastes memory and likely cache.
   Use 96 KiB as it gave good results in a benchmark in 2018
   (see 2018-09-06 commit labeled "grep: triple initial buffer size: 32k->96k")
   even though the same benchmark in 2024 found no significant
   difference for values from 32 KiB to 1024 KiB on Ubuntu 24.04.1 LTS
   with an Intel Xeon W-1350.  */
enum { GOOD_READSIZE_MIN = 96 * 1024 };

/* Return VAL aligned to the next multiple of ALIGNMENT.  VAL can be
   an integer or a pointer.  Both args must be free of side effects.  */
#define ALIGN_TO(val, alignment) \
  ((uintptr_t) (val) % (alignment) == 0 \
   ? (val) \
   : (val) + ((alignment) - (uintptr_t) (val) % (alignment)))

static void
init_worker_buffer (void)
{
  if (buffer)
    return;

  size_t target_sizes[] = {
    16 * 1024 * 1024,
    2 * 1024 * 1024
  };

  for (size_t i = 0; i < sizeof target_sizes / sizeof target_sizes[0]; ++i)
    {
      size_t slab = target_sizes[i];
      size_t alloc_sz = slab + pagesize + uword_size;
      void *ptr = mmap (NULL, alloc_sz, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
      if (ptr != MAP_FAILED)
        {
          buffer = ptr;
          bufalloc = alloc_sz;
          buffer_is_mmap = true;
          good_readsize = ALIGN_TO (GOOD_READSIZE_MIN, pagesize);
          return;
        }
    }

  good_readsize = ALIGN_TO (GOOD_READSIZE_MIN, pagesize);
  bufalloc = 2 * good_readsize + pagesize + uword_size;
  buffer = ximalloc (bufalloc);
  buffer_is_mmap = false;
}

static void
free_worker_buffer (void)
{
  if (buffer)
    {
      if (buffer_is_mmap)
        munmap (buffer, bufalloc);
      else
        free (buffer);
      buffer = nullptr;
      bufalloc = 0;
      buffer_is_mmap = false;
    }
}

/* Add two numbers that count input bytes or lines, and report an
   error if the addition overflows.  */
static intmax_t
add_count (intmax_t a, idx_t b)
{
  intmax_t sum;
  if (ckd_add (&sum, a, b))
    die (EXIT_TROUBLE, 0, _("input is too large to count"));
  return sum;
}

/* Return true if BUF (of size SIZE) is all zeros.  */
static bool _GL_ATTRIBUTE_PURE
all_zeros (char const *buf, idx_t size)
{
  char const *p = buf;
  char const *lim = buf + size;

#if defined __AVX2__
  while (lim - p >= 128)
    {
      __m256i v0 = _mm256_loadu_si256 ((__m256i const *) (void const *) (p + 0));
      __m256i v1 = _mm256_loadu_si256 ((__m256i const *) (void const *) (p + 32));
      __m256i v2 = _mm256_loadu_si256 ((__m256i const *) (void const *) (p + 64));
      __m256i v3 = _mm256_loadu_si256 ((__m256i const *) (void const *) (p + 96));
      __m256i or01 = _mm256_or_si256 (v0, v1);
      __m256i or23 = _mm256_or_si256 (v2, v3);
      __m256i combined = _mm256_or_si256 (or01, or23);
      if (!_mm256_testz_si256 (combined, combined))
        return false;
      p += 128;
    }
  while (lim - p >= 32)
    {
      __m256i v = _mm256_loadu_si256 ((__m256i const *) (void const *) p);
      if (!_mm256_testz_si256 (v, v))
        return false;
      p += 32;
    }
#endif

  while (p < lim)
    {
      if (*p)
        return false;
      p++;
    }
  return true;
}

/* Reset the buffer for a new file, returning false if we should skip it.
   Initialize on the first time through. */
static bool
reset (int fd, struct stat const *st)
{
  init_worker_buffer ();
  bufbeg = buflim = ALIGN_TO (buffer + 1, pagesize);
  bufbeg[-1] = eolbyte;
  bufdesc = fd;
  eof_known = false;
#ifdef F_SETPIPE_SZ
  if (fd == STDIN_FILENO)
    fcntl (STDIN_FILENO, F_SETPIPE_SZ, 1024 * 1024);
#endif
  bufoffset = fd == STDIN_FILENO ? lseek (fd, 0, SEEK_CUR) : 0;
  seek_failed = bufoffset < 0;

  /* Assume SEEK_DATA fails if SEEK_CUR does.  */
  seek_data_failed = seek_failed;

  if (seek_failed)
    {
      if (errno != ESPIPE)
        {
          suppressible_error (errno);
          return false;
        }
      bufoffset = 0;
    }
  return true;
}

/* Read new stuff into the buffer, saving the specified
   amount of old stuff.  When we're done, 'bufbeg' points
   to the beginning of the buffer contents, and 'buflim'
   points just after the end.  Return false if there's an error.  */
static bool
fillbuf (idx_t save, struct stat const *st)
{
  char *readbuf;

  /* After BUFLIM, we need room for a good-sized read plus a
     trailing uword.  */
  idx_t min_after_buflim = good_readsize + uword_size;

  if (min_after_buflim <= buffer + bufalloc - buflim)
    readbuf = buflim;
  else
    {
      char *newbuf;

      /* For data to be searched we need room for the saved bytes,
         plus at least a good-sized read.  */
      idx_t minsize = save + good_readsize;

      /* Add enough room so that the buffer is aligned and has room
         for byte sentinels fore and aft, and so that a uword can
         be read aft.  */
      ptrdiff_t incr_min = minsize - bufalloc + min_after_buflim;

      idx_t old_bufalloc = bufalloc;
      if (incr_min <= 0)
        newbuf = buffer;
      else
        {
          /* Try not to allocate more memory than the file size indicates,
             as that might cause unnecessary memory exhaustion if the file
             is large.  However, do not use the original file size as a
             heuristic if we've already read past the file end, as most
             likely the file is growing.  */
          ptrdiff_t alloc_max = -1;
          if (usable_st_size (st))
            {
              off_t to_be_read = st->st_size - bufoffset;
              ptrdiff_t a;
              if (0 <= to_be_read
                  && !ckd_add (&a, to_be_read, save + min_after_buflim))
                alloc_max = MAX (a, bufalloc + incr_min);
            }

          newbuf = xpalloc (nullptr, &bufalloc, incr_min, alloc_max, 1);
        }

      readbuf = ALIGN_TO (newbuf + 1 + save, pagesize);
      idx_t moved = save + 1;  /* Move the preceding byte sentinel too.  */
      memmove (readbuf - moved, buflim - moved, moved);
      if (0 < incr_min)
        {
          if (buffer_is_mmap)
            munmap (buffer, old_bufalloc);
          else
            free (buffer);
          buffer_is_mmap = false;
          buffer = newbuf;
        }
    }

  bufbeg = readbuf - save;

  clear_asan_poison ();

  ptrdiff_t fillsize;
  bool cc = true;

  while (true)
    {
      if (eof_known)
        fillsize = 0;
      else
        {
          fillsize = safe_read (bufdesc, readbuf, good_readsize);
          if (fillsize < 0)
            {
              fillsize = 0;
              cc = false;
            }
          /* Pseudo-files may report sizes of 0 or 4096, and files may
             grow, so trust the size only if one read returned exactly
             that many bytes and was not limited by the buffer.  */
          else if (S_ISREG (st->st_mode) && 0 < st->st_size
                   && fillsize == st->st_size && fillsize < good_readsize)
            eof_known = true;
        }
      bufoffset += fillsize;

      if (((fillsize == 0) | !skip_nuls) || !all_zeros (readbuf, fillsize))
        break;
      totalnl = add_count (totalnl, fillsize);

      if (SEEK_DATA != SEEK_SET && !seek_data_failed)
        {
          /* Solaris SEEK_DATA fails with errno == ENXIO in a hole at EOF.  */
          off_t data_start = lseek (bufdesc, bufoffset, SEEK_DATA);
          if (data_start < 0 && errno == ENXIO
              && usable_st_size (st) && bufoffset < st->st_size)
            data_start = lseek (bufdesc, 0, SEEK_END);

          if (data_start < 0)
            seek_data_failed = true;
          else
            {
              totalnl = add_count (totalnl, data_start - bufoffset);
              bufoffset = data_start;
            }
        }
    }

  buflim = readbuf + fillsize;

  /* Initialize the following word, because skip_easy_bytes and some
     matchers read (but do not use) those bytes.  This avoids false
     positive reports of these bytes being used uninitialized.  */
  memset (buflim, 0, uword_size);

  /* Mark the part of the buffer not filled by the read or set by
     the above memset call as ASAN-poisoned.  */
  asan_poison (buflim + uword_size, bufalloc - (buflim - buffer) - uword_size);

  return cc;
}

/* Flags controlling the style of output. */
static enum
{
  BINARY_BINARY_FILES,
  TEXT_BINARY_FILES,
  WITHOUT_MATCH_BINARY_FILES
} binary_files;		/* How to handle binary files.  */

/* Options for output as a list of matching/non-matching files */
static enum
{
  LISTFILES_NONE,
  LISTFILES_MATCHING,
  LISTFILES_NONMATCHING,
} list_files;

/* Whether to output filenames.  1 means yes, 0 means no, and -1 means
   'grep -r PATTERN FILE' was used and it is not known yet whether
   FILE is a directory (which means yes) or not (which means no).  */
static int out_file;

static int filename_mask;	/* If zero, output nulls after filenames.  */
static _Thread_local bool out_quiet;	/* Suppress all normal output. */
static bool initial_out_quiet;
static bool out_invert;		/* Print nonmatching stuff. */
static bool out_line;		/* Print line numbers. */
static bool out_byte;		/* Print byte offsets. */
static intmax_t out_before;	/* Lines of leading context. */
static intmax_t out_after;	/* Lines of trailing context. */
static bool count_matches;	/* Count matching lines.  */
static intmax_t max_count;	/* Max number of selected
                                   lines from an input file.  */
static bool line_buffered;	/* Use line buffering.  */
static char *label;		/* Fake filename for stdin */


/* Internal variables to keep track of byte count, context, etc. */
static _Thread_local intmax_t totalcc;	/* Total character count before bufbeg. */
static _Thread_local char const *lastnl;	/* Pointer after last newline counted. */
static _Thread_local char *lastout;		/* Pointer after last character output;
                                   null if no character has been output
                                   or if it's conceptually before bufbeg. */
static _Thread_local intmax_t outleft;	/* Maximum number of selected lines.  */
static _Thread_local intmax_t pending;	/* Pending lines of output.
                                   Always kept 0 if out_quiet is true.  */
static _Thread_local bool done_on_match;	/* Stop scanning file on first match.  */
static bool initial_done_on_match;
static bool exit_on_match;	/* Exit on first match.  */
static bool dev_null_output;	/* Stdout is known to be /dev/null.  */
static bool binary;		/* Use binary rather than text I/O.  */

static bool global_used;
static _Thread_local bool file_used;
static _Thread_local bool task_had_prtext;

static idx_t _GL_ATTRIBUTE_PURE
count_newlines (char const *beg, char const *lim)
{
  idx_t newlines = 0;

#if defined __AVX2__
  if (lim - beg >= 128)
    {
      __m256i match_eol = _mm256_set1_epi8 (eolbyte);
      while (lim - beg >= 128)
        {
          __m256i v0 = _mm256_loadu_si256 ((__m256i const *) (void const *) (beg + 0));
          __m256i v1 = _mm256_loadu_si256 ((__m256i const *) (void const *) (beg + 32));
          __m256i v2 = _mm256_loadu_si256 ((__m256i const *) (void const *) (beg + 64));
          __m256i v3 = _mm256_loadu_si256 ((__m256i const *) (void const *) (beg + 96));
          unsigned int m0 = _mm256_movemask_epi8 (_mm256_cmpeq_epi8 (v0, match_eol));
          unsigned int m1 = _mm256_movemask_epi8 (_mm256_cmpeq_epi8 (v1, match_eol));
          unsigned int m2 = _mm256_movemask_epi8 (_mm256_cmpeq_epi8 (v2, match_eol));
          unsigned int m3 = _mm256_movemask_epi8 (_mm256_cmpeq_epi8 (v3, match_eol));
          newlines += __builtin_popcount (m0) + __builtin_popcount (m1)
                    + __builtin_popcount (m2) + __builtin_popcount (m3);
          beg += 128;
        }
    }
  while (lim - beg >= 32)
    {
      __m256i v = _mm256_loadu_si256 ((__m256i const *) (void const *) beg);
      __m256i match_eol = _mm256_set1_epi8 (eolbyte);
      unsigned int m = _mm256_movemask_epi8 (_mm256_cmpeq_epi8 (v, match_eol));
      newlines += __builtin_popcount (m);
      beg += 32;
    }
#endif

  while (beg < lim)
    {
      beg = memchr (beg, eolbyte, lim - beg);
      if (!beg)
        break;
      newlines++;
      beg++;
    }

  return newlines;
}

static void
nlscan (char const *lim)
{
  idx_t newlines = count_newlines (lastnl, lim);
  totalnl = add_count (totalnl, newlines);
  lastnl = lim;
}

/* Print the current filename.  */
static void
print_filename (void)
{
  pr_sgr_start_if (filename_color);
  fputs_errno (input_filename ());
  pr_sgr_end_if (filename_color);
}

/* Print a character separator.  */
static void
print_sep (char sep)
{
  pr_sgr_start_if (sep_color);
  putchar_errno (sep);
  pr_sgr_end_if (sep_color);
}

/* Print a line number or a byte offset.  */
static void
print_offset (intmax_t pos, const char *color)
{
  pr_sgr_start_if (color);
  printf_errno ("%*"PRIdMAX, offset_width, pos);
  pr_sgr_end_if (color);
}

/* Print a whole line head (filename, line, byte).  The output data
   starts at BEG and contains LEN bytes; it is followed by at least
   uword_size bytes, the first of which may be temporarily modified.
   The output data comes from what is perhaps a larger input line that
   goes until LIM, where LIM[-1] is an end-of-line byte.  Use SEP as
   the separator on output.

   Return true unless the line was suppressed due to an encoding error.  */

static bool
print_line_head (char *beg, idx_t len, char const *lim, char sep)
{
  if (binary_files != TEXT_BINARY_FILES && unibyte_mask)
    {
      bool encoding_errors = buf_has_encoding_errors (beg, len);
      if (encoding_errors)
        {
          encoding_error_output = true;
          return false;
        }
    }

  if (out_file)
    {
      print_filename ();
      if (filename_mask)
        print_sep (sep);
      else
        putchar_errno (0);
    }

  if (out_line)
    {
      if (lastnl < lim)
        {
          nlscan (beg);
          totalnl = add_count (totalnl, 1);
          lastnl = lim;
        }
      print_offset (totalnl, line_num_color);
      print_sep (sep);
    }

  if (out_byte)
    {
      intmax_t pos = add_count (totalcc, beg - bufbeg);
      print_offset (pos, byte_num_color);
      print_sep (sep);
    }

  if (align_tabs && (out_file | out_line | out_byte) && len != 0)
    putchar_errno ('\t');

  return true;
}

static char *
print_line_middle (char *beg, char *lim,
                   const char *line_color, const char *match_color)
{
  idx_t match_size;
  ptrdiff_t match_offset;
  char *cur;
  char *mid = nullptr;
  char *b;

  for (cur = beg;
       (cur < lim
        && 0 <= (match_offset = execute (compiled_pattern, beg, lim - beg,
                                         &match_size, cur)));
       cur = b + match_size)
    {
      b = beg + match_offset;

      /* Avoid matching the empty line at the end of the buffer. */
      if (b == lim)
        break;

      /* Avoid hanging on grep --color "" foo */
      if (match_size == 0)
        {
          /* Make minimal progress; there may be further non-empty matches.  */
          /* XXX - Could really advance by one whole multi-octet character.  */
          match_size = 1;
          if (!mid)
            mid = cur;
        }
      else
        {
          /* This function is called on a matching line only,
             but is it selected or rejected/context?  */
          if (only_matching)
            {
              char sep = out_invert ? SEP_CHAR_REJECTED : SEP_CHAR_SELECTED;
              if (! print_line_head (b, match_size, lim, sep))
                return nullptr;
            }
          else
            {
              pr_sgr_start (line_color);
              if (mid)
                {
                  cur = mid;
                  mid = nullptr;
                }
              fwrite_errno (cur, 1, b - cur);
            }

          pr_sgr_start_if (match_color);
          fwrite_errno (b, 1, match_size);
          pr_sgr_end_if (match_color);
          if (only_matching)
            putchar_errno (eolbyte);
        }
    }

  if (only_matching)
    cur = lim;
  else if (mid)
    cur = mid;

  return cur;
}

static char *
print_line_tail (char *beg, const char *lim, const char *line_color)
{
  idx_t eol_size;
  idx_t tail_size;

  eol_size   = (lim > beg && lim[-1] == eolbyte);
  eol_size  += (lim - eol_size > beg && lim[-(1 + eol_size)] == '\r');
  tail_size  =  lim - eol_size - beg;

  if (tail_size > 0)
    {
      pr_sgr_start (line_color);
      fwrite_errno (beg, 1, tail_size);
      beg += tail_size;
      pr_sgr_end (line_color);
    }

  return beg;
}

static void
prline (char *beg, char *lim, char sep)
{
  bool matching;
  const char *line_color;
  const char *match_color;

  if (!only_matching)
    if (! print_line_head (beg, lim - beg - 1, lim, sep))
      return;

  matching = (sep == SEP_CHAR_SELECTED) ^ out_invert;

  if (color_option)
    {
      line_color = (((sep == SEP_CHAR_SELECTED)
                     ^ (out_invert && (color_option < 0)))
                    ? selected_line_color  : context_line_color);
      match_color = (sep == SEP_CHAR_SELECTED
                     ? selected_match_color : context_match_color);
    }
  else
    line_color = match_color = nullptr; /* Shouldn't be used.  */

  if ((only_matching && matching)
      || (color_option && (*line_color || *match_color)))
    {
      /* We already know that non-matching lines have no match (to colorize). */
      if (matching && (only_matching || *match_color))
        {
          beg = print_line_middle (beg, lim, line_color, match_color);
          if (! beg)
            return;
        }

      if (!only_matching && *line_color)
        {
          /* This code is exercised at least when grep is invoked like this:
             echo k| GREP_COLORS='sl=01;32' src/grep k --color=always  */
          beg = print_line_tail (beg, lim, line_color);
        }
    }

  if (!only_matching && lim > beg)
    fwrite_errno (beg, 1, lim - beg);

  if (line_buffered)
    fflush_errno ();

  if (stdout_errno)
    die (EXIT_TROUBLE, stdout_errno, _("write error"));

  lastout = lim;
}

/* Print pending lines of trailing context prior to LIM.  */
static void
prpending (char const *lim)
{
  if (!lastout)
    lastout = bufbeg;
  for (; 0 < pending && lastout < lim; pending--)
    {
      char *nl = rawmemchr (lastout, eolbyte);
      prline (lastout, nl + 1, SEP_CHAR_REJECTED);
    }
}

/* Output the lines between BEG and LIM.  Deal with context.  */
static void
prtext (char *beg, char *lim)
{
  char eol = eolbyte;

  if (!out_quiet && pending > 0)
    prpending (beg);

  char *p = beg;

  if (!out_quiet)
    {
      /* Deal with leading context.  */
      char const *bp = lastout ? lastout : bufbeg;
      intmax_t i;
      for (i = 0; i < out_before; ++i)
        if (p > bp)
          do
            --p;
          while (p[-1] != eol);

      /* Print the group separator unless the output is adjacent to
         the previous output in the file.  */
      bool is_used = buffering_enabled ? file_used : (file_used || global_used);
      if ((0 <= out_before || 0 <= out_after) && is_used
          && p != lastout && group_separator)
        {
          pr_sgr_start_if (sep_color);
          fputs_errno (group_separator);
          pr_sgr_end_if (sep_color);
          putchar_errno ('\n');
        }
      else if (buffering_enabled && !task_had_prtext)
        task_sep = true;

      while (p < beg)
        {
          char *nl = rawmemchr (p, eol);
          nl++;
          prline (p, nl, SEP_CHAR_REJECTED);
          p = nl;
        }
    }

  intmax_t n;
  if (out_invert)
    {
      /* One or more lines are output.  */
      for (n = 0; p < lim && n < outleft; n++)
        {
          char *nl = rawmemchr (p, eol);
          nl++;
          if (!out_quiet)
            prline (p, nl, SEP_CHAR_SELECTED);
          p = nl;
        }
    }
  else
    {
      /* Just one line is output.  */
      if (!out_quiet)
        prline (beg, lim, SEP_CHAR_SELECTED);
      n = 1;
      p = lim;
    }

  after_last_match = bufoffset - (buflim - p);
  pending = out_quiet ? 0 : MAX (0, out_after);
  file_used = true;
  if (!buffering_enabled)
    global_used = true;
  task_had_prtext = true;
  outleft -= n;
}

/* Replace all NUL bytes in buffer P (which ends at LIM) with EOL.
   This avoids running out of memory when binary input contains a long
   sequence of zeros, which would otherwise be considered to be part
   of a long line.  *LIM should be EOL.  */
static void
zap_nuls (char *p, char *lim, char eol)
{
  if (eol)
    while (true)
      {
        *lim = '\0';
        p += strlen (p);
        *lim = eol;
        if (p == lim)
          break;
        do
          *p++ = eol;
        while (!*p);
      }
}

/* Scan the specified portion of the buffer, matching lines (or
   between matching lines if OUT_INVERT is true).  Return a count of
   lines printed.  Replace all NUL bytes with NUL_ZAPPER as we go.  */
static intmax_t
grepbuf (char *beg, char const *lim)
{
  intmax_t outleft0 = outleft;
  char *endp;

  for (char *p = beg; p < lim; p = endp)
    {
      idx_t match_size;
      ptrdiff_t match_offset = execute (compiled_pattern, p, lim - p,
                                        &match_size, nullptr);
      if (match_offset < 0)
        {
          if (!out_invert)
            break;
          match_offset = lim - p;
          match_size = 0;
        }
      char *b = p + match_offset;
      endp = b + match_size;
      /* Avoid matching the empty line at the end of the buffer. */
      if (!out_invert && b == lim)
        break;
      if (!out_invert || p < b)
        {
          if (list_files != LISTFILES_NONE)
            return 1;
          char *prbeg = out_invert ? p : b;
          char *prend = out_invert ? b : endp;
          prtext (prbeg, prend);
          if (!outleft || done_on_match)
            {
              if (exit_on_match)
                {
                  if (!buffering_enabled)
                    {
                      stdout_errno = -1;
                      exit (EXIT_SUCCESS);
                    }
                  break;
                }
              break;
            }
        }
    }

  return outleft0 - outleft;
}

static void walk_maybe_recruit (void);

/* Search a given (non-directory) file.  Return a count of lines printed.
   Set *INEOF to true if end-of-file reached.  */
static intmax_t
grep (int fd, struct stat const *st, bool *ineof)
{
  intmax_t nlines, i;
  idx_t residue, save;
  char eol = eolbyte;
  char nul_zapper = '\0';
  bool done_on_match_0 = done_on_match;
  bool out_quiet_0 = out_quiet;

  /* The value of NLINES when nulls were first deduced in the input;
     this is not necessarily the same as the number of matching lines
     before the first null.  -1 if no input nulls have been deduced.  */
  intmax_t nlines_first_null = -1;

  if (! reset (fd, st))
    return 0;

  totalcc = 0;
  lastout = nullptr;
  totalnl = 0;
  outleft = max_count;
  after_last_match = 0;
  pending = 0;
  skip_nuls = skip_empty_lines && !eol;
  encoding_error_output = false;

  nlines = 0;
  residue = 0;
  save = 0;

  if (! fillbuf (save, st))
    {
      suppressible_error (errno);
      return 0;
    }

  offset_width = 0;
  if (align_tabs)
    {
      /* Width is log of maximum number.  Line numbers are origin-1.  */
      intmax_t num = usable_st_size (st) ? st->st_size : INTMAX_MAX;
      num += out_line && num < INTMAX_MAX;
      do
        offset_width++;
      while ((num /= 10) != 0);
    }

  for (bool firsttime = true; ; firsttime = false)
    {
      if (nlines_first_null < 0 && eol && binary_files != TEXT_BINARY_FILES
          && (buf_has_nulls (bufbeg, buflim - bufbeg)
              || (firsttime && file_must_have_nulls (buflim - bufbeg, fd, st))))
        {
          if (binary_files == WITHOUT_MATCH_BINARY_FILES)
            return 0;
          if (!count_matches)
            {
              out_quiet = true;
              if (max_count == INTMAX_MAX)
                done_on_match = true;
            }
          nlines_first_null = nlines;
          nul_zapper = eol;
          skip_nuls = skip_empty_lines;
        }

      lastnl = bufbeg;
      if (lastout)
        lastout = bufbeg;

      char *beg = bufbeg + save;

      /* no more data to scan (eof) except for maybe a residue -> break */
      if (beg == buflim)
        {
          *ineof = true;
          break;
        }

      zap_nuls (beg, buflim, nul_zapper);

      /* Determine new residue (the length of an incomplete line at the end of
         the buffer, 0 means there is no incomplete last line).  */
      char *last_eol = memrchr (beg, eol, buflim - beg);
      beg -= residue;
      char *lim = last_eol ? last_eol + 1 : beg;
      residue = buflim - lim;

      if (beg < lim)
        {
          if (outleft)
            nlines += grepbuf (beg, lim);
          if (pending)
            prpending (lim);
          if ((!outleft && !pending)
              || (done_on_match && MAX (0, nlines_first_null) < nlines))
            goto finish_grep;
        }

      /* The last OUT_BEFORE lines at the end of the buffer will be needed as
         leading context if there is a matching line at the begin of the
         next data. Make beg point to their begin.  */
      i = 0;
      beg = lim;
      while (i < out_before && beg > bufbeg && beg != lastout)
        {
          ++i;
          do
            --beg;
          while (beg[-1] != eol);
        }

      /* Detect whether leading context is adjacent to previous output.  */
      if (beg != lastout)
        lastout = nullptr;

      /* Handle some details and read more data to scan.  */
      save = residue + lim - beg;
      if (out_byte)
        totalcc = add_count (totalcc, buflim - bufbeg - save);
      if (out_line)
        nlscan (beg);
      if (! fillbuf (save, st))
        {
          suppressible_error (errno);
          goto finish_grep;
        }

      /* A traversal that has not recruited the worker threads yet should
         not wait for the end of a long file to do so.  */
      walk_maybe_recruit ();
    }
  if (residue)
    {
      *buflim++ = eol;
      if (outleft)
        nlines += grepbuf (bufbeg + save - residue, buflim);
      if (pending)
        prpending (buflim);
    }

 finish_grep:
  done_on_match = done_on_match_0;
  out_quiet = out_quiet_0;
  if (binary_files == BINARY_BINARY_FILES && !out_quiet
      && (encoding_error_output
          || (0 <= nlines_first_null && nlines_first_null < nlines)))
    task_error (0, _("%s: binary file matches"), input_filename ());
  return nlines;
}

static bool
grepdirent (FTS *fts, FTSENT *ent, bool command_line)
{
  bool follow;
  command_line &= ent->fts_level == FTS_ROOTLEVEL;

  if (ent->fts_info == FTS_DP)
    return true;

  if (!command_line
      && skipped_file (ent->fts_name, false,
                       (ent->fts_info == FTS_D || ent->fts_info == FTS_DC
                        || ent->fts_info == FTS_DNR)))
    {
      fts_set (fts, ent, FTS_SKIP);
      return true;
    }

  filename = ent->fts_path;
  if (omit_dot_slash && filename[1])
    filename += 2;
  follow = (fts->fts_options & FTS_LOGICAL
            || (fts->fts_options & FTS_COMFOLLOW && command_line));

  switch (ent->fts_info)
    {
    case FTS_D:
      if (directories == RECURSE_DIRECTORIES)
        return true;
      fts_set (fts, ent, FTS_SKIP);
      break;

    case FTS_DC:
      if (!suppress_errors)
        task_error (0, _("%s: warning: recursive directory loop"), filename);
      return true;

    case FTS_DNR:
    case FTS_ERR:
    case FTS_NS:
      suppressible_error (ent->fts_errno);
      return true;

    case FTS_DEFAULT:
    case FTS_NSOK:
      if (skip_devices (command_line))
        {
          struct stat *st = ent->fts_statp;
          struct stat st1;
          if (! st->st_mode)
            {
              /* The file type is not already known.  Get the file status
                 before opening, since opening might have side effects
                 on a device.  */
              int flag = follow ? 0 : AT_SYMLINK_NOFOLLOW;
              if (fstatat (fts->fts_cwd_fd, ent->fts_accpath, &st1, flag) != 0)
                {
                  suppressible_error (errno);
                  return true;
                }
              st = &st1;
            }
          if (is_device_mode (st->st_mode))
            return true;
        }
      break;

    case FTS_F:
    case FTS_SLNONE:
      break;

    case FTS_SL:
    case FTS_W:
      return true;

    default:
      abort ();
    }

  return grepfile (fts->fts_cwd_fd, ent->fts_accpath, follow, command_line);
}

/* True if errno is ERR after 'open ("symlink", ... O_NOFOLLOW ...)'.
   POSIX specifies ELOOP, but it's EMLINK on FreeBSD and EFTYPE on NetBSD.  */
static bool
open_symlink_nofollow_error (int err)
{
  if (err == ELOOP || err == EMLINK)
    return true;
#ifdef EFTYPE
  if (err == EFTYPE)
    return true;
#endif
  return false;
}

static bool
grepfile (int dirdesc, char const *name, bool follow, bool command_line)
{
  int oflag = (O_RDONLY | O_NOCTTY
               | (IGNORE_DUPLICATE_BRANCH_WARNING
                  (binary ? O_BINARY : 0))
               | (follow ? 0 : O_NOFOLLOW)
               | (skip_devices (command_line) ? O_NONBLOCK : 0));
  int desc = openat_safer (dirdesc, name, oflag);
  if (desc < 0)
    {
      if (follow || ! open_symlink_nofollow_error (errno))
        suppressible_error (errno);
      return true;
    }
  return grepdesc (desc, command_line);
}

/* Read all data from FD, with status ST.  Return true if successful,
   false (setting errno) otherwise.  */
static bool
drain_input (int fd, struct stat const *st)
{
  ssize_t nbytes;
  if (S_ISFIFO (st->st_mode) && dev_null_output)
    {
#ifdef SPLICE_F_MOVE
      /* Should be faster, since it need not copy data to user space.  */
      nbytes = splice (fd, nullptr, STDOUT_FILENO, nullptr,
                       good_readsize, SPLICE_F_MOVE);
      if (0 <= nbytes || errno != EINVAL)
        {
          while (0 < nbytes)
            nbytes = splice (fd, nullptr, STDOUT_FILENO, nullptr,
                             good_readsize, SPLICE_F_MOVE);
          return nbytes == 0;
        }
#endif
    }
  while ((nbytes = safe_read (fd, buffer, bufalloc)))
    if (nbytes < 0)
      return false;
  return true;
}

/* Finish reading from FD, with status ST and where end-of-file has
   been seen if INEOF.  Typically this is a no-op, but when reading
   from standard input this may adjust the file offset or drain a
   pipe.  */

static void
finalize_input (int fd, struct stat const *st, bool ineof)
{
  if (fd == STDIN_FILENO
      && (outleft
          ? (!ineof
             && (seek_failed
                 || (lseek (fd, 0, SEEK_END) < 0
                     /* Linux proc file system has EINVAL (Bug#25180).  */
                     && errno != EINVAL))
             && ! drain_input (fd, st))
          : (bufoffset != after_last_match && !seek_failed
             && lseek (fd, after_last_match, SEEK_SET) < 0)))
    suppressible_error (errno);
}

static bool
grepdesc (int desc, bool command_line)
{
  intmax_t count;
  bool status = true;
  bool ineof = false;
  struct stat st;

  /* Get the file status, possibly for the second time.  This catches
     a race condition if the directory entry changes after the
     directory entry is read and before the file is opened.  For
     example, normally DESC is a directory only at the top level, but
     there is an exception if some other process substitutes a
     directory for a non-directory while 'grep' is running.  */
  if (fstat (desc, &st) != 0)
    {
      suppressible_error (errno);
      goto closeout;
    }

  if (desc != STDIN_FILENO && skip_devices (command_line)
      && is_device_mode (st.st_mode))
    goto closeout;

  if (desc != STDIN_FILENO && command_line
      && skipped_file (filename, true, S_ISDIR (st.st_mode) != 0))
    goto closeout;

  /* Don't output file names if invoked as 'grep -r PATTERN NONDIRECTORY'.  */
  if (out_file < 0)
    out_file = !!S_ISDIR (st.st_mode);

  if (desc != STDIN_FILENO
      && directories == RECURSE_DIRECTORIES && S_ISDIR (st.st_mode))
    {
      /* Traverse the directory starting with its full name, because
         unfortunately fts provides no way to traverse the directory
         starting from its file descriptor.  */

      FTS *fts;
      FTSENT *ent;
      int opts = fts_options & ~(command_line ? 0 : FTS_COMFOLLOW);
      char *fts_arg[2];

      /* Close DESC now, to conserve file descriptors if the race
         condition occurs many times in a deep recursion.  */
      if (close (desc) != 0)
        suppressible_error (errno);

      fts_arg[0] = (char *) filename;
      fts_arg[1] = nullptr;
      fts = fts_open (fts_arg, opts, nullptr);

      if (!fts)
        xalloc_die ();
      while ((ent = fts_read (fts)))
        status &= grepdirent (fts, ent, command_line);
      if (errno)
        suppressible_error (errno);
      if (fts_close (fts) != 0)
        suppressible_error (errno);
      return status;
    }
  if (desc != STDIN_FILENO
      && ((directories == SKIP_DIRECTORIES && S_ISDIR (st.st_mode))
          || ((devices == SKIP_DEVICES
               || (devices == READ_COMMAND_LINE_DEVICES && !command_line))
              && is_device_mode (st.st_mode))))
    goto closeout;

  /* If there is a regular file on stdout and the current file refers
     to the same i-node, we have to report the problem and skip it.
     Otherwise when matching lines from some other input reach the
     disk before we open this file, we can end up reading and matching
     those lines and appending them to the file from which we're reading.
     Then we'd have what appears to be an infinite loop that'd terminate
     only upon filling the output file system or reaching a quota.
     However, there is no risk of an infinite loop if grep is generating
     no output, i.e., with --silent, --quiet, -q.
     Similarly, with any of these:
       --max-count=N (-m) (for N >= 2)
       --files-with-matches (-l)
       --files-without-match (-L)
     there is no risk of trouble.
     For --max-count=1, grep stops after printing the first match,
     so there is no risk of malfunction.  But even --max-count=2, with
     input==output, while there is no risk of infloop, there is a race
     condition that could result in "alternate" output.  */
  if (!out_quiet && list_files == LISTFILES_NONE && 1 < max_count
      && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))
    {
      if (! suppress_errors)
        task_error (0, _("%s: input file is also the output"), input_filename ());
      if (buffering_enabled)
        tls_errseen = true;
      else
        errseen = true;
      goto closeout;
    }

  /* In exact mode, let the committer read the output file; see
     TLS_MAY_DEFER.  */
  if (tls_may_defer && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))
    {
      tls_deferred = true;
      goto closeout;
    }

  count = grep (desc, &st, &ineof);
  if (count_matches)
    {
      if (out_file)
        {
          print_filename ();
          if (filename_mask)
            print_sep (SEP_CHAR_SELECTED);
          else
            putchar_errno (0);
        }
      printf_errno ("%" PRIdMAX "\n", count);
      if (line_buffered)
        fflush_errno ();
    }

  status = !count;

  if (list_files == LISTFILES_NONE)
    finalize_input (desc, &st, ineof);
  else if (list_files == (status ? LISTFILES_NONMATCHING : LISTFILES_MATCHING))
    {
      print_filename ();
      putchar_errno ('\n' & filename_mask);
      if (line_buffered)
        fflush_errno ();
    }

 closeout:
  if (desc != STDIN_FILENO && close (desc) != 0)
    suppressible_error (errno);
  return status;
}

static bool
grep_command_line_arg (char const *arg)
{
  if (STREQ (arg, "-"))
    {
      filename = label;
      if (binary)
        xset_binary_mode (STDIN_FILENO, O_BINARY);
      return grepdesc (STDIN_FILENO, true);
    }
  else
    {
      filename = arg;
      return grepfile (AT_FDCWD, arg, true, true);
    }
}

_Noreturn void usage (int);
void
usage (int status)
{
  if (status != 0)
    {
      fprintf (stderr, _("Usage: %s [OPTION]... PATTERNS [FILE]...\n"),
               getprogname ());
      fprintf (stderr, _("Try '%s --help' for more information.\n"),
               getprogname ());
    }
  else
    {
      printf (_("Usage: %s [OPTION]... PATTERNS [FILE]...\n"), getprogname ());
      printf (_("Search for PATTERNS in each FILE.\n"));
      printf (_("\
Example: %s -i 'hello world' menu.h main.c\n\
PATTERNS can contain multiple patterns separated by newlines.\n\
\n\
Pattern selection and interpretation:\n"), getprogname ());
      printf (_("\
  -E, --extended-regexp     PATTERNS are extended regular expressions\n\
  -F, --fixed-strings       PATTERNS are strings\n\
  -G, --basic-regexp        PATTERNS are basic regular expressions\n\
  -P, --perl-regexp         PATTERNS are Perl regular expressions\n"));
  /* -X is deliberately undocumented.  */
      printf (_("\
  -e, --regexp=PATTERNS     use PATTERNS for matching\n\
  -f, --file=FILE           take PATTERNS from FILE\n\
  -i, --ignore-case         ignore case distinctions in patterns and data\n\
      --no-ignore-case      do not ignore case distinctions (default)\n\
  -w, --word-regexp         match only whole words\n\
  -x, --line-regexp         match only whole lines\n\
  -z, --null-data           a data line ends in 0 byte, not newline\n"));
      printf (_("\
\n\
Miscellaneous:\n\
  -s, --no-messages         suppress error messages\n\
  -v, --invert-match        select non-matching lines\n\
  -V, --version             display version information and exit\n\
      --help                display this help text and exit\n"));
      printf (_("\
\n\
Output control:\n\
  -m, --max-count=NUM       stop after NUM selected lines\n\
  -b, --byte-offset         print the byte offset with output lines\n\
  -n, --line-number         print line number with output lines\n\
      --line-buffered       flush output on every line\n\
  -H, --with-filename       print file name with output lines\n\
  -h, --no-filename         suppress the file name prefix on output\n\
      --label=LABEL         use LABEL as the standard input file name prefix\n\
"));
      printf (_("\
  -o, --only-matching       show only nonempty parts of lines that match\n\
  -q, --quiet, --silent     suppress all normal output\n\
      --binary-files=TYPE   assume that binary files are TYPE;\n\
                            TYPE is 'binary', 'text', or 'without-match'\n\
  -a, --text                equivalent to --binary-files=text\n\
"));
      printf (_("\
  -I                        equivalent to --binary-files=without-match\n\
  -d, --directories=ACTION  how to handle directories;\n\
                            ACTION is 'read', 'recurse', or 'skip'\n\
  -D, --devices=ACTION      how to handle devices, FIFOs and sockets;\n\
                            ACTION is 'read' or 'skip'\n\
  -r, --recursive           like --directories=recurse\n\
  -R, --dereference-recursive  likewise, but follow all symlinks\n\
"));
      printf (_("\
      --include=GLOB        search only files that match GLOB (a file pattern)"
                "\n\
      --exclude=GLOB        skip files that match GLOB\n\
      --exclude-from=FILE   skip files that match any file pattern from FILE\n\
      --exclude-dir=GLOB    skip directories that match GLOB\n\
"));
      printf (_("\
  -L, --files-without-match  print only names of FILEs with no selected lines\n\
  -l, --files-with-matches  print only names of FILEs with selected lines\n\
  -c, --count               print only a count of selected lines per FILE\n\
  -T, --initial-tab         make tabs line up (if needed)\n\
  -Z, --null                print 0 byte after FILE name\n"));
      printf (_("\
\n\
Context control:\n\
  -B, --before-context=NUM  print NUM lines of leading context\n\
  -A, --after-context=NUM   print NUM lines of trailing context\n\
  -C, --context=NUM         print NUM lines of output context\n\
"));
      printf (_("\
  -NUM                      same as --context=NUM\n\
      --group-separator=SEP  print SEP on line between matches with context\n\
      --no-group-separator  do not print separator for matches with context\n\
      --color[=WHEN],\n\
      --colour[=WHEN]       use markers to highlight the matching strings;\n\
                            WHEN is 'always', 'never', or 'auto'\n\
  -U, --binary              do not strip CR characters at EOL (MSDOS/Windows)\n\
\n"));
      printf (_("\
When FILE is '-', read standard input.  If no FILE is given, read standard\n\
input, but with -r, recursively search the working directory instead.  With\n\
fewer than two FILEs, assume -h.  Exit status is 0 if any line is selected,\n\
1 otherwise; if any error occurs and -q is not given, the exit status is 2.\n"
                ));
      emit_bug_reporting_address ();
    }
  exit (status);
}

/* Pattern compilers and matchers.  */

static struct
{
  char name[12];
  int syntax; /* used if compile == GEAcompile */
  compile_fp_t compile;
  execute_fp_t execute;
} const matchers[] = {
  { "grep", RE_SYNTAX_GREP, GEAcompile, EGexecute },
  { "egrep", RE_SYNTAX_EGREP, GEAcompile, EGexecute },
  { "fgrep", 0, Fcompile, Fexecute, },
  { "awk", RE_SYNTAX_AWK, GEAcompile, EGexecute },
  { "gawk", RE_SYNTAX_GNU_AWK, GEAcompile, EGexecute },
  { "posixawk", RE_SYNTAX_POSIX_AWK, GEAcompile, EGexecute },
#if HAVE_LIBPCRE
  { "perl", 0, Pcompile, Pexecute, },
#endif
};
/* Keep these in sync with the 'matchers' table.  */
enum { E_MATCHER_INDEX = 1, F_MATCHER_INDEX = 2, G_MATCHER_INDEX = 0 };

/* Clone a compiled pattern instance for a worker thread.  */
void *
clone_compiled_pattern (void)
{
#if HAVE_LIBPCRE
  if (matchers[raw_matcher_index].compile == Pcompile)
    return Pclone (primary_compiled_pattern);
#endif

  static pthread_mutex_t compile_mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock (&compile_mutex);
  suppress_dfawarn = true;
  char *keys_copy = ximalloc (raw_keycc + 2);
  memcpy (keys_copy, raw_keys, raw_keycc);
  keys_copy[raw_keycc] = '\n';
  keys_copy[raw_keycc + 1] = '\0';
  void *pat = matchers[raw_matcher_index].compile (keys_copy, raw_keycc,
                                                   raw_matcher_syntax,
                                                   raw_exact);
  free (keys_copy);
  suppress_dfawarn = false;
  pthread_mutex_unlock (&compile_mutex);
  return pat;
}

static void
free_compiled_pattern (void *pat)
{
  if (!pat)
    return;
#if HAVE_LIBPCRE
  if (matchers[raw_matcher_index].compile == Pcompile)
    {
      Pfree (pat);
      return;
    }
#endif
  if (matchers[raw_matcher_index].compile == Fcompile)
    Ffree (pat);
  else
    EGfree (pat);
}

/* Return the index of the matcher corresponding to M if available.
   MATCHER is the index of the previous matcher, or -1 if none.
   Exit in case of conflicts or if M is not available.  */
static int
setmatcher (char const *m, int matcher)
{
  for (int i = 0; i < sizeof matchers / sizeof *matchers; i++)
    if (STREQ (m, matchers[i].name))
      {
        if (0 <= matcher && matcher != i)
          die (EXIT_TROUBLE, 0, _("conflicting matchers specified"));
        return i;
      }

#if !HAVE_LIBPCRE
  if (STREQ (m, "perl"))
    die (EXIT_TROUBLE, 0,
         _("Perl matching not supported in a --disable-perl-regexp build"));
#endif
  die (EXIT_TROUBLE, 0, _("invalid matcher %s"), m);
}

/* Get the next non-digit option from ARGC and ARGV.
   Return -1 if there are no more options.
   Process any digit options that were encountered on the way,
   and store the resulting integer into *DEFAULT_CONTEXT.  */
static int
get_nondigit_option (int argc, char *const *argv, intmax_t *default_context)
{
  static int prev_digit_optind = -1;
  int this_digit_optind;
  bool was_digit;
  char buf[INT_BUFSIZE_BOUND (intmax_t) + 4];
  char *p = buf;
  int opt;

  was_digit = false;
  this_digit_optind = optind;
  while (true)
    {
      opt = getopt_long (argc, (char **) argv, short_options,
                         long_options, nullptr);
      if (! c_isdigit (opt))
        break;

      if (prev_digit_optind != this_digit_optind || !was_digit)
        {
          /* Reset to start another context length argument.  */
          p = buf;
        }
      else
        {
          /* Suppress trivial leading zeros, to avoid incorrect
             diagnostic on strings like 00000000000.  */
          p -= buf[0] == '0';
        }

      if (p == buf + sizeof buf - 4)
        {
          /* Too many digits.  Append "..." to make context_length_arg
             complain about "X...", where X contains the digits seen
             so far.  */
          strcpy (p, "...");
          p += 3;
          break;
        }
      *p++ = opt;

      was_digit = true;
      prev_digit_optind = this_digit_optind;
      this_digit_optind = optind;
    }
  if (p != buf)
    {
      *p = '\0';
      context_length_arg (buf, default_context);
    }

  return opt;
}

/* Parse GREP_COLORS.  The default would look like:
     GREP_COLORS='ms=01;31:mc=01;31:sl=:cx=:fn=35:ln=32:bn=32:se=36'
   with boolean capabilities (ne and rv) unset (i.e., omitted).
   No character escaping is needed or supported.  */
static void
parse_grep_colors (void)
{
  const char *p;
  char *q;
  char *name;
  char *val;

  p = getenv ("GREP_COLORS"); /* Plural! */
  if (!p || *p == '\0')
    return;

  /* Work off a writable copy.  */
  q = xstrdup (p);

  name = q;
  val = nullptr;
  /* From now on, be well-formed or you're gone.  */
  for (;;)
    if (*q == ':' || *q == '\0')
      {
        char c = *q;
        struct color_cap const *cap;

        *q++ = '\0'; /* Terminate name or val.  */
        /* Empty name without val (empty cap)
         * won't match and will be ignored.  */
        for (cap = color_dict; cap->name; cap++)
          if (STREQ (cap->name, name))
            break;
        /* If name unknown, go on for forward compatibility.  */
        if (cap->var && val)
          *(cap->var) = val;
        if (cap->fct)
          cap->fct ();
        if (c == '\0')
          return;
        name = q;
        val = nullptr;
      }
    else if (*q == '=')
      {
        if (q == name || val)
          return;
        *q++ = '\0'; /* Terminate name.  */
        val = q; /* Can be the empty string.  */
      }
    else if (!val)
      q++; /* Accumulate name.  */
    else if (*q == ';' || c_isdigit (*q))
      q++; /* Accumulate val.  Protect the terminal from being sent crap.  */
    else
      return;
}

/* Return true if PAT (of length PATLEN) contains an encoding error.  */
static bool
contains_encoding_error (char const *pat, idx_t patlen)
{
  mbstate_t mbs; mbszero (&mbs);
  ptrdiff_t charlen;

  for (idx_t i = 0; i < patlen; i += charlen)
    {
      charlen = mb_clen (pat + i, patlen - i, &mbs);
      if (charlen < 0)
        return true;
    }
  return false;
}

/* When ignoring case and (-E or -F or -G), then for each single-byte
   character I, ok_fold[I] is 1 if every case folded counterpart of I
   is also single-byte, and is -1 otherwise.  */
static signed char ok_fold[NCHAR];
static void
setup_ok_fold (void)
{
  for (int i = 0; i < NCHAR; i++)
    {
      wint_t wi = localeinfo.sbctowc[i];
      if (wi == WEOF)
        continue;

      int ok = 1;
      char32_t folded[CASE_FOLDED_BUFSIZE];
      for (int n = case_folded_counterparts (wi, folded); 0 <= --n; )
        {
          char buf[MB_LEN_MAX];
          mbstate_t s; mbszero (&s);
          if (c32rtomb (buf, folded[n], &s) != 1)
            {
              ok = -1;
              break;
            }
        }
      ok_fold[i] = ok;
    }
}

/* Return the number of bytes in the initial character of PAT, of size
   PATLEN, if Fcompile can handle that character.  Return -1 if
   Fcompile cannot handle it.  MBS is the multibyte conversion state.
   PATLEN must be nonzero.  */

static ptrdiff_t
fgrep_icase_charlen (char const *pat, idx_t patlen, mbstate_t *mbs)
{
  unsigned char pat0 = pat[0];

  /* If PAT starts with a single-byte character, Fcompile works if
     every case folded counterpart is also single-byte.  */
  if (localeinfo.sbctowc[pat0] != WEOF)
    return ok_fold[pat0];

  char32_t wc;
  size_t wn = mbrtoc32 (&wc, pat, patlen, mbs);

  /* If PAT starts with an encoding error, Fcompile does not work.  */
  if (MB_LEN_MAX < wn)
    return -1;

  /* PAT starts with a multibyte character.  Fcompile works if the
     character has no case folded counterparts and toupper translates
     none of its encoding's bytes.  */
  char32_t folded[CASE_FOLDED_BUFSIZE];
  if (case_folded_counterparts (wc, folded))
    return -1;
  for (idx_t i = wn; 0 < --i; )
    {
      unsigned char c = pat[i];
      if (toupper (c) != c)
        return -1;
    }
  return wn;
}

/* Return true if the -F patterns PAT, of size PATLEN, contain only
   single-byte characters that case-fold only to single-byte
   characters, or multibyte characters not subject to case folding,
   and so can be processed by Fcompile.  */

static bool
fgrep_icase_available (char const *pat, idx_t patlen)
{
  mbstate_t mbs; mbszero (&mbs);

  for (idx_t i = 0; i < patlen; )
    {
      int n = fgrep_icase_charlen (pat + i, patlen - i, &mbs);
      if (n < 0)
        return false;
      i += n;
    }

  return true;
}

/* Change the pattern *KEYS_P, of size *LEN_P, from fgrep to grep style.  */

void
fgrep_to_grep_pattern (char **keys_p, idx_t *len_p)
{
  idx_t len = *len_p;
  char *keys = *keys_p;
  mbstate_t mb_state; mbszero (&mb_state);
  char *new_keys = xnmalloc (len + 1, 2);
  char *p = new_keys;

  for (ptrdiff_t n; len; keys += n, len -= n)
    {
      n = mb_clen (keys, len, &mb_state);
      switch (n)
        {
        case -2:
          n = len;
          FALLTHROUGH;
        default:
          p = mempcpy (p, keys, n);
          break;

        case -1:
          memset (&mb_state, 0, sizeof mb_state);
          n = 1;
          FALLTHROUGH;
        case 1:
          switch (*keys)
            {
            case '$': case '*': case '.': case '[': case '\\': case '^':
              *p++ = '\\'; break;
            }
          *p++ = *keys;
          break;
        }
    }

  *p = '\n';
  free (*keys_p);
  *keys_p = new_keys;
  *len_p = p - new_keys;
}

/* If it is easy, convert the MATCHER-style patterns KEYS (of size
   *LEN_P) to -F style, update *LEN_P to a possibly-smaller value, and
   return F_MATCHER_INDEX.  If not, leave KEYS and *LEN_P alone and
   return MATCHER.  This function is conservative and sometimes misses
   conversions, e.g., it does not convert the -E pattern "(a|a|[aa])"
   to the -F pattern "a".  */

static int
try_fgrep_pattern (int matcher, char *keys, idx_t *len_p)
{
  int result = matcher;
  idx_t len = *len_p;
  char *new_keys = ximalloc (len + 1);
  char *p = new_keys;
  char const *q = keys;
  mbstate_t mb_state; mbszero (&mb_state);

  while (len != 0)
    {
      switch (*q)
        {
        case '$': case '*': case '.': case '[': case '^':
          goto fail;

        case '(': case '+': case '?': case '{': case '|':
          /* There is no "case ')'" here, as "grep -E ')'" acts like
             "grep -E '\)'".  */
          if (matcher != G_MATCHER_INDEX)
            goto fail;
          break;

        case '\\':
          if (1 < len)
            switch (q[1])
              {
              case '\n':
              case 'B': case 'S': case 'W': case'\'': case '<':
              case 'b': case 's': case 'w': case '`': case '>':
              case '1': case '2': case '3': case '4':
              case '5': case '6': case '7': case '8': case '9':
                goto fail;

              case '(': case '+': case '?': case '{': case '|':
                /* Pass '\)' to GEAcompile so it can complain.  Otherwise,
                   "grep '\)'" would act like "grep ')'" while "grep '.*\)'
                   would be an error.  */
              case ')':
                if (matcher == G_MATCHER_INDEX)
                  goto fail;
                FALLTHROUGH;
              default:
                q++, len--;
                break;
              }
          break;
        }

      ptrdiff_t clen = (match_icase
                        ? fgrep_icase_charlen (q, len, &mb_state)
                        : mb_clen (q, len, &mb_state));
      if (clen < 0)
        goto fail;
      p = mempcpy (p, q, clen);
      q += clen;
      len -= clen;
    }

  if (*len_p != p - new_keys)
    {
      *len_p = p - new_keys;
      char *keys_end = mempcpy (keys, new_keys, p - new_keys);
      *keys_end = '\n';
    }
  result = F_MATCHER_INDEX;

 fail:
  free (new_keys);
  return result;
}

/* Number of threads to use.  */
static int
get_num_threads (void)
{
  char const *env = getenv ("GREP_THREADS");
  if (env && *env)
    {
      char *end;
      long val = strtol (env, &end, 10);
      if (*end == '\0' && val > 0)
        return (int) MIN (val, 256);
    }

#if defined CPU_COUNT
  cpu_set_t set;
  CPU_ZERO (&set);
  if (sched_getaffinity (0, sizeof set, &set) == 0)
    {
      int count = CPU_COUNT (&set);
      if (count > 0)
        return MIN (count, 16);
    }
#endif

#if defined _SC_NPROCESSORS_ONLN
  long n = sysconf (_SC_NPROCESSORS_ONLN);
  if (n > 0)
    return (int) MIN (n, 16);
#endif

  return 1;
}

enum rob_slot_state
{
  ROB_SLOT_EMPTY = 0,
  ROB_SLOT_IN_PROGRESS,
  ROB_SLOT_DONE
};

struct rob_slot
{
  enum rob_slot_state state;
  bool status;
  bool errseen;
  bool had_prtext;
  bool sep;

  char *stdout_buf;
  size_t stdout_len;
  size_t stdout_cap;

  char *stderr_buf;
  size_t stderr_len;
  size_t stderr_cap;

  size_t *big;
  size_t nbig;

  /* A file left for the committer to search; see TLS_MAY_DEFER.  */
  char *defer_name;
  bool defer_follow;
  bool defer_command_line;

  bool is_chunk;
  bool is_last_chunk;
  intmax_t chunk_count;

  /* True if the chunk's output omitted a line because of an encoding
     error, so that a "binary file matches" message must follow the
     output of the file's last chunk.  */
  bool enc_error;
  void (*cleanup_fn) (void *);
  void *cleanup_arg;
  char *filename;

  bool is_stream_chunk;
  intmax_t chunk_nls;
  char *stream_chunk_alloc;
  char *stream_chunk_data;
  size_t stream_chunk_len;
  char *stream_first_lbeg;
  intmax_t stream_first_rel_nl;
  off_t stream_totalcc;

  char inline_out[1024];
};

enum work_task_type
{
  TASK_TYPE_FILE,
  TASK_TYPE_STDIN,
  TASK_TYPE_CHUNK,
  TASK_TYPE_STREAM_CHUNK,
  TASK_TYPE_MMAP_SCAN,
  TASK_TYPE_TERMINATE,
  TASK_TYPE_WALK
};

enum chunk_mode
{
  CHUNK_MODE_STATELESS,
  CHUNK_MODE_COUNT,
  CHUNK_MODE_LINE_NUMBER
};

struct work_task
{
  enum work_task_type type;
  uint64_t seq;
  char *filename;
  bool follow;
  bool command_line;

  enum chunk_mode mode;
  char const *chunk_data;
  size_t chunk_len;
  intmax_t chunk_totalcc;
  intmax_t chunk_lineno_offset;
  bool chunk_free_data;
  bool is_last_chunk;
  bool is_binary_stream;
  size_t chunk_idx;
  void *shared_state;
};

struct match_record
{
  size_t chunk_idx;
  char const *line_beg;
  char const *line_end;
  intmax_t rel_nl;
  intmax_t lineno;
};

struct chunk_file_state
{
  char *base_addr;
  size_t base_size;
  char *mmap_data;
  size_t mmap_size;
  int file_desc;
  bool close_fd;
  char const *fname;
  bool fname_allocated;
  size_t num_chunks;
  size_t *bounds;

  _Atomic size_t chunks_completed;
  _Atomic intmax_t total_count;
  _Atomic bool has_match;
  _Atomic bool early_stop;

  pthread_mutex_t done_mutex;
  pthread_cond_t done_cv;

  intmax_t *chunk_nls;
  intmax_t *prefix_nls;
  bool *chunk_has_matches;

  pthread_mutex_t match_mutex;
  struct match_record *matches;
  size_t match_count;
  size_t match_cap;
  _Atomic bool match_overflow;
  int offset_width;
};

enum { WORK_QUEUE_CAP = 256 };
enum { ROB_CAP = 1024 };

static struct rob_slot rob[ROB_CAP];
static uint64_t next_seq;
static uint64_t commit_seq;

static struct work_task work_queue[WORK_QUEUE_CAP];
static size_t q_head;
static size_t q_tail;
static size_t q_count;

static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_not_empty_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_not_full_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t rob_space_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t rob_done_cv = PTHREAD_COND_INITIALIZER;

static size_t rob_space_waiters = 0;
static size_t rob_done_waiters = 0;

static bool pool_terminate;
static void rob_try_commit_locked (void);

enum { SLICE_BUF_SIZE = 4 * 1024 * 1024 };
enum { SLICE_POOL_CAP = 32 };
static char *slice_freelist[SLICE_POOL_CAP];
static int slice_freelist_cnt = 0;
static pthread_cond_t slice_avail_cv = PTHREAD_COND_INITIALIZER;

static _Atomic bool binary_stream_reported;

static char *
alloc_slice_buf_locked (void)
{
  if (slice_freelist_cnt > 0)
    return slice_freelist[--slice_freelist_cnt];

  size_t buf_sz = SLICE_BUF_SIZE + 2 + uword_size;
  void *p = mmap (NULL, buf_sz, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (p == MAP_FAILED)
    {
      p = mmap (NULL, buf_sz, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p == MAP_FAILED)
        xalloc_die ();
    }
  return p;
}

static void
free_slice_buf_locked (char *ptr)
{
  if (slice_freelist_cnt < SLICE_POOL_CAP)
    {
      slice_freelist[slice_freelist_cnt++] = ptr;
      pthread_cond_signal (&slice_avail_cv);
    }
  else
    munmap (ptr, SLICE_BUF_SIZE + 2 + uword_size);
}

static void
cleanup_slice_freelist (void)
{
  for (int i = 0; i < slice_freelist_cnt; ++i)
    munmap (slice_freelist[i], SLICE_BUF_SIZE + 2 + uword_size);
  slice_freelist_cnt = 0;
}

static int pool_nthreads;
static pthread_t *pool_threads;

/* The number of worker threads to use, and whether they have been
   started.  Workers start only when there is work for them; a
   directory traversal first runs on the main thread alone and recruits
   them only if it lasts long enough, so that small trees and early exits
   do not pay for starting and stopping threads.  */
static int pool_target;
static bool pool_started;

/* The number of entries of POOL_THREADS that have been filled in, and
   whether the thread that creates the workers should stop.  */
static atomic_int pool_created;
static atomic_bool pool_stop_spawning;
static bool overall_status = true;

static intmax_t chunk_accumulated_count = 0;
static bool chunk_has_any_match = false;
static bool chunk_enc_error = false;
static intmax_t stream_commit_totalnl = 0;

/* Output the group separator, as prtext does.  */
static void
print_group_separator (void)
{
  pr_sgr_start_if (sep_color);
  fputs_errno (group_separator);
  pr_sgr_end_if (sep_color);
  putchar_errno ('\n');
}

/* Return true if a task's output needs a leading group separator,
   given what has been output so far.  HAD_PRTEXT tells whether the task
   called prtext at all, and SEP whether its first call left the
   decision to the committer (see TASK_SEP).  Like the static variable
   USED of single-threaded grep, GLOBAL_USED becomes true after any
   prtext call, even one that output nothing.  */
static bool _GL_ATTRIBUTE_PURE
need_group_separator (bool had_prtext, bool sep)
{
  return (had_prtext && sep && (0 <= out_before || 0 <= out_after)
          && global_used && group_separator);
}

/* The size of the pieces in which exact mode replays small writes; see
   BIG_WRITE_MIN.  */
static size_t
exact_piece_size (void)
{
#ifdef __GLIBC__
  size_t bufsize = __fbufsize (stdout);
  if (BIG_WRITE_MIN <= bufsize)
    return bufsize - 1;
#endif
  return BIG_WRITE_MIN - 1;
}

/* Write OUT[FROM..TO) to standard output.  BIG[0..2*NBIG) are the
   large writes recorded while buffering, and *BIGI is the index of the
   first one not yet replayed.  */
static void
commit_stdout (char const *out, size_t from, size_t to,
               size_t const *big, size_t nbig, size_t *bigi)
{
  if (!exact_stdout)
    {
      if (from < to)
        fwrite_errno (out + from, 1, to - from);
      return;
    }
  while (from < to)
    {
      if (*bigi < nbig && big[2 * *bigi] == from)
        {
          size_t len = big[2 * *bigi + 1];
          fwrite_errno (out + from, 1, len);
          from += len;
          ++*bigi;
          continue;
        }
      size_t lim = *bigi < nbig ? MIN (to, big[2 * *bigi]) : to;
      size_t n = MIN (lim - from, exact_piece_size ());
      fwrite_errno (out + from, 1, n);
      from += n;
    }
}

/* Commit output captured by a worker: OUTLEN bytes of standard output
   at OUT, the diagnostic records ERR..ERR+ERRLEN (see struct errrec),
   and the large writes BIG[0..2*NBIG) recorded in exact mode.  Write
   each diagnostic after the output that preceded it, flushing standard
   output first, as error () does.  If SEP, output a group separator
   first.  Buffering must be disabled.  */
static void
commit_output (char const *out, size_t outlen, char const *err,
               size_t errlen, size_t const *big, size_t nbig, bool sep)
{
  size_t pos = 0, bigi = 0;
  char const *lim = err + errlen;
  for (char const *p = err; p < lim; )
    {
      struct errrec h;
      memcpy (&h, p, sizeof h);
      char const *text = p + sizeof h;
      if (sep && h.after_sep)
        {
          print_group_separator ();
          sep = false;
        }
      commit_stdout (out, pos, h.outpos, big, nbig, &bigi);
      pos = h.outpos;
      fflush (stdout);
      fwrite (text, 1, h.len, stderr);
      p = text + h.len;
    }
  if (sep)
    print_group_separator ();
  commit_stdout (out, pos, outlen, big, nbig, &bigi);
}

/* Reset the per-file state of the current thread before searching a
   file into its buffers.  */
static void
task_reset (void)
{
  membuf_reset (&tls_out);
  membuf_reset (&tls_err);
  membuf_reset (&tls_big);
  tls_errseen = false;
  tls_deferred = false;
  out_quiet = initial_out_quiet;
  done_on_match = initial_done_on_match;
  totalcc = 0;
  lastnl = nullptr;
  lastout = nullptr;
  outleft = max_count;
  pending = 0;
  file_used = false;
  task_had_prtext = false;
  task_sep = false;
}

/* Search the file that a worker left for the committer, now that all
   output before it has been committed, and put the results into SLOT.
   See TLS_MAY_DEFER.  */
static void
rob_search_deferred (struct rob_slot *slot)
{
  bool saved_buffering = buffering_enabled;
  char const *saved_filename = filename;
  struct membuf saved_out = tls_out, saved_err = tls_err, saved_big = tls_big;
  tls_out = tls_err = tls_big = (struct membuf) { .data = nullptr };
  buffering_enabled = true;
  task_reset ();
  filename = slot->defer_name;
  slot->status = grepfile (AT_FDCWD, slot->defer_name, slot->defer_follow,
                           slot->defer_command_line);
  slot->errseen = tls_errseen;
  slot->had_prtext = task_had_prtext;
  slot->sep = task_sep;
  slot->stdout_buf = tls_out.data;
  slot->stdout_len = tls_out.len;
  slot->stdout_cap = tls_out.cap;
  slot->stderr_buf = tls_err.data;
  slot->stderr_len = tls_err.len;
  slot->stderr_cap = tls_err.cap;
  slot->big = CAST_ALIGNED (size_t *, tls_big.data);
  slot->nbig = tls_big.len / (2 * sizeof (size_t));
  free (slot->defer_name);
  slot->defer_name = nullptr;
  tls_out = saved_out;
  tls_err = saved_err;
  tls_big = saved_big;
  filename = saved_filename;
  buffering_enabled = saved_buffering;
}

static void
rob_try_commit_locked (void)
{
  bool saved_buffering = buffering_enabled;
  buffering_enabled = false;
  bool committed_any = false;

  while (commit_seq < next_seq)
    {
      struct rob_slot *slot = &rob[commit_seq % ROB_CAP];
      if (slot->state != ROB_SLOT_DONE)
        break;

      if (slot->defer_name)
        rob_search_deferred (slot);

      if (slot->filename)
        filename = slot->filename;

      /* Group separator logic across files */
      bool want_sep = need_group_separator (slot->had_prtext, slot->sep);
      if (slot->had_prtext)
        global_used = true;

      if (slot->is_stream_chunk)
        {
          if (slot->stream_chunk_data != nullptr)
            {
              char const *saved_filename = filename;
              char *saved_bufbeg = bufbeg;
              char *saved_buflim = buflim;
              off_t saved_totalcc = totalcc;
              intmax_t saved_totalnl = totalnl;
              char const *saved_lastnl = lastnl;
              char *saved_lastout = lastout;
              intmax_t saved_outleft = outleft;
              idx_t saved_pending = pending;
              bool saved_buffering_enabled = buffering_enabled;
              bool saved_out_quiet = out_quiet;
              bool saved_done_on_match = done_on_match;
              bool saved_encoding_error_output = encoding_error_output;

              buffering_enabled = false;
              filename = label;
              bufbeg = slot->stream_chunk_data;
              buflim = bufbeg + slot->stream_chunk_len;
              totalcc = slot->stream_totalcc;
              totalnl = stream_commit_totalnl + slot->stream_first_rel_nl;
              lastnl = slot->stream_first_lbeg;
              lastout = nullptr;
              outleft = max_count;
              pending = 0;
              out_quiet = initial_out_quiet;
              done_on_match = initial_done_on_match;
              encoding_error_output = false;

              intmax_t mcnt = grepbuf (slot->stream_first_lbeg, buflim);
              slot->status = (mcnt == 0);
              if (mcnt > 0)
                slot->had_prtext = true;
              slot->enc_error |= (binary_files == BINARY_BINARY_FILES
                                  && !out_quiet && encoding_error_output);

              filename = saved_filename;
              bufbeg = saved_bufbeg;
              buflim = saved_buflim;
              totalcc = saved_totalcc;
              totalnl = saved_totalnl;
              lastnl = saved_lastnl;
              lastout = saved_lastout;
              outleft = saved_outleft;
              pending = saved_pending;
              buffering_enabled = saved_buffering_enabled;
              out_quiet = saved_out_quiet;
              done_on_match = saved_done_on_match;
              encoding_error_output = saved_encoding_error_output;

              free_slice_buf_locked (slot->stream_chunk_alloc);
              slot->stream_chunk_alloc = nullptr;
              slot->stream_chunk_data = nullptr;
            }
          stream_commit_totalnl += slot->chunk_nls;
          slot->is_stream_chunk = false;
          slot->chunk_nls = 0;
        }

      bool binary_message = false;
      if (slot->is_chunk)
        {
          chunk_accumulated_count += slot->chunk_count;
          if (slot->chunk_count > 0 || !slot->status)
            chunk_has_any_match = true;
          chunk_enc_error |= slot->enc_error;

          if (slot->is_last_chunk)
            {
              if (count_matches)
                {
                  if (out_file)
                    {
                      print_filename ();
                      if (filename_mask)
                        print_sep (SEP_CHAR_SELECTED);
                      else
                        putchar_errno (0);
                    }
                  printf_errno ("%" PRIdMAX "\n", chunk_accumulated_count);
                  if (line_buffered)
                    fflush_errno ();
                }
              else if (list_files == LISTFILES_MATCHING)
                {
                  if (chunk_has_any_match)
                    {
                      print_filename ();
                      putchar_errno ('\n' & filename_mask);
                      if (line_buffered)
                        fflush_errno ();
                    }
                }
              else if (list_files == LISTFILES_NONMATCHING)
                {
                  if (!chunk_has_any_match)
                    {
                      print_filename ();
                      putchar_errno ('\n' & filename_mask);
                      if (line_buffered)
                        fflush_errno ();
                    }
                }

              slot->status = !chunk_has_any_match;
              binary_message = chunk_enc_error;
              chunk_accumulated_count = 0;
              chunk_has_any_match = false;
              chunk_enc_error = false;
            }
          else
            {
              if (count_matches || list_files != LISTFILES_NONE)
                slot->status = true;
            }
        }

      commit_output (slot->stdout_buf ? slot->stdout_buf : slot->inline_out,
                     slot->stdout_len, slot->stderr_buf, slot->stderr_len,
                     slot->big, slot->nbig, want_sep);

      /* Like single-threaded grep, report omitted lines once, after all
         the output of the file.  */
      if (binary_message)
        error (0, 0, _("%s: binary file matches"),
               slot->filename ? slot->filename : _("(standard input)"));

      if (line_buffered)
        fflush_errno ();

      if (stdout_errno)
        die (EXIT_TROUBLE, stdout_errno, _("write error"));

      overall_status &= slot->status;
      if (slot->errseen)
        errseen = true;

      if (slot->stdout_buf)
        {
          free (slot->stdout_buf);
          slot->stdout_buf = nullptr;
        }

      if (slot->stderr_buf)
        {
          free (slot->stderr_buf);
          slot->stderr_buf = nullptr;
        }

      free (slot->big);
      slot->big = nullptr;
      slot->nbig = 0;

      if (slot->cleanup_fn)
        {
          slot->cleanup_fn (slot->cleanup_arg);
          slot->cleanup_fn = nullptr;
          slot->cleanup_arg = nullptr;
        }

      if (slot->filename)
        {
          free (slot->filename);
          slot->filename = nullptr;
          filename = nullptr;
        }

      if (slot->stream_chunk_alloc)
        {
          free_slice_buf_locked (slot->stream_chunk_alloc);
          slot->stream_chunk_alloc = nullptr;
          slot->stream_chunk_data = nullptr;
        }

      slot->state = ROB_SLOT_EMPTY;
      commit_seq++;
      committed_any = true;

      if (exit_on_match && !slot->status)
        {
          if (line_buffered)
            fflush_errno ();
          exit (EXIT_SUCCESS);
        }
    }

  if (committed_any)
    {
      if (rob_space_waiters > 0)
        pthread_cond_signal (&rob_space_cv);
      if (rob_done_waiters > 0)
        pthread_cond_broadcast (&rob_done_cv);
    }

  buffering_enabled = saved_buffering;
}

static uint64_t
producer_alloc_slot_locked (void)
{
  while (next_seq - commit_seq >= ROB_CAP && !pool_terminate)
    {
      rob_try_commit_locked ();
      if (next_seq - commit_seq < ROB_CAP)
        break;
      rob_space_waiters++;
      pthread_cond_wait (&rob_space_cv, &pool_mutex);
      rob_space_waiters--;
    }

  uint64_t seq = next_seq++;
  struct rob_slot *slot = &rob[seq % ROB_CAP];
  memset (slot, 0, sizeof *slot);
  slot->state = ROB_SLOT_IN_PROGRESS;
  return seq;
}

static void walk_participate (unsigned int);
static void walk_free_thread_buffers (void);

static void *
worker_thread_func (void *arg)
{
  (void) arg;

  compiled_pattern = clone_compiled_pattern ();
  buffering_enabled = true;
  membuf_init (&tls_out, 64 * 1024);
  membuf_init (&tls_err, 4096);

  pthread_mutex_lock (&pool_mutex);

  while (true)
    {
      while (q_count == 0 && !pool_terminate)
        pthread_cond_wait (&q_not_empty_cv, &pool_mutex);

      if (pool_terminate && q_count == 0)
        {
          pthread_mutex_unlock (&pool_mutex);
          break;
        }

      struct work_task task = work_queue[q_head];
      q_head = (q_head + 1) % WORK_QUEUE_CAP;
      q_count--;
      pthread_cond_signal (&q_not_full_cv);
      pthread_mutex_unlock (&pool_mutex);

      if (task.type == TASK_TYPE_TERMINATE)
        break;

      if (task.type == TASK_TYPE_WALK)
        {
          walk_participate (task.seq);
          pthread_mutex_lock (&pool_mutex);
          continue;
        }

      if (task.type == TASK_TYPE_MMAP_SCAN)
        {
          struct chunk_file_state *state = task.shared_state;
          size_t idx = task.chunk_idx;
          char const *cbeg = task.chunk_data;
          char const *clim = task.chunk_data + task.chunk_len;

          state->chunk_nls[idx] = count_newlines (cbeg, clim);

          idx_t match_size;
          ptrdiff_t off = execute (compiled_pattern, cbeg, clim - cbeg, &match_size, nullptr);
          bool matched = (off >= 0);
          if (!out_invert && off >= 0 && cbeg + off == clim)
            matched = false;
          state->chunk_has_matches[idx] = out_invert ? true : matched;

          if (state->chunk_has_matches[idx] && !out_invert && !count_matches
              && list_files == LISTFILES_NONE)
            {
              char const *nl_cursor = cbeg;
              idx_t running_nl = 0;
              char const *p = cbeg;

              while (p < clim)
                {
                  if (out_before <= 0 && out_after <= 0 && atomic_load (&state->match_overflow))
                    break;

                  ptrdiff_t mo;
                  if (p == cbeg && off >= 0 && !out_invert)
                    mo = off;
                  else
                    mo = execute (compiled_pattern, p, clim - p, &match_size, nullptr);

                  if (mo < 0)
                    {
                      if (!out_invert) break;
                      mo = clim - p;
                      match_size = 0;
                    }
                  char const *b = p + mo;
                  char const *endp = b + match_size;
                  if (!out_invert && b == clim) break;
                  if (!out_invert || p < b)
                    {
                      char const *mbeg = out_invert ? p : b;
                      char const *lbeg = mbeg;
                      while (lbeg > cbeg && lbeg[-1] != eolbyte) lbeg--;
                      char const *lend = memchr (mbeg, eolbyte, clim - mbeg);
                      lend = lend ? lend + 1 : clim;

                      if (out_line)
                        {
                          running_nl += count_newlines (nl_cursor, lbeg);
                          nl_cursor = lbeg;
                        }

                      pthread_mutex_lock (&state->match_mutex);
                      if (out_before > 0 || out_after > 0)
                        {
                          if (state->match_count >= state->match_cap)
                            {
                              state->match_cap *= 2;
                              state->matches = xrealloc (state->matches,
                                                         state->match_cap * sizeof *state->matches);
                            }
                          state->matches[state->match_count].chunk_idx = idx;
                          state->matches[state->match_count].line_beg = lbeg;
                          state->matches[state->match_count].line_end = lend;
                          state->matches[state->match_count].rel_nl = running_nl;
                          state->match_count++;
                        }
                      else
                        {
                          if (state->match_count < state->match_cap)
                            {
                              state->matches[state->match_count].chunk_idx = idx;
                              state->matches[state->match_count].line_beg = lbeg;
                              state->matches[state->match_count].line_end = lend;
                              state->matches[state->match_count].rel_nl = running_nl;
                              state->match_count++;
                            }
                          else
                            atomic_store (&state->match_overflow, true);
                        }
                      pthread_mutex_unlock (&state->match_mutex);
                    }
                  p = endp;
                }
            }

          if (atomic_fetch_add (&state->chunks_completed, 1) == state->num_chunks - 1)
            {
              pthread_mutex_lock (&state->done_mutex);
              pthread_cond_signal (&state->done_cv);
              pthread_mutex_unlock (&state->done_mutex);
            }

          pthread_mutex_lock (&pool_mutex);
          continue;
        }

      task_reset ();

      bool status = true;
      char *defer_name = nullptr;
      intmax_t chunk_matched_cnt = 0;
      bool enc_error = false;

      if (task.type == TASK_TYPE_STDIN)
        {
          filename = label;
          if (binary)
            xset_binary_mode (STDIN_FILENO, O_BINARY);
          status = grepdesc (STDIN_FILENO, true);
        }
      else if (task.type == TASK_TYPE_FILE)
        {
          filename = task.filename;
          tls_may_defer = exact_stdout;
          status = grepfile (AT_FDCWD, task.filename, task.follow, task.command_line);
          tls_may_defer = false;
          if (tls_deferred)
            defer_name = task.filename;
          else
            free (task.filename);
        }
      else if (task.type == TASK_TYPE_STREAM_CHUNK)
        {
          filename = label;
          bufbeg = (char *) task.chunk_data;
          buflim = (char *) task.chunk_data + task.chunk_len;
          totalcc = task.chunk_totalcc;
          totalnl = task.chunk_lineno_offset;
          lastnl = bufbeg;

          offset_width = 0;
          if (align_tabs)
            {
              intmax_t num = INTMAX_MAX;
              do
                offset_width++;
              while ((num /= 10) != 0);
            }

          if (out_line && !out_quiet && !task.is_binary_stream && !count_matches && list_files == LISTFILES_NONE)
            {
              intmax_t chunk_nls = count_newlines (bufbeg, buflim);
              if (!out_invert)
                {
                  idx_t match_size;
                  ptrdiff_t mo = execute (compiled_pattern, bufbeg, buflim - bufbeg, &match_size, nullptr);
                  if (mo < 0 || bufbeg + mo == buflim)
                    {
                      pthread_mutex_lock (&pool_mutex);
                      free_slice_buf_locked ((char *) (task.chunk_data - 1));
                      struct rob_slot *slot = &rob[task.seq % ROB_CAP];
                      slot->is_chunk = true;
                      slot->is_last_chunk = task.is_last_chunk;
                      slot->is_stream_chunk = true;
                      slot->chunk_nls = chunk_nls;
                      slot->stream_chunk_alloc = nullptr;
                      slot->stream_chunk_data = nullptr;
                      slot->status = true;
                      slot->state = ROB_SLOT_DONE;
                      rob_try_commit_locked ();
                      continue;
                    }
                  else
                    {
                      char *lbeg = bufbeg;
                      intmax_t first_rel_nl = 0;
                      if (out_before < 0 && out_after < 0)
                        {
                          lbeg = bufbeg + mo;
                          while (lbeg > bufbeg && lbeg[-1] != eolbyte)
                            lbeg--;
                          first_rel_nl = count_newlines (bufbeg, lbeg);
                        }

                      pthread_mutex_lock (&pool_mutex);
                      struct rob_slot *slot = &rob[task.seq % ROB_CAP];
                      slot->is_chunk = true;
                      slot->is_last_chunk = task.is_last_chunk;
                      slot->is_stream_chunk = true;
                      slot->chunk_nls = chunk_nls;
                      slot->stream_chunk_alloc = (char *) (task.chunk_data - 1);
                      slot->stream_chunk_data = bufbeg;
                      slot->stream_chunk_len = task.chunk_len;
                      slot->stream_first_lbeg = lbeg;
                      slot->stream_first_rel_nl = first_rel_nl;
                      slot->stream_totalcc = task.chunk_totalcc;
                      slot->status = false;
                      slot->state = ROB_SLOT_DONE;
                      rob_try_commit_locked ();
                      continue;
                    }
                }
              else
                {
                  char *lbeg = bufbeg;
                  intmax_t first_rel_nl = 0;

                  pthread_mutex_lock (&pool_mutex);
                  struct rob_slot *slot = &rob[task.seq % ROB_CAP];
                  slot->is_chunk = true;
                  slot->is_last_chunk = task.is_last_chunk;
                  slot->is_stream_chunk = true;
                  slot->chunk_nls = chunk_nls;
                  slot->stream_chunk_alloc = (char *) (task.chunk_data - 1);
                  slot->stream_chunk_data = bufbeg;
                  slot->stream_chunk_len = task.chunk_len;
                  slot->stream_first_lbeg = lbeg;
                  slot->stream_first_rel_nl = first_rel_nl;
                  slot->stream_totalcc = task.chunk_totalcc;
                  slot->status = false;
                  slot->state = ROB_SLOT_DONE;
                  rob_try_commit_locked ();
                  continue;
                }
            }
          else if (task.is_binary_stream)
            {
              out_quiet = true;
              done_on_match = true;
              intmax_t lines_matched = grepbuf (bufbeg, buflim);
              if (lines_matched > 0)
                {
                  if (!atomic_exchange (&binary_stream_reported, true))
                    task_error (0, _("%s: binary file matches"),
                                label ? label : _("(standard input)"));
                  status = false;
                }
              else
                status = true;
            }
          else if (count_matches)
            {
              out_quiet = true;
              chunk_matched_cnt = grepbuf (bufbeg, buflim);
              status = (chunk_matched_cnt == 0);
            }
          else
            {
              encoding_error_output = false;
              intmax_t lines_matched = grepbuf (bufbeg, buflim);
              status = (lines_matched == 0);
              enc_error = (binary_files == BINARY_BINARY_FILES && !out_quiet
                           && encoding_error_output);
            }
        }
      else if (task.type == TASK_TYPE_CHUNK)
        {
          filename = task.filename;
          bufbeg = (char *) task.chunk_data;
          buflim = (char *) task.chunk_data + task.chunk_len;
          totalcc = task.chunk_totalcc;
          totalnl = task.chunk_lineno_offset;
          lastnl = (char *) task.chunk_data;

          if (task.shared_state)
            offset_width = ((struct chunk_file_state *) task.shared_state)->offset_width;
          else
            offset_width = 0;

          encoding_error_output = false;

          if (task.mode == CHUNK_MODE_COUNT)
            {
              struct chunk_file_state *state = task.shared_state;
              if (atomic_load (&state->early_stop))
                {
                  status = true;
                }
              else
                {
                  out_quiet = true;
                  intmax_t cnt = grepbuf (bufbeg, buflim);
                  chunk_matched_cnt = cnt;
                  if (cnt > 0)
                    {
                      atomic_store (&state->has_match, true);
                      if (exit_on_match || list_files == LISTFILES_MATCHING)
                        atomic_store (&state->early_stop, true);
                    }
                  status = (cnt == 0);
                }
            }
          else
            {
              intmax_t lines_matched = grepbuf (bufbeg, buflim);
              status = (lines_matched == 0);
              enc_error = (binary_files == BINARY_BINARY_FILES && !out_quiet
                           && encoding_error_output);
            }
        }

      char *copy_out = nullptr;
      if (tls_out.len > sizeof rob[0].inline_out && tls_out.len <= 32768)
        {
          copy_out = ximalloc (tls_out.len);
          memcpy (copy_out, tls_out.data, tls_out.len);
        }

      pthread_mutex_lock (&pool_mutex);

      if (task.chunk_free_data)
        free_slice_buf_locked ((char *) (task.chunk_data - 1));

      struct rob_slot *slot = &rob[task.seq % ROB_CAP];
      slot->status = status;
      slot->errseen = tls_errseen;
      slot->had_prtext = task_had_prtext;
      slot->sep = task_sep;
      slot->chunk_count = chunk_matched_cnt;
      slot->enc_error = enc_error;
      if (defer_name)
        {
          slot->defer_name = defer_name;
          slot->defer_follow = task.follow;
          slot->defer_command_line = task.command_line;
        }
      if (tls_big.len)
        {
          slot->big = ximemdup (tls_big.data, tls_big.len);
          slot->nbig = tls_big.len / (2 * sizeof (size_t));
        }
      if (task.type == TASK_TYPE_CHUNK || task.type == TASK_TYPE_STREAM_CHUNK)
        {
          slot->is_chunk = true;
          slot->is_last_chunk = task.is_last_chunk;
        }

      if (tls_out.len > 0)
        {
          if (tls_out.len <= sizeof slot->inline_out)
            {
              memcpy (slot->inline_out, tls_out.data, tls_out.len);
              slot->stdout_buf = nullptr;
              slot->stdout_len = tls_out.len;
              slot->stdout_cap = sizeof slot->inline_out;
              tls_out.len = 0;
            }
          else if (copy_out)
            {
              slot->stdout_buf = copy_out;
              slot->stdout_len = tls_out.len;
              slot->stdout_cap = tls_out.len;
              tls_out.len = 0;
            }
          else
            {
              slot->stdout_buf = tls_out.data;
              slot->stdout_len = tls_out.len;
              slot->stdout_cap = tls_out.cap;
              tls_out.data = nullptr;
              tls_out.len = 0;
              tls_out.cap = 0;
              membuf_init (&tls_out, 64 * 1024);
            }
        }

      if (tls_err.len > 0)
        {
          slot->stderr_buf = tls_err.data;
          slot->stderr_len = tls_err.len;
          slot->stderr_cap = tls_err.cap;
          tls_err.data = nullptr;
          tls_err.len = 0;
          tls_err.cap = 0;
          membuf_init (&tls_err, 4096);
        }

      slot->state = ROB_SLOT_DONE;
      rob_try_commit_locked ();
    }

  membuf_free (&tls_out);
  membuf_free (&tls_err);
  free_worker_buffer ();
  walk_free_thread_buffers ();
  membuf_free (&tls_big);
  if (compiled_pattern)
    {
      free_compiled_pattern (compiled_pattern);
      compiled_pattern = NULL;
    }
  return NULL;
}

enum { PRODUCER_BATCH_SIZE = 32 };

struct producer_batch_item
{
  char *filename;
  bool follow;
  bool command_line;
};

static struct producer_batch_item producer_batch[PRODUCER_BATCH_SIZE];
static size_t producer_batch_count = 0;

static void ensure_pool (void);

static void
producer_flush_batch (void)
{
  if (producer_batch_count == 0)
    return;
  ensure_pool ();

  pthread_mutex_lock (&pool_mutex);

  for (size_t i = 0; i < producer_batch_count; ++i)
    {
      if (pool_terminate)
        {
          for (size_t j = i; j < producer_batch_count; ++j)
            free (producer_batch[j].filename);
          producer_batch_count = 0;
          pthread_mutex_unlock (&pool_mutex);
          return;
        }

      uint64_t seq = producer_alloc_slot_locked ();
      if (pool_terminate)
        {
          for (size_t j = i; j < producer_batch_count; ++j)
            free (producer_batch[j].filename);
          producer_batch_count = 0;
          pthread_mutex_unlock (&pool_mutex);
          return;
        }

      while (q_count >= WORK_QUEUE_CAP && !pool_terminate)
        {
          rob_try_commit_locked ();
          if (q_count < WORK_QUEUE_CAP)
            break;
          pthread_cond_wait (&q_not_full_cv, &pool_mutex);
        }

      if (pool_terminate)
        {
          for (size_t j = i; j < producer_batch_count; ++j)
            free (producer_batch[j].filename);
          producer_batch_count = 0;
          pthread_mutex_unlock (&pool_mutex);
          return;
        }

      struct work_task *t = &work_queue[q_tail];
      t->type = TASK_TYPE_FILE;
      t->seq = seq;
      t->filename = producer_batch[i].filename;
      t->follow = producer_batch[i].follow;
      t->command_line = producer_batch[i].command_line;

      q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
      q_count++;
    }

  producer_batch_count = 0;
  pthread_cond_broadcast (&q_not_empty_cv);
  pthread_mutex_unlock (&pool_mutex);
}

static void
producer_enqueue_file (char const *fname, bool follow, bool command_line)
{
  producer_batch[producer_batch_count].filename = xstrdup (fname);
  producer_batch[producer_batch_count].follow = follow;
  producer_batch[producer_batch_count].command_line = command_line;
  producer_batch_count++;

  if (producer_batch_count == PRODUCER_BATCH_SIZE)
    producer_flush_batch ();
}

static void
producer_enqueue_stdin (void)
{
  producer_flush_batch ();
  ensure_pool ();
  pthread_mutex_lock (&pool_mutex);

  uint64_t seq = producer_alloc_slot_locked ();
  if (pool_terminate)
    {
      pthread_mutex_unlock (&pool_mutex);
      return;
    }

  while (q_count >= WORK_QUEUE_CAP && !pool_terminate)
    {
      rob_try_commit_locked ();
      if (q_count < WORK_QUEUE_CAP)
        break;
      pthread_cond_wait (&q_not_full_cv, &pool_mutex);
    }

  if (pool_terminate)
    {
      pthread_mutex_unlock (&pool_mutex);
      return;
    }

  struct work_task *t = &work_queue[q_tail];
  t->type = TASK_TYPE_STDIN;
  t->seq = seq;
  t->filename = nullptr;
  t->follow = false;
  t->command_line = true;

  q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
  q_count++;

  pthread_cond_signal (&q_not_empty_cv);
  pthread_mutex_unlock (&pool_mutex);
}

/* Store into SLOT the diagnostic error (0, ERRNUM, "%s", MSG) would
   output.  */
static void
slot_set_diagnostic (struct rob_slot *slot, int errnum, char const *msg)
{
  size_t len;
  char *text = diagnostic_text (errnum, msg, &len);
  struct membuf mb = { .data = nullptr };
  errrec_add (&mb, 0, false, text, len);
  free (text);
  slot->stderr_buf = mb.data;
  slot->stderr_len = mb.len;
  slot->stderr_cap = mb.cap;
}

static void
producer_enqueue_error (int errnum, char const *fname)
{
  producer_flush_batch ();
  pthread_mutex_lock (&pool_mutex);
  uint64_t seq = producer_alloc_slot_locked ();
  struct rob_slot *slot = &rob[seq % ROB_CAP];

  slot->status = true;
  slot->errseen = true;
  slot->had_prtext = false;

  if (!suppress_errors)
    slot_set_diagnostic (slot, errnum, fname);

  slot->state = ROB_SLOT_DONE;
  rob_try_commit_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

/* Like format_message, but with a variable argument list.  */
static char * _GL_ATTRIBUTE_FORMAT_PRINTF_STANDARD (3, 4)
format_message_l (char *buf, size_t bufsize, char const *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  char *msg = format_message (buf, bufsize, fmt, ap);
  va_end (ap);
  return msg;
}

static void
producer_enqueue_msg (int errnum, char const *fmt, char const *fname)
{
  producer_flush_batch ();
  pthread_mutex_lock (&pool_mutex);
  uint64_t seq = producer_alloc_slot_locked ();
  struct rob_slot *slot = &rob[seq % ROB_CAP];

  slot->status = true;
  slot->errseen = false;
  slot->had_prtext = false;

  if (!suppress_errors)
    {
      char buf[1024];
      char *msg = format_message_l (buf, sizeof buf, fmt, fname);
      slot_set_diagnostic (slot, errnum, msg);
      if (msg != buf)
        free (msg);
    }

  slot->state = ROB_SLOT_DONE;
  rob_try_commit_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

static void
producer_enqueue_errseen (void)
{
  producer_flush_batch ();
  pthread_mutex_lock (&pool_mutex);
  uint64_t seq = producer_alloc_slot_locked ();
  struct rob_slot *slot = &rob[seq % ROB_CAP];
  slot->status = true;
  slot->errseen = true;
  slot->had_prtext = false;
  slot->state = ROB_SLOT_DONE;
  rob_try_commit_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

static _Atomic uint64_t retained_mmap_bytes = 0;

static void
cleanup_chunk_file_state (void *arg)
{
  struct chunk_file_state *state = arg;
  if (!state)
    return;

  bool skip_munmap = false;
  if (state->mmap_size >= (16 << 20))
    {
      uint64_t prev = atomic_fetch_add_explicit (&retained_mmap_bytes,
                                                 state->base_size ? state->base_size : state->mmap_size,
                                                 memory_order_relaxed);
      if (prev + (state->base_size ? state->base_size : state->mmap_size) <= (4ULL << 30))
        skip_munmap = true;
    }

  if (!skip_munmap)
    {
      if (state->base_addr && state->base_addr != MAP_FAILED)
        munmap (state->base_addr, state->base_size);
      else if (state->mmap_data && state->mmap_data != MAP_FAILED)
        munmap (state->mmap_data, state->mmap_size);
    }

  if (state->close_fd && state->file_desc >= 0)
    close (state->file_desc);
  else if (!state->close_fd && state->file_desc >= 0)
    {
      if (!exit_on_match)
        lseek (state->file_desc, state->mmap_size, SEEK_SET);
    }

  free (state->bounds);
  free (state->chunk_nls);
  free (state->prefix_nls);
  free (state->chunk_has_matches);
  free (state->matches);
  if (state->fname_allocated)
    free ((char *) state->fname);

  pthread_mutex_destroy (&state->done_mutex);
  pthread_cond_destroy (&state->done_cv);
  pthread_mutex_destroy (&state->match_mutex);

  free (state);
}

/* Search the file of STATE, named FNAME, as single-threaded grep
   would, into the buffers of the current thread, and return its
   status.  This is for the rare files whose output depends on how
   single-threaded grep happens to divide its input into buffers.  */
static bool
chunk_search_sequentially (struct chunk_file_state *state, char const *fname)
{
  char const *saved_filename = filename;
  filename = fname;
  bool status = grepdesc (state->file_desc, true);
  if (state->close_fd)
    state->file_desc = -1;  /* grepdesc closed it.  */
  filename = saved_filename;
  return status;
}

static int
cmp_matches (void const *a, void const *b)
{
  struct match_record const *ma = a;
  struct match_record const *mb = b;
  if (ma->line_beg < mb->line_beg) return -1;
  if (ma->line_beg > mb->line_beg) return 1;
  return 0;
}

static void
format_context_matches_to_rob (char const *fname, struct chunk_file_state *state)
{
  pthread_mutex_lock (&pool_mutex);
  uint64_t seq = producer_alloc_slot_locked ();
  struct rob_slot *slot = &rob[seq % ROB_CAP];
  slot->is_chunk = true;
  slot->is_last_chunk = true;
  slot->cleanup_fn = cleanup_chunk_file_state;
  slot->cleanup_arg = state;
  slot->filename = xstrdup (fname);

  if (state->match_count == 0)
    {
      slot->status = true;
      slot->state = ROB_SLOT_DONE;
      rob_try_commit_locked ();
      pthread_mutex_unlock (&pool_mutex);
      return;
    }

  /* Sort matches by line_beg */
  qsort (state->matches, state->match_count, sizeof (struct match_record), cmp_matches);

  /* Deduplicate matches by line_beg */
  size_t unique_count = 0;
  for (size_t i = 0; i < state->match_count; ++i)
    {
      if (unique_count == 0 || state->matches[i].line_beg != state->matches[unique_count - 1].line_beg)
        {
          state->matches[unique_count++] = state->matches[i];
        }
    }
  state->match_count = unique_count;

  /* Format output into a membuf */
  struct membuf out;
  membuf_init (&out, 64 * 1024);

  struct membuf saved_out = tls_out;
  struct membuf saved_err = tls_err;
  struct membuf saved_big = tls_big;
  bool saved_buffering = buffering_enabled;
  tls_out = out;
  tls_err = tls_big = (struct membuf) { .data = nullptr };
  buffering_enabled = true;

  char const *saved_filename = filename;
  filename = fname;
  char *saved_bufbeg = bufbeg;
  char *saved_buflim = buflim;
  char const *saved_lastnl = lastnl;
  char *saved_lastout = lastout;
  intmax_t saved_totalcc = totalcc;
  intmax_t saved_totalnl = totalnl;
  intmax_t saved_pending = pending;
  intmax_t saved_outleft = outleft;

  bufbeg = state->mmap_data;
  buflim = state->mmap_data + state->mmap_size;
  lastnl = bufbeg;
  lastout = nullptr;
  totalcc = 0;
  totalnl = 0;
  pending = 0;
  outleft = max_count;
  file_used = false;
  bool saved_had_prtext = task_had_prtext;
  bool saved_sep = task_sep;
  task_had_prtext = false;
  task_sep = false;

  int saved_offset_width = offset_width;
  offset_width = state->offset_width;
  encoding_error_output = false;

  for (size_t i = 0; i < state->match_count; ++i)
    {
      if (outleft == 0)
        break;
      struct match_record *m = &state->matches[i];
      if (!out_quiet && pending > 0)
        prpending ((char *) m->line_beg);

      if (out_line && !out_invert)
        {
          intmax_t match_nl = state->prefix_nls[m->chunk_idx] + m->rel_nl;
          char const *bp = lastout ? lastout : bufbeg;
          char const *p = m->line_beg;
          intmax_t k = 0;
          if (!out_quiet)
            {
              for (; k < out_before && p > bp; ++k)
                do
                  --p;
                while (p[-1] != eolbyte);
            }
          if (lastnl < p)
            {
              totalnl = match_nl - k;
              lastnl = p;
            }
        }

      prtext ((char *) m->line_beg, (char *) m->line_end);
    }
  if (pending > 0)
    prpending (buflim);

  bool status = false;
  if ((0 < out_before || 0 < out_after) && encoding_error_output)
    {
      /* After omitting a line with an encoding error, single-threaded
         grep prints context lines that depend on where its read buffers
         happen to end.  Search the file as it does.  */
      membuf_reset (&tls_out);
      membuf_reset (&tls_err);
      membuf_reset (&tls_big);
      task_had_prtext = false;
      task_sep = false;
      file_used = false;
      pthread_mutex_unlock (&pool_mutex);
      status = chunk_search_sequentially (state, fname);
      pthread_mutex_lock (&pool_mutex);
    }
  else
    {
      task_had_prtext = true;
      if (binary_files == BINARY_BINARY_FILES && !out_quiet
          && encoding_error_output)
        task_error (0, _("%s: binary file matches"), fname);
    }

  offset_width = saved_offset_width;

  out = tls_out;
  tls_out = saved_out;
  struct membuf err = tls_err;
  tls_err = saved_err;
  struct membuf big = tls_big;
  tls_big = saved_big;
  buffering_enabled = saved_buffering;
  filename = saved_filename;
  bufbeg = saved_bufbeg;
  buflim = saved_buflim;
  lastnl = saved_lastnl;
  lastout = saved_lastout;
  totalcc = saved_totalcc;
  totalnl = saved_totalnl;
  pending = saved_pending;
  outleft = saved_outleft;

  slot->status = status;
  slot->had_prtext = task_had_prtext;
  slot->sep = task_sep;
  task_had_prtext = saved_had_prtext;
  task_sep = saved_sep;
  if (out.len <= sizeof slot->inline_out)
    {
      memcpy (slot->inline_out, out.data, out.len);
      slot->stdout_buf = nullptr;
      slot->stdout_len = out.len;
      slot->stdout_cap = sizeof slot->inline_out;
      membuf_free (&out);
    }
  else
    {
      slot->stdout_buf = out.data;
      slot->stdout_len = out.len;
      slot->stdout_cap = out.cap;
    }
  slot->stderr_buf = err.data;
  slot->stderr_len = err.len;
  slot->stderr_cap = err.cap;
  slot->big = CAST_ALIGNED (size_t *, big.data);
  slot->nbig = big.len / (2 * sizeof (size_t));
  slot->state = ROB_SLOT_DONE;

  rob_try_commit_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

static void stream_slicer_stdin (void);

static void
chunked_grep_fd (int fd, bool close_fd, char const *fname, struct stat const *st_in)
{
  ensure_pool ();
  struct stat st;
  if (st_in)
    st = *st_in;
  else if (fstat (fd, &st) != 0)
    {
      if (!suppress_errors)
        producer_enqueue_error (errno, fname);
      else
        producer_enqueue_errseen ();
      if (close_fd)
        close (fd);
      return;
    }

  if (!out_quiet && list_files == LISTFILES_NONE && 1 < max_count
      && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))
    {
      if (close_fd)
        close (fd);
      producer_enqueue_msg (0, _("%s: input file is also the output"), fname);
      producer_enqueue_errseen ();
      return;
    }

  if (st.st_size == 0)
    {
      if (close_fd)
        close (fd);
      pthread_mutex_lock (&pool_mutex);
      uint64_t seq = producer_alloc_slot_locked ();
      struct rob_slot *slot = &rob[seq % ROB_CAP];
      slot->status = true;
      slot->errseen = false;
      slot->had_prtext = false;
      slot->is_chunk = true;
      slot->is_last_chunk = true;
      slot->filename = xstrdup (fname);
      slot->state = ROB_SLOT_DONE;
      rob_try_commit_locked ();
      pthread_mutex_unlock (&pool_mutex);
      return;
    }

  size_t file_size = (size_t) st.st_size;
  size_t total_alloc = file_size + 2 * pagesize;
  char *base = mmap (NULL, total_alloc, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED)
    {
      if (close_fd)
        {
          close (fd);
          producer_enqueue_file (fname, true, true);
        }
      else
        stream_slicer_stdin ();
      return;
    }

  char *data = mmap (base + pagesize, file_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_FIXED, fd, 0);
  if (data == MAP_FAILED)
    {
      munmap (base, total_alloc);
      if (close_fd)
        {
          close (fd);
          producer_enqueue_file (fname, true, true);
        }
      else
        stream_slicer_stdin ();
      return;
    }

  data[-1] = eolbyte;
  data[file_size] = eolbyte;

  if (eolbyte && binary_files != TEXT_BINARY_FILES
      && (memchr (data, '\0', MIN (file_size, 64 * 1024)) != nullptr
          || file_must_have_nulls (0, fd, &st)))
    {
      munmap (base, total_alloc);
      if (close_fd)
        {
          close (fd);
          producer_enqueue_file (fname, true, true);
        }
      else
        stream_slicer_stdin ();
      return;
    }

  size_t num_chunks = pool_nthreads * 2;
  if (num_chunks < 2)
    num_chunks = 2;
  if (num_chunks > 64)
    num_chunks = 64;
  size_t min_chunk_size = 2 * 1024 * 1024;
  if (file_size / num_chunks < min_chunk_size)
    num_chunks = file_size / min_chunk_size;
  if (num_chunks < 2)
    num_chunks = 2;

  size_t *bounds = xnmalloc (num_chunks + 1, sizeof *bounds);
  bounds[0] = 0;
  for (size_t k = 1; k < num_chunks; ++k)
    {
      size_t target = k * (file_size / num_chunks);
      char const *nl = memchr (data + target, eolbyte, file_size - target);
      if (nl)
        bounds[k] = nl + 1 - data;
      else
        bounds[k] = file_size;
    }
  bounds[num_chunks] = file_size;

  struct chunk_file_state *state = xzalloc (sizeof *state);
  state->base_addr = base;
  state->base_size = total_alloc;
  state->mmap_data = data;
  state->mmap_size = file_size;
  state->file_desc = fd;
  state->close_fd = close_fd;
  state->fname = xstrdup (fname);
  state->fname_allocated = true;
  state->num_chunks = num_chunks;
  state->bounds = bounds;
  state->offset_width = 0;
  if (align_tabs)
    {
      intmax_t num = usable_st_size (&st) ? st.st_size : INTMAX_MAX;
      num += out_line && num < INTMAX_MAX;
      do
        state->offset_width++;
      while ((num /= 10) != 0);
    }
  atomic_init (&state->chunks_completed, 0);
  atomic_init (&state->total_count, 0);
  atomic_init (&state->has_match, false);
  atomic_init (&state->early_stop, false);
  pthread_mutex_init (&state->done_mutex, NULL);
  pthread_cond_init (&state->done_cv, NULL);
  pthread_mutex_init (&state->match_mutex, NULL);

  /* Case 1: Count / Early Stop / List Files */
  if (count_matches || exit_on_match || list_files != LISTFILES_NONE)
    {
      for (size_t k = 0; k < num_chunks; ++k)
        {
          size_t c_beg = bounds[k];
          size_t c_end = bounds[k + 1];
          bool is_last = (k == num_chunks - 1);

          pthread_mutex_lock (&pool_mutex);
          uint64_t seq = producer_alloc_slot_locked ();
          struct rob_slot *slot = &rob[seq % ROB_CAP];
          slot->is_chunk = true;
          slot->is_last_chunk = is_last;
          if (is_last)
            {
              slot->cleanup_fn = cleanup_chunk_file_state;
              slot->cleanup_arg = state;
              slot->filename = xstrdup (fname);
            }

          while (q_count >= WORK_QUEUE_CAP && !pool_terminate)
            {
              rob_try_commit_locked ();
              if (q_count < WORK_QUEUE_CAP)
                break;
              pthread_cond_wait (&q_not_full_cv, &pool_mutex);
            }

          if (pool_terminate)
            {
              pthread_mutex_unlock (&pool_mutex);
              return;
            }

          struct work_task *t = &work_queue[q_tail];
          t->type = TASK_TYPE_CHUNK;
          t->seq = seq;
          t->filename = (char *) state->fname;
          t->mode = CHUNK_MODE_COUNT;
          t->chunk_data = data + c_beg;
          t->chunk_len = c_end - c_beg;
          t->chunk_totalcc = c_beg;
          t->chunk_lineno_offset = 0;
          t->chunk_free_data = false;
          t->is_last_chunk = is_last;
          t->shared_state = state;

          q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
          q_count++;

          pthread_cond_signal (&q_not_empty_cv);
          pthread_mutex_unlock (&pool_mutex);
        }
      return;
    }

  /* Case 2: Mode 2: Independent Chunks (!out_line && out_before <= 0 && out_after <= 0 && max_count == INTMAX_MAX) */
  if (!out_line && out_before <= 0 && out_after <= 0 && max_count == INTMAX_MAX)
    {
      for (size_t k = 0; k < num_chunks; ++k)
        {
          size_t c_beg = bounds[k];
          size_t c_end = bounds[k + 1];
          bool is_last = (k == num_chunks - 1);

          pthread_mutex_lock (&pool_mutex);
          uint64_t seq = producer_alloc_slot_locked ();
          struct rob_slot *slot = &rob[seq % ROB_CAP];
          slot->is_chunk = true;
          slot->is_last_chunk = is_last;
          if (is_last)
            {
              slot->cleanup_fn = cleanup_chunk_file_state;
              slot->cleanup_arg = state;
              slot->filename = xstrdup (fname);
            }

          while (q_count >= WORK_QUEUE_CAP && !pool_terminate)
            {
              rob_try_commit_locked ();
              if (q_count < WORK_QUEUE_CAP)
                break;
              pthread_cond_wait (&q_not_full_cv, &pool_mutex);
            }

          if (pool_terminate)
            {
              pthread_mutex_unlock (&pool_mutex);
              return;
            }

          struct work_task *t = &work_queue[q_tail];
          t->type = TASK_TYPE_CHUNK;
          t->seq = seq;
          t->filename = (char *) state->fname;
          t->mode = CHUNK_MODE_STATELESS;
          t->chunk_data = data + c_beg;
          t->chunk_len = c_end - c_beg;
          t->chunk_totalcc = c_beg;
          t->chunk_lineno_offset = 0;
          t->chunk_free_data = false;
          t->is_last_chunk = is_last;
          t->shared_state = state;

          q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
          q_count++;

          pthread_cond_signal (&q_not_empty_cv);
          pthread_mutex_unlock (&pool_mutex);
        }
      return;
    }

  /* Case 3: Mode 3 (Line numbers and/or context) */
  state->chunk_nls = xnmalloc (num_chunks, sizeof *state->chunk_nls);
  state->prefix_nls = xnmalloc (num_chunks + 1, sizeof *state->prefix_nls);
  state->chunk_has_matches = xnmalloc (num_chunks, sizeof *state->chunk_has_matches);
  state->match_cap = (out_before > 0 || out_after > 0) ? 65536 : 4096;
  state->matches = xnmalloc (state->match_cap, sizeof *state->matches);
  state->match_count = 0;
  atomic_init (&state->match_overflow, false);

  /* Pass 1: MMAP Scan tasks */
  pthread_mutex_lock (&pool_mutex);
  while (q_count + num_chunks > WORK_QUEUE_CAP && !pool_terminate)
    {
      rob_try_commit_locked ();
      if (q_count + num_chunks <= WORK_QUEUE_CAP)
        break;
      pthread_cond_wait (&q_not_full_cv, &pool_mutex);
    }

  if (!pool_terminate)
    {
      for (size_t k = 0; k < num_chunks; ++k)
        {
          struct work_task *t = &work_queue[q_tail];
          t->type = TASK_TYPE_MMAP_SCAN;
          t->seq = 0;
          t->filename = (char *) state->fname;
          t->chunk_data = data + bounds[k];
          t->chunk_len = bounds[k + 1] - bounds[k];
          t->chunk_idx = k;
          t->shared_state = state;

          q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
          q_count++;
        }
      pthread_cond_broadcast (&q_not_empty_cv);
    }
  pthread_mutex_unlock (&pool_mutex);

  /* Wait for Pass 1 to finish */
  pthread_mutex_lock (&state->done_mutex);
  while (atomic_load (&state->chunks_completed) < num_chunks)
    pthread_cond_wait (&state->done_cv, &state->done_mutex);
  pthread_mutex_unlock (&state->done_mutex);

  /* Prefix sum of newlines */
  state->prefix_nls[0] = 0;
  for (size_t k = 0; k < num_chunks; ++k)
    state->prefix_nls[k + 1] = state->prefix_nls[k] + state->chunk_nls[k];

  /* Sparse match fast-path (context lines or rare matches with line numbers/byte offsets) */
  if ((out_before > 0 || out_after > 0)
      || (!out_invert && !count_matches && list_files == LISTFILES_NONE
          && !atomic_load (&state->match_overflow) && state->match_count < state->match_cap))
    {
      format_context_matches_to_rob (fname, state);
      return;
    }

  /* Mode 3A: Line numbers without context */
  pthread_mutex_lock (&pool_mutex);
  while (q_count + num_chunks > WORK_QUEUE_CAP && !pool_terminate)
    {
      rob_try_commit_locked ();
      if (q_count + num_chunks <= WORK_QUEUE_CAP)
        break;
      pthread_cond_wait (&q_not_full_cv, &pool_mutex);
    }

  if (!pool_terminate)
    {
      for (size_t k = 0; k < num_chunks; ++k)
        {
          size_t c_beg = bounds[k];
          size_t c_end = bounds[k + 1];
          bool is_last = (k == num_chunks - 1);

          uint64_t seq = producer_alloc_slot_locked ();
          struct rob_slot *slot = &rob[seq % ROB_CAP];
          slot->is_chunk = true;
          slot->is_last_chunk = is_last;
          if (is_last)
            {
              slot->cleanup_fn = cleanup_chunk_file_state;
              slot->cleanup_arg = state;
              slot->filename = xstrdup (fname);
            }

          if (!state->chunk_has_matches[k])
            {
              slot->status = true;
              slot->state = ROB_SLOT_DONE;
              continue;
            }

          struct work_task *t = &work_queue[q_tail];
          t->type = TASK_TYPE_CHUNK;
          t->seq = seq;
          t->filename = (char *) state->fname;
          t->mode = CHUNK_MODE_LINE_NUMBER;
          t->chunk_data = data + c_beg;
          t->chunk_len = c_end - c_beg;
          t->chunk_totalcc = c_beg;
          t->chunk_lineno_offset = state->prefix_nls[k];
          t->chunk_free_data = false;
          t->is_last_chunk = is_last;
          t->shared_state = state;

          q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
          q_count++;
        }
      rob_try_commit_locked ();
      pthread_cond_broadcast (&q_not_empty_cv);
    }
  pthread_mutex_unlock (&pool_mutex);
}

static void
chunked_grep_file (char const *fname, struct stat const *st_in)
{
  int oflag = (O_RDONLY | O_NOCTTY
               | (IGNORE_DUPLICATE_BRANCH_WARNING (binary ? O_BINARY : 0)));
  int fd = openat_safer (AT_FDCWD, fname, oflag);
  if (fd < 0)
    {
      if (!suppress_errors)
        producer_enqueue_error (errno, fname);
      else
        producer_enqueue_errseen ();
      return;
    }
  chunked_grep_fd (fd, true, fname, st_in);
}

static void
stream_slicer_stdin (void)
{
#ifdef F_SETPIPE_SZ
  fcntl (STDIN_FILENO, F_SETPIPE_SZ, 1024 * 1024);
#endif
  stream_commit_totalnl = 0;
  ensure_pool ();

  struct stat st;
  if (fstat (STDIN_FILENO, &st) == 0)
    {
      if (!out_quiet && list_files == LISTFILES_NONE && 1 < max_count
          && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))
        {
          producer_enqueue_msg (0, _("%s: input file is also the output"),
                                label ? label : _("(standard input)"));
          producer_enqueue_errseen ();
          return;
        }
    }

  size_t residue_cap = 64 * 1024;
  char *residue_buf = xmalloc (residue_cap);
  size_t residue_len = 0;
  intmax_t stream_totalcc = 0;
  bool is_first_chunk = true;
  bool is_binary = false;

  while (true)
    {
      pthread_mutex_lock (&pool_mutex);
      char *chunk_alloc = alloc_slice_buf_locked ();
      pthread_mutex_unlock (&pool_mutex);

      chunk_alloc[0] = eolbyte;
      char *chunk_data = chunk_alloc + 1;

      if (residue_len > 0)
        memcpy (chunk_data, residue_buf, residue_len);

      size_t total_in_buf = residue_len;
      bool is_eof = false;

      while (total_in_buf < SLICE_BUF_SIZE)
        {
          ssize_t nread = safe_read (STDIN_FILENO, chunk_data + total_in_buf,
                                     SLICE_BUF_SIZE - total_in_buf);
          if (nread <= 0)
            {
              if (nread < 0)
                suppressible_error (errno);
              is_eof = true;
              break;
            }
          total_in_buf += nread;
        }

      if (total_in_buf == 0)
        {
          pthread_mutex_lock (&pool_mutex);
          free_slice_buf_locked (chunk_alloc);
          if (is_first_chunk)
            {
              uint64_t seq = producer_alloc_slot_locked ();
              struct rob_slot *slot = &rob[seq % ROB_CAP];
              slot->status = true;
              slot->errseen = false;
              slot->had_prtext = false;
              slot->is_chunk = true;
              slot->is_last_chunk = true;
              slot->filename = label ? xstrdup (label) : nullptr;
              slot->state = ROB_SLOT_DONE;
              rob_try_commit_locked ();
            }
          pthread_mutex_unlock (&pool_mutex);
          break;
        }

      if (is_first_chunk)
        {
          is_first_chunk = false;
          if (eolbyte && binary_files != TEXT_BINARY_FILES
              && buf_has_nulls (chunk_data, total_in_buf))
            {
              if (binary_files == WITHOUT_MATCH_BINARY_FILES)
                {
                  pthread_mutex_lock (&pool_mutex);
                  free_slice_buf_locked (chunk_alloc);
                  uint64_t seq = producer_alloc_slot_locked ();
                  struct rob_slot *slot = &rob[seq % ROB_CAP];
                  slot->status = true;
                  slot->errseen = false;
                  slot->had_prtext = false;
                  slot->is_chunk = true;
                  slot->is_last_chunk = true;
                  slot->filename = label ? xstrdup (label) : nullptr;
                  slot->state = ROB_SLOT_DONE;
                  rob_try_commit_locked ();
                  pthread_mutex_unlock (&pool_mutex);
                  break;
                }
              is_binary = true;
            }
        }

      if (is_binary)
        zap_nuls (chunk_data, chunk_data + total_in_buf, eolbyte);

      size_t chunk_len = 0;
      size_t next_residue = 0;

      if (is_eof)
        {
          if (total_in_buf > 0 && chunk_data[total_in_buf - 1] != eolbyte)
            {
              chunk_data[total_in_buf] = eolbyte;
              total_in_buf++;
            }
          chunk_len = total_in_buf;
          next_residue = 0;
        }
      else
        {
          char const *last_eol = memrchr (chunk_data, eolbyte, total_in_buf);
          if (last_eol)
            {
              chunk_len = last_eol + 1 - chunk_data;
              next_residue = total_in_buf - chunk_len;
            }
          else
            {
              chunk_len = total_in_buf;
              next_residue = 0;
            }
        }

      if (next_residue > 0)
        {
          if (next_residue > residue_cap)
            {
              residue_cap = next_residue + 4096;
              residue_buf = xrealloc (residue_buf, residue_cap);
            }
          memcpy (residue_buf, chunk_data + chunk_len, next_residue);
        }
      residue_len = next_residue;

      chunk_data[chunk_len] = eolbyte;
      memset (chunk_data + chunk_len + 1, 0, uword_size);

      pthread_mutex_lock (&pool_mutex);

      while (next_seq - commit_seq >= 56 && !pool_terminate)
        {
          rob_try_commit_locked ();
          if (next_seq - commit_seq < 56)
            break;
          rob_space_waiters++;
          pthread_cond_wait (&rob_space_cv, &pool_mutex);
          rob_space_waiters--;
        }

      uint64_t seq = producer_alloc_slot_locked ();
      struct rob_slot *slot = &rob[seq % ROB_CAP];
      slot->is_chunk = true;
      slot->is_last_chunk = is_eof;
      if (is_eof)
        slot->filename = label ? xstrdup (label) : nullptr;

      while (q_count >= WORK_QUEUE_CAP && !pool_terminate)
        {
          rob_try_commit_locked ();
          if (q_count < WORK_QUEUE_CAP)
            break;
          pthread_cond_wait (&q_not_full_cv, &pool_mutex);
        }

      if (pool_terminate)
        {
          free_slice_buf_locked (chunk_alloc);
          pthread_mutex_unlock (&pool_mutex);
          break;
        }

      struct work_task *t = &work_queue[q_tail];
      t->type = TASK_TYPE_STREAM_CHUNK;
      t->seq = seq;
      t->filename = nullptr;
      t->chunk_data = chunk_data;
      t->chunk_len = chunk_len;
      t->chunk_totalcc = stream_totalcc;
      t->chunk_lineno_offset = 0;
      t->chunk_free_data = true;
      t->is_last_chunk = is_eof;
      t->is_binary_stream = is_binary;
      t->shared_state = nullptr;

      q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
      q_count++;

      pthread_cond_signal (&q_not_empty_cv);
      pthread_mutex_unlock (&pool_mutex);

      stream_totalcc += chunk_len;

      if (is_eof)
        break;
    }

  free (residue_buf);
}

/* Parallel ordered directory traversal.

   A directory operand of a recursive search is traversed by tasks that
   any thread may run: a task either reads one directory or searches a
   batch of files of one directory.  Output is nevertheless committed in
   exactly the order of grep's fts-based traversal.  Each directory node
   keeps its entries in fts order, and a single commit cursor walks the
   tree of nodes depth first, committing the results of each entry once
   they are ready.  Tasks are run in that same order as far as possible,
   so that the cursor seldom waits and little output is held back.

   The traversal reproduces what grep observes from fts with the options
   it uses, FTS_CWDFD | FTS_NOSTAT | FTS_TIGHT_CYCLE_CHECK plus either
   FTS_PHYSICAL | FTS_COMFOLLOW (-r) or FTS_LOGICAL (-R):

   - Entries are visited in readdir order, except that fts sorts each
     batch of more than 10,000 entries by inode number, unless it knows
     that the file system does not benefit; it knows the file system
     type only with FTS_CWDFD.
   - A directory whose device and inode numbers equal those of one of its
     ancestors is a loop.
   - With -r, entries are opened relative to their directory, so file
     names may be longer than PATH_MAX.  With -R, which implies
     FTS_NOCHDIR, entries are accessed by their full names.
   - Diagnostics appear where fts-based grep issues them, after the same
     checks for exclusion patterns, devices and symbolic links.  */

/* Maximum number of files searched by one task.  */
enum { WALK_BATCH = 32 };

/* gnulib fts reads at most this many entries of a directory at a time,
   and sorts a batch by inode number if it has more than
   WALK_INODE_SORT_MIN entries.  */
enum { WALK_READDIR_BATCH = 100000 };
enum { WALK_INODE_SORT_MIN = 10000 };

enum walk_kind
{
  WALK_SKIP,                    /* nothing to search */
  WALK_FILE,                    /* a file to search */
  WALK_DIR,                     /* a directory to read and descend into */
};

/* Output captured for one entry: OUTLEN bytes of standard output,
   ERRLEN bytes of diagnostic records (see struct errrec) and NBIG
   recorded large writes (see TLS_BIG).  */
struct walk_out
{
  size_t outlen, errlen, nbig;
  /* NBIG pairs of offsets and lengths, followed by the output and the
     diagnostic records.  */
  size_t big[];
};

struct walk_dir;

/* An entry of a directory.  Entries are kept in fts order.  */
struct walk_ent
{
  char const *name;
  ino_t ino;                    /* d_ino, for sorting */
  struct walk_out *out;         /* output to commit, or null */
  struct walk_dir *sub;         /* WALK_DIR: the directory, once read */
  unsigned char type;           /* d_type */
  unsigned char kind;           /* enum walk_kind */
  bool status;                  /* true if no lines were selected */
  bool errseen;                 /* an error was diagnosed */
  bool used;                    /* prtext was called */
  bool sep;                     /* see TASK_SEP */
  bool defer;                   /* the output file; see TLS_MAY_DEFER */
  atomic_bool done;             /* the entry can be committed */
};

/* A directory being traversed.  */
struct walk_dir
{
  struct walk_dir *parent;      /* null for the holder of the root */
  size_t pindex;                /* index of this directory in PARENT */
  size_t depth;                 /* 0 for the holder of the root */
  struct walk_ent *ents;
  size_t nents;
  size_t cpos;                  /* position of the commit cursor */
  char *path;                   /* fts_path of this directory */
  size_t pathlen;
  char *names;                  /* storage for the names of ENTS */
  struct walk_out *post;        /* diagnostic to output after ENTS */
  dev_t dev;
  ino_t ino;
  atomic_int fd;                /* open descriptor, or -1 */
  atomic_int fd_users;          /* pending tasks that need FD */
  atomic_int refs;              /* references to this structure */
  struct walk_dir *fd_prev, *fd_next;   /* list of directories with FD */
};

/* A task: read the directory DIR->ents[LO], or search the files among
   DIR->ents[LO..HI-1].  */
struct walk_task
{
  struct walk_dir *dir;
  size_t lo, hi;
  bool read;
};

/* A binary heap of tasks, earliest in fts order first.  */
struct walk_heap
{
  struct walk_task **v;
  idx_t n, alloc;
};

static struct
{
  pthread_mutex_t mu;           /* protects the members up to INSIDE */
  pthread_cond_t cv;            /* tasks became available, or done */
  pthread_cond_t left_cv;       /* INSIDE dropped to zero */
  struct walk_heap reads, files;
  int idle;                     /* threads waiting on CV */
  int running;                  /* threads running a task or committing */
  int inside;                   /* threads in walk_participate */
  unsigned int gen;             /* number of the current traversal */
  bool done;                    /* the traversal is fully committed */

  bool logical;                 /* -R: use full names */
  int fd_cap;                   /* soft limit on open directories */

  pthread_mutex_t fd_mu;        /* protects the members below */
  atomic_int open_fds;          /* directories with open descriptors */
  struct walk_dir *fd_list;     /* those directories */

  pthread_mutex_t commit_mu;    /* held by the committer */
  atomic_ulong commit_req;      /* commit attempts so far */
  struct walk_dir *cur;         /* directory of the commit cursor */
} walk =
  {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
    .left_cv = PTHREAD_COND_INITIALIZER,
    .fd_mu = PTHREAD_MUTEX_INITIALIZER,
    .commit_mu = PTHREAD_MUTEX_INITIALIZER,
  };

/* Whether the worker threads were asked to join the current traversal,
   when it started, and whether the current thread is the main thread,
   which recruits them.  */
static bool walk_recruited;
static struct timespec walk_start;
static _Thread_local bool walk_is_main;

/* The main thread traverses alone for this many nanoseconds before it
   recruits the worker threads.  Searches that end sooner, such as -q
   with an early match, then never start them.  */
#ifndef WALK_RECRUIT_NS
# define WALK_RECRUIT_NS (1000 * 1000)
#endif

/* Per-thread buffers for names and directory entries.  */
static _Thread_local char *walk_pbuf;
static _Thread_local idx_t walk_pbufsize;
static _Thread_local char *walk_dbuf;
enum { WALK_DBUF_SIZE = 64 * 1024 };

static void
walk_free_thread_buffers (void)
{
  free (walk_pbuf);
  walk_pbuf = nullptr;
  walk_pbufsize = 0;
  free (walk_dbuf);
  walk_dbuf = nullptr;
}

/* Flags with which fts opens directories, except O_NOFOLLOW.  */
enum { WALK_DIR_FLAGS = (O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOCTTY
                         | O_NONBLOCK) };

/* Return FD, or a duplicate of it that is not a standard descriptor.  */
static int
walk_fd_safer (int fd)
{
  if (0 <= fd && fd <= STDERR_FILENO)
    {
      int nfd = fcntl (fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      int err = errno;
      close (fd);
      errno = err;
      fd = nfd;
    }
  return fd;
}

/* Store into WALK_PBUF the fts_path of entry NAME of directory D, and
   return it.  */
static char *
walk_path (struct walk_dir const *d, char const *name)
{
  size_t dlen = d->pathlen;
  if (dlen && d->path[dlen - 1] == '/')
    dlen--;
  size_t nlen = strlen (name);
  idx_t need = dlen + 1 + nlen + 1;
  if (walk_pbufsize < need)
    {
      free (walk_pbuf);
      walk_pbufsize = need;
      walk_pbuf = xpalloc (nullptr, &walk_pbufsize, need, -1, 1);
    }
  char *p = mempcpy (walk_pbuf, d->path, dlen);
  *p++ = '/';
  memcpy (p, name, nlen + 1);
  return walk_pbuf;
}

/* Return how grep names file name PATH in diagnostics and output.  */
static char const *
walk_shown_name (char const *path)
{
  return omit_dot_slash && path[1] ? path + 2 : path;
}

/* Return the fts_path of entry E of D, in WALK_PBUF unless E is the
   root.  */
static char *
walk_ent_path (struct walk_dir const *d, struct walk_ent const *e)
{
  return d->parent ? walk_path (d, e->name) : (char *) e->name;
}

/* Return a new output record holding diagnostic error (0, ERRNUM, FMT,
   NAME) would output, or null if diagnostics are suppressed.  */
static struct walk_out *
walk_diagnostic (int errnum, char const *fmt, char const *name)
{
  if (suppress_errors)
    return nullptr;
  char buf[1024];
  char *msg = format_message_l (buf, sizeof buf, fmt, name);
  size_t len;
  char *text = diagnostic_text (errnum, msg, &len);
  if (msg != buf)
    free (msg);
  struct walk_out *o = ximalloc (sizeof *o + sizeof (struct errrec) + len);
  o->outlen = o->nbig = 0;
  o->errlen = sizeof (struct errrec) + len;
  struct errrec h = { .outpos = 0, .len = len, .after_sep = false };
  char *p = (char *) o->big;
  memcpy (p, &h, sizeof h);
  memcpy (p + sizeof h, text, len);
  free (text);
  return o;
}

/* Record that entry E of directory D got error ERRNUM, as
   suppressible_error would report it.  */
static void
walk_ent_error (struct walk_dir const *d, struct walk_ent *e, int errnum)
{
  e->out = walk_diagnostic (errnum, "%s", walk_shown_name (walk_ent_path (d, e)));
  e->errseen = true;
}

/* Record that the status of entry E of D could not be determined, which
   fts reports as FTS_NS; grep then treats the entry as a file.  */
static void
walk_ent_stat_error (struct walk_dir const *d, struct walk_ent *e,
                     int errnum)
{
  if (!skipped_file (e->name, false, false))
    walk_ent_error (d, e, errnum);
}

/* Record that the directory entry E of D is a loop (FTS_DC).  */
static void
walk_ent_loop (struct walk_dir const *d, struct walk_ent *e)
{
  e->out = walk_diagnostic (0, _("%s: warning: recursive directory loop"),
                            walk_shown_name (walk_ent_path (d, e)));
}

/* Return true if a directory with device DEV and inode INO is D or one
   of its ancestors, i.e., entering it would loop.  */
static bool _GL_ATTRIBUTE_PURE
walk_is_loop (struct walk_dir const *d, dev_t dev, ino_t ino)
{
  for (; d->parent; d = d->parent)
    if (d->ino == ino && d->dev == dev)
      return true;
  return false;
}

/* Return true if position I of directory A comes before position J of
   directory B in fts order.  A directory entry comes before the entries
   of the directory.  */
static bool _GL_ATTRIBUTE_PURE
walk_before (struct walk_dir const *a, size_t i,
             struct walk_dir const *b, size_t j)
{
  bool a_up = false, b_up = false;
  while (b->depth < a->depth)
    {
      i = a->pindex;
      a = a->parent;
      a_up = true;
    }
  while (a->depth < b->depth)
    {
      j = b->pindex;
      b = b->parent;
      b_up = true;
    }
  while (a != b)
    {
      i = a->pindex;
      a = a->parent;
      j = b->pindex;
      b = b->parent;
      a_up = b_up = true;
    }
  return i != j ? i < j : !a_up && b_up;
}

static bool _GL_ATTRIBUTE_PURE
walk_task_before (struct walk_task const *a, struct walk_task const *b)
{
  return walk_before (a->dir, a->lo, b->dir, b->lo);
}

static void
walk_heap_push (struct walk_heap *h, struct walk_task *t)
{
  if (h->n == h->alloc)
    h->v = xpalloc (h->v, &h->alloc, 1, -1, sizeof *h->v);
  idx_t k = h->n++;
  while (0 < k)
    {
      idx_t p = (k - 1) / 2;
      if (!walk_task_before (t, h->v[p]))
        break;
      h->v[k] = h->v[p];
      k = p;
    }
  h->v[k] = t;
}

static struct walk_task *
walk_heap_pop (struct walk_heap *h)
{
  struct walk_task *top = h->v[0];
  struct walk_task *last = h->v[--h->n];
  idx_t k = 0;
  for (;;)
    {
      idx_t c = 2 * k + 1;
      if (h->n <= c)
        break;
      if (c + 1 < h->n && walk_task_before (h->v[c + 1], h->v[c]))
        c++;
      if (!walk_task_before (h->v[c], last))
        break;
      h->v[k] = h->v[c];
      k = c;
    }
  if (0 < h->n)
    h->v[k] = last;
  return top;
}

static void
walk_free_out (struct walk_out *o)
{
  free (o);
}

static void
walk_unref (struct walk_dir *d)
{
  if (atomic_fetch_sub (&d->refs, 1) == 1)
    {
      for (size_t i = 0; i < d->nents; i++)
        walk_free_out (d->ents[i].out);
      walk_free_out (d->post);
      free (d->ents);
      free (d->names);
      free (d->path);
      free (d);
    }
}

/* Make FD the open descriptor of D.  WALK.fd_mu must be held.  */
static void
walk_set_fd_locked (struct walk_dir *d, int fd)
{
  atomic_store_explicit (&d->fd, fd, memory_order_relaxed);
  atomic_fetch_add (&walk.open_fds, 1);
  d->fd_prev = nullptr;
  d->fd_next = walk.fd_list;
  if (walk.fd_list)
    walk.fd_list->fd_prev = d;
  walk.fd_list = d;
}

/* Close the descriptor of D.  WALK.fd_mu must be held.  */
static void
walk_close_fd_locked (struct walk_dir *d)
{
  int fd = atomic_load_explicit (&d->fd, memory_order_relaxed);
  if (fd < 0)
    return;
  close (fd);
  atomic_store_explicit (&d->fd, -1, memory_order_relaxed);
  atomic_fetch_sub (&walk.open_fds, 1);
  if (d->fd_prev)
    d->fd_prev->fd_next = d->fd_next;
  else
    walk.fd_list = d->fd_next;
  if (d->fd_next)
    d->fd_next->fd_prev = d->fd_prev;
}

/* A task that needed the descriptor of D is done with it.  */
static void
walk_fd_release (struct walk_dir *d)
{
  if (!walk.logical && atomic_fetch_sub (&d->fd_users, 1) == 1)
    {
      pthread_mutex_lock (&walk.fd_mu);
      walk_close_fd_locked (d);
      pthread_mutex_unlock (&walk.fd_mu);
    }
}

/* Open D again after its descriptor was closed to save descriptors,
   checking that it is still the same directory.  Open the directories
   between D and its nearest open ancestor only temporarily.
   WALK.fd_mu must be held.  Return the descriptor, or -1 (setting
   errno) on failure.  */
static int
walk_reopen_locked (struct walk_dir *d)
{
  int fd = atomic_load_explicit (&d->fd, memory_order_relaxed);
  if (0 <= fd)
    return fd;

  /* CHAIN[0] is D, and CHAIN[N - 1] the topmost directory to open,
     either the root or a child of the open directory BASE.  */
  struct walk_dir **chain = nullptr;
  idx_t n = 0, alloc = 0;
  int base = -1;
  for (struct walk_dir *a = d; ; a = a->parent)
    {
      if (n == alloc)
        chain = xpalloc (chain, &alloc, 1, -1, sizeof *chain);
      chain[n++] = a;
      if (!a->parent->parent)
        break;
      base = atomic_load_explicit (&a->parent->fd, memory_order_relaxed);
      if (0 <= base)
        break;
    }

  int cur = base;
  for (idx_t k = n; 0 < k--; )
    {
      struct walk_dir *x = chain[k];
      int nfd = (x->parent->parent
                 ? openat (cur, x->parent->ents[x->pindex].name,
                           WALK_DIR_FLAGS | O_NOFOLLOW)
                 : open (x->path, WALK_DIR_FLAGS));
      int err = errno;
      if (cur != base)
        close (cur);
      nfd = walk_fd_safer (nfd);
      struct stat st;
      if (nfd < 0 || fstat (nfd, &st) != 0
          || st.st_dev != x->dev || st.st_ino != x->ino)
        {
          if (0 <= nfd)
            {
              close (nfd);
              err = ENOENT;
            }
          free (chain);
          errno = err;
          return -1;
        }
      cur = nfd;
    }
  free (chain);
  walk_set_fd_locked (d, cur);
  return cur;
}

/* Return a descriptor open on D, or -1 (setting errno) on failure.  */
static int
walk_dir_fd (struct walk_dir *d)
{
  int fd = atomic_load_explicit (&d->fd, memory_order_relaxed);
  if (0 <= fd)
    return fd;
  pthread_mutex_lock (&walk.fd_mu);
  fd = walk_reopen_locked (d);
  pthread_mutex_unlock (&walk.fd_mu);
  return fd;
}

/* Too many directories are open and no task is running.  Close the
   descriptors of all directories other than KEEP; they are needed only
   by pending tasks, which open them again on demand.  WALK.mu must be
   held.  */
static void
walk_relieve_fd_pressure (struct walk_dir *keep)
{
  pthread_mutex_lock (&walk.fd_mu);
  for (struct walk_dir *d = walk.fd_list, *next; d; d = next)
    {
      next = d->fd_next;
      if (d != keep)
        walk_close_fd_locked (d);
    }
  pthread_mutex_unlock (&walk.fd_mu);
}

/* Create a directory node for entry PINDEX of PARENT, whose fts_path is
   PATH with length PATHLEN.  */
static struct walk_dir *
walk_dir_new (struct walk_dir *parent, size_t pindex, char const *path,
              size_t pathlen)
{
  struct walk_dir *d = xzalloc (sizeof *d);
  d->parent = parent;
  d->pindex = pindex;
  d->depth = parent ? parent->depth + 1 : 0;
  d->path = ximemdup0 (path, pathlen);
  d->pathlen = pathlen;
  atomic_init (&d->fd, -1);
  atomic_init (&d->fd_users, 0);
  atomic_init (&d->refs, 1);
  return d;
}

/* Append entry NAME with type TYPE and inode INO to the arrays being
   built for D.  Names are stored at offsets until the arrays are final.  */
static void
walk_add_ent (struct walk_dir *d, idx_t *nalloc, idx_t *namesalloc,
              idx_t *namesused, char const *name, unsigned char type,
              ino_t ino)
{
  idx_t len = strlen (name) + 1;
  if (*namesalloc - *namesused < len)
    d->names = xpalloc (d->names, namesalloc, len - (*namesalloc - *namesused),
                        -1, 1);
  memcpy (d->names + *namesused, name, len);
  if (d->nents == *nalloc)
    d->ents = xpalloc (d->ents, nalloc, 1, -1, sizeof *d->ents);
  struct walk_ent *e = &d->ents[d->nents++];
  memset (e, 0, sizeof *e);
  e->name = (char const *) *namesused;
  e->ino = ino;
  e->type = type;
  e->status = true;
  *namesused += len;
}

/* Read the entries of the directory open on FD into D.  Return 0 on
   success, or an errno value; then the entries read so far are kept.  */
static int
walk_read_entries (int fd, struct walk_dir *d)
{
  idx_t nalloc = 0, namesalloc = 0, namesused = 0;
  int err = 0;
#if (defined __linux__ && defined __GLIBC__ \
     && 2 < __GLIBC__ + (30 <= __GLIBC_MINOR__))
  if (!walk_dbuf)
    walk_dbuf = ximalloc (WALK_DBUF_SIZE);
  for (;;)
    {
      ssize_t n = getdents64 (fd, walk_dbuf, WALK_DBUF_SIZE);
      if (n <= 0)
        {
          if (n < 0)
            err = errno;
          break;
        }
      for (char *p = walk_dbuf; p < walk_dbuf + n; )
        {
          struct dirent64 *de = CAST_ALIGNED (struct dirent64 *, p);
          p += de->d_reclen;
          char const *nm = de->d_name;
          if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))
            continue;
          walk_add_ent (d, &nalloc, &namesalloc, &namesused, nm, de->d_type,
                        de->d_ino);
        }
    }
#else
  int dfd = fcntl (fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  DIR *dirp = dfd < 0 ? nullptr : fdopendir (dfd);
  if (!dirp)
    {
      err = errno;
      if (0 <= dfd)
        close (dfd);
    }
  else
    {
      for (;;)
        {
          errno = 0;
          struct dirent *de = readdir (dirp);
          if (!de)
            {
              err = errno;
              break;
            }
          char const *nm = de->d_name;
          if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))
            continue;
          walk_add_ent (d, &nalloc, &namesalloc, &namesused, nm,
# ifdef _DIRENT_HAVE_D_TYPE
                        de->d_type,
# else
                        DT_UNKNOWN,
# endif
                        de->d_ino);
        }
      closedir (dirp);
    }
#endif
  for (size_t i = 0; i < d->nents; i++)
    d->ents[i].name = d->names + (intptr_t) d->ents[i].name;
  return err;
}

/* Return true if fts would sort large batches of entries of the
   directory open on FD by inode number.  */
static bool
walk_inode_sort_useful (int fd)
{
  if (walk.logical)
    return true;
#if defined __linux__ && HAVE_SYS_VFS_H && HAVE_FSTATFS
  struct statfs fs;
  if (fstatfs (fd, &fs) == 0)
    switch (fs.f_type)
      {
      case 0x0BD00BD0:          /* Lustre */
      case 0xFF534D42:          /* CIFS */
      case 0x6969:              /* NFS */
      case 0x01021994:          /* tmpfs */
        return false;
      }
#endif
  return true;
}

static int
walk_ino_cmp (void const *a, void const *b)
{
  struct walk_ent const *const *pa = a;
  struct walk_ent const *const *pb = b;
  return _GL_CMP ((*pa)->ino, (*pb)->ino);
}

/* Sort the entries of D, open on FD, as fts would.  */
static void
walk_sort (struct walk_dir *d, int fd)
{
  if (d->nents <= WALK_INODE_SORT_MIN || !walk_inode_sort_useful (fd))
    return;
  struct walk_ent *sorted = xnmalloc (d->nents, sizeof *sorted);
  struct walk_ent **ptrs = xnmalloc (MIN (d->nents, WALK_READDIR_BATCH),
                                     sizeof *ptrs);
  for (size_t b = 0; b < d->nents; b += WALK_READDIR_BATCH)
    {
      size_t n = MIN (d->nents - b, WALK_READDIR_BATCH);
      for (size_t i = 0; i < n; i++)
        ptrs[i] = &d->ents[b + i];
      if (WALK_INODE_SORT_MIN < n)
        qsort (ptrs, n, sizeof *ptrs, walk_ino_cmp);
      for (size_t i = 0; i < n; i++)
        sorted[b + i] = *ptrs[i];
    }
  free (ptrs);
  free (d->ents);
  d->ents = sorted;
}

/* Classify entry E of D as grepdirent would, given the status ST of the
   file it names, as fts_stat would have obtained it.  */
static void
walk_classify_stat (struct walk_dir *d, struct walk_ent *e,
                    struct stat const *st)
{
  if (S_ISDIR (st->st_mode))
    e->kind = WALK_DIR;
  else if (skipped_file (e->name, false, false) || S_ISLNK (st->st_mode)
           || (!S_ISREG (st->st_mode) && skip_devices (false)
               && is_device_mode (st->st_mode)))
    e->kind = WALK_SKIP;
  else
    e->kind = WALK_FILE;
  (void) d;
}

/* Classify the entries of D, open on FD, as grepdirent would.  Entries
   that need no further work are marked done.  */
static void
walk_classify (struct walk_dir *d, int fd)
{
  for (size_t i = 0; i < d->nents; i++)
    {
      struct walk_ent *e = &d->ents[i];
      unsigned char t = e->type;
      if (t == DT_DIR)
        e->kind = WALK_DIR;
      else if (t == DT_REG || (t != DT_LNK && t != DT_UNKNOWN))
        {
          /* fts does not stat these; grepdirent checks the type.  */
          mode_t m = DTTOIF (t);
          e->kind = (skipped_file (e->name, false, false)
                     || (t != DT_REG && skip_devices (false)
                         && is_device_mode (m))
                     ? WALK_SKIP : WALK_FILE);
        }
      else if (!walk.logical && t == DT_LNK)
        {
          /* Opening it with O_NOFOLLOW would fail, silently.  */
          e->kind = WALK_SKIP;
        }
      else
        {
          /* fts_stat.  */
          struct stat st;
          bool ok;
          if (walk.logical)
            {
              char *path = walk_path (d, e->name);
              ok = stat (path, &st) == 0;
              if (!ok && errno == ENOENT && lstat (path, &st) == 0)
                {
                  /* FTS_SLNONE: grep tries to open it.  */
                  e->kind = (skipped_file (e->name, false, false)
                             ? WALK_SKIP : WALK_FILE);
                  goto classified;
                }
            }
          else
            ok = fstatat (fd, e->name, &st, AT_SYMLINK_NOFOLLOW) == 0;
          if (ok)
            walk_classify_stat (d, e, &st);
          else
            {
              e->kind = WALK_SKIP;
              walk_ent_stat_error (d, e, errno);
            }
        }
    classified:
      if (e->kind == WALK_SKIP)
        atomic_init (&e->done, true);
    }
}

/* Search entry E of D, whose directory is open on DFD (unless -R), into
   the buffers of the current thread, and record the results in E.  If
   MAY_DEFER, leave the output file to the committer.  */
static void
walk_search (struct walk_dir *d, int dfd, struct walk_ent *e, bool may_defer)
{
  char *path = walk_path (d, e->name);
  task_reset ();
  filename = walk_shown_name (path);
  tls_may_defer = may_defer && exact_stdout;
  e->status = grepfile (dfd, walk.logical ? path : e->name, walk.logical,
                        false);
  tls_may_defer = false;
  e->errseen = tls_errseen;
  e->used = task_had_prtext;
  e->sep = task_sep;
  if (tls_deferred)
    {
      e->defer = true;
      return;
    }
  if (tls_out.len | tls_err.len)
    {
      struct walk_out *o = ximalloc (sizeof *o + tls_big.len + tls_out.len
                                     + tls_err.len);
      o->outlen = tls_out.len;
      o->errlen = tls_err.len;
      o->nbig = tls_big.len / (2 * sizeof (size_t));
      char *p = mempcpy (o->big, tls_big.data, tls_big.len);
      p = mempcpy (p, tls_out.data, tls_out.len);
      memcpy (p, tls_err.data, tls_err.len);
      e->out = o;
    }
}

static void walk_push_dir (struct walk_dir *d);

/* Read the directory named by entry PINDEX of P, as fts would when it
   reaches that entry, and record what to commit for it.  */
static void
walk_read_dir (struct walk_dir *p, size_t pindex)
{
  struct walk_ent *e = &p->ents[pindex];
  bool root = !p->parent;
  char *path = walk_ent_path (p, e);
  struct stat st;
  int fd;

  if (walk.logical)
    {
      if (!root)
        {
          if (stat (path, &st) != 0)
            {
              int err = errno;
              if (err == ENOENT && lstat (path, &st) == 0)
                goto search_as_file;
              walk_ent_stat_error (p, e, err);
              return;
            }
          if (!S_ISDIR (st.st_mode))
            goto search_as_file;
          if (skipped_file (e->name, false, true))
            return;
          if (walk_is_loop (p, st.st_dev, st.st_ino))
            {
              walk_ent_loop (p, e);
              return;
            }
        }
      fd = walk_fd_safer (open (path, WALK_DIR_FLAGS));
      if (fd < 0)
        {
          walk_ent_error (p, e, errno);
          return;
        }
    }
  else if (root)
    {
      fd = walk_fd_safer (open (path, WALK_DIR_FLAGS));
      if (fd < 0)
        {
          walk_ent_error (p, e, errno);
          return;
        }
    }
  else
    {
      int pfd = walk_dir_fd (p);
      if (pfd < 0)
        {
          walk_ent_error (p, e, errno);
          return;
        }
      if (skipped_file (e->name, false, true))
        {
          /* fts would stat it first, and grep would treat it as a file
             if that failed.  */
          if (fstatat (pfd, e->name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            walk_ent_stat_error (p, e, errno);
          return;
        }
      fd = walk_fd_safer (openat (pfd, e->name, WALK_DIR_FLAGS | O_NOFOLLOW));
      if (fd < 0)
        {
          int err = errno;
          if (fstatat (pfd, e->name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            walk_ent_stat_error (p, e, errno);
          else if (!S_ISDIR (st.st_mode))
            goto search_as_file;
          else if (walk_is_loop (p, st.st_dev, st.st_ino))
            walk_ent_loop (p, e);
          else
            walk_ent_error (p, e, err);
          return;
        }
    }

  if (fstat (fd, &st) != 0)
    {
      int err = errno;
      close (fd);
      walk_ent_error (p, e, err);
      return;
    }
  if (!root && !walk.logical && walk_is_loop (p, st.st_dev, st.st_ino))
    {
      close (fd);
      walk_ent_loop (p, e);
      return;
    }

  struct walk_dir *d = walk_dir_new (p, pindex, path, strlen (path));
  d->dev = st.st_dev;
  d->ino = st.st_ino;
  int err = walk_read_entries (fd, d);
  if (err && !d->nents)
    {
      /* FTS_DNR.  */
      close (fd);
      walk_unref (d);
      walk_ent_error (p, e, err);
      return;
    }
  if (err)
    {
      /* FTS_ERR, reported after the entries.  */
      d->post = walk_diagnostic (err, "%s", walk_shown_name (d->path));
      e->errseen = true;
    }
  walk_sort (d, fd);
  walk_classify (d, fd);
  if (walk.logical)
    close (fd);
  else
    {
      pthread_mutex_lock (&walk.fd_mu);
      walk_set_fd_locked (d, fd);
      pthread_mutex_unlock (&walk.fd_mu);
    }
  e->sub = d;
  walk_push_dir (d);
  return;

 search_as_file:
  /* The entry is no longer a directory.  Search it as grepdirent would
     search a file of that type.  */
  if (!skipped_file (e->name, false, false)
      && !S_ISLNK (st.st_mode)
      && ! (skip_devices (false) && is_device_mode (st.st_mode)))
    {
      e->kind = WALK_FILE;
      walk_search (p, walk.logical ? AT_FDCWD : walk_dir_fd (p), e, false);
    }
}

/* Queue the tasks for the entries of the directory D, which has just
   been read.  */
static void
walk_push_dir (struct walk_dir *d)
{
  /* Build the tasks before taking the lock.  */
  struct walk_task *stackv[64];
  struct walk_task **v = stackv;
  idx_t nv = 0, valloc = sizeof stackv / sizeof *stackv;
  size_t batch_lo = 0, batch_files = 0;
  for (size_t i = 0; i <= d->nents; i++)
    {
      struct walk_ent *e = i < d->nents ? &d->ents[i] : nullptr;
      bool flush = batch_files && (!e || batch_files == WALK_BATCH);
      for (int pass = 0; pass < 2; pass++)
        {
          struct walk_task *t = nullptr;
          if (pass == 0 && flush)
            {
              t = xmalloc (sizeof *t);
              *t = (struct walk_task) { .dir = d, .lo = batch_lo, .hi = i,
                                        .read = false };
              batch_files = 0;
            }
          else if (pass == 1 && e && e->kind == WALK_DIR)
            {
              t = xmalloc (sizeof *t);
              *t = (struct walk_task) { .dir = d, .lo = i, .hi = i + 1,
                                        .read = true };
            }
          if (t)
            {
              if (nv == valloc)
                {
                  if (v == stackv)
                    {
                      v = xnmalloc (valloc * 2, sizeof *v);
                      memcpy (v, stackv, sizeof stackv);
                      valloc *= 2;
                    }
                  else
                    v = xpalloc (v, &valloc, 1, -1, sizeof *v);
                }
              v[nv++] = t;
            }
        }
      if (e && e->kind == WALK_FILE)
        {
          if (!batch_files)
            batch_lo = i;
          batch_files++;
        }
    }

  atomic_fetch_add (&d->refs, nv);
  if (!walk.logical)
    {
      if (nv)
        atomic_fetch_add (&d->fd_users, nv);
      else
        {
          pthread_mutex_lock (&walk.fd_mu);
          walk_close_fd_locked (d);
          pthread_mutex_unlock (&walk.fd_mu);
        }
    }

  if (nv)
    {
      pthread_mutex_lock (&walk.mu);
      for (idx_t k = 0; k < nv; k++)
        walk_heap_push (v[k]->read ? &walk.reads : &walk.files, v[k]);
      if (nv < walk.idle)
        for (idx_t k = 0; k < nv; k++)
          pthread_cond_signal (&walk.cv);
      else if (walk.idle)
        pthread_cond_broadcast (&walk.cv);
      pthread_mutex_unlock (&walk.mu);
    }
  if (v != stackv)
    free (v);
}

/* Ask the worker threads to join traversal number GEN.  */
static void
walk_recruit (unsigned int gen)
{
  walk_recruited = true;
  ensure_pool ();
  pthread_mutex_lock (&pool_mutex);
  for (int i = 0; i < pool_nthreads; i++)
    {
      while (q_count >= WORK_QUEUE_CAP)
        pthread_cond_wait (&q_not_full_cv, &pool_mutex);
      work_queue[q_tail] = (struct work_task) { .type = TASK_TYPE_WALK,
                                                .seq = gen };
      q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
      q_count++;
    }
  pthread_cond_broadcast (&q_not_empty_cv);
  pthread_mutex_unlock (&pool_mutex);
}

/* If the current thread is the main thread and it has traversed alone
   for long enough, recruit the worker threads.  */
static void
walk_maybe_recruit (void)
{
  if (walk_is_main && !walk_recruited)
    {
      struct timespec now;
      clock_gettime (CLOCK_MONOTONIC, &now);
      if (WALK_RECRUIT_NS <= ((now.tv_sec - walk_start.tv_sec) * 1000000000
                              + now.tv_nsec - walk_start.tv_nsec))
        walk_recruit (walk.gen);
    }
}

static void walk_commit (void);

/* Search the files of task T.  */
static void
walk_search_files (struct walk_task *t)
{
  struct walk_dir *d = t->dir;
  int dfd = walk.logical ? AT_FDCWD : walk_dir_fd (d);
  int fderr = errno;
  for (size_t i = t->lo; i < t->hi; i++)
    {
      struct walk_ent *e = &d->ents[i];
      if (e->kind != WALK_FILE)
        continue;
      if (dfd < 0 && !walk.logical)
        walk_ent_error (d, e, fderr);
      else
        {
          walk_search (d, dfd, e, true);
          if (e->defer)
            {
              /* The committer will need the directory.  */
              atomic_fetch_add (&d->refs, 1);
              atomic_fetch_add (&d->fd_users, 1);
            }
        }
      atomic_store_explicit (&e->done, true, memory_order_release);

      /* With -q, a match may end the search; do not wait for the rest
         of the batch before committing it.  */
      if (exit_on_match && !e->status)
        walk_commit ();
      walk_maybe_recruit ();
    }
}

/* Run task T.  */
static void
walk_run_task (struct walk_task *t)
{
  struct walk_dir *d = t->dir;
  if (t->read)
    {
      walk_read_dir (d, t->lo);
      atomic_store_explicit (&d->ents[t->lo].done, true,
                             memory_order_release);
    }
  else
    walk_search_files (t);
  walk_fd_release (d);
  walk_unref (d);
  free (t);
}

/* Search the output file, entry E of D, now that everything before it
   has been committed.  */
static void
walk_search_deferred (struct walk_dir *d, struct walk_ent *e)
{
  bool saved_buffering = buffering_enabled;
  buffering_enabled = true;
  e->defer = false;
  walk_search (d, walk.logical ? AT_FDCWD : walk_dir_fd (d), e, false);
  buffering_enabled = saved_buffering;
  walk_fd_release (d);
  walk_unref (d);
}

/* Commit the results of entry E.  */
static void
walk_commit_ent (struct walk_ent *e)
{
  bool sep = need_group_separator (e->used, e->sep);
  if (e->used)
    global_used = true;
  struct walk_out *o = e->out;
  if (o)
    {
      char const *out = (char const *) (o->big + 2 * o->nbig);
      commit_output (out, o->outlen, out + o->outlen, o->errlen, o->big,
                     o->nbig, sep);
      walk_free_out (o);
      e->out = nullptr;
      if (line_buffered)
        fflush_errno ();
    }
  else if (sep)
    print_group_separator ();
  if (stdout_errno)
    die (EXIT_TROUBLE, stdout_errno, _("write error"));
  overall_status &= e->status;
  if (e->errseen)
    errseen = true;
  if (exit_on_match && !e->status)
    {
      stdout_errno = -1;
      exit (EXIT_SUCCESS);
    }
}

/* Commit as much as possible.  WALK.commit_mu must be held.  Return true
   if the traversal is now fully committed.  */
static bool
walk_commit_locked (void)
{
  bool saved_buffering = buffering_enabled;
  buffering_enabled = false;
  struct walk_dir *d = walk.cur;
  while (d)
    {
      if (d->cpos == d->nents)
        {
          if (d->post)
            {
              struct walk_out *o = d->post;
              commit_output (nullptr, 0, (char const *) o->big, o->errlen,
                             nullptr, 0, false);
            }
          struct walk_dir *p = d->parent;
          if (p)
            p->cpos++;
          walk.cur = p;
          walk_unref (d);
          d = p;
          continue;
        }
      struct walk_ent *e = &d->ents[d->cpos];
      if (!atomic_load_explicit (&e->done, memory_order_acquire))
        break;
      if (e->defer)
        walk_search_deferred (d, e);
      walk_commit_ent (e);
      if (e->sub)
        {
          d = walk.cur = e->sub;
          e->sub = nullptr;
          continue;
        }
      d->cpos++;
    }
  buffering_enabled = saved_buffering;
  return !d;
}

/* Commit whatever can be committed, unless another thread is doing it;
   that thread then commits on behalf of this one too.  */
static void
walk_commit (void)
{
  atomic_fetch_add (&walk.commit_req, 1);
  while (pthread_mutex_trylock (&walk.commit_mu) == 0)
    {
      unsigned long req = atomic_load (&walk.commit_req);
      bool finished = walk_commit_locked ();
      pthread_mutex_unlock (&walk.commit_mu);
      if (finished)
        {
          pthread_mutex_lock (&walk.mu);
          walk.done = true;
          pthread_cond_broadcast (&walk.cv);
          pthread_mutex_unlock (&walk.mu);
          return;
        }
      if (atomic_load (&walk.commit_req) == req)
        return;
    }
}

/* Return the next task to run, or null if the traversal is over.
   WALK.mu must be held.  */
static struct walk_task *
walk_pick (void)
{
  struct walk_task *r = walk.reads.n ? walk.reads.v[0] : nullptr;
  struct walk_task *f = walk.files.n ? walk.files.v[0] : nullptr;
  if (r && (!f || walk_task_before (r, f))
      && (walk.logical || atomic_load (&walk.open_fds) < walk.fd_cap))
    return walk_heap_pop (&walk.reads);
  if (f)
    return walk_heap_pop (&walk.files);
  if (r && walk.running == 0)
    {
      walk_relieve_fd_pressure (r->dir);
      return walk_heap_pop (&walk.reads);
    }
  return nullptr;
}

/* Take part in traversal number GEN until it is over.  */
static void
walk_participate (unsigned int gen)
{
  pthread_mutex_lock (&walk.mu);
  if (walk.gen != gen || walk.done)
    {
      pthread_mutex_unlock (&walk.mu);
      return;
    }
  walk.inside++;
  for (;;)
    {
      struct walk_task *t = walk_pick ();
      if (!t)
        {
          if (walk.done)
            break;
          walk.idle++;
          pthread_cond_wait (&walk.cv, &walk.mu);
          walk.idle--;
          continue;
        }
      walk.running++;
      pthread_mutex_unlock (&walk.mu);
      walk_run_task (t);
      walk_commit ();
      walk_maybe_recruit ();
      pthread_mutex_lock (&walk.mu);
      walk.running--;
    }
  if (--walk.inside == 0)
    pthread_cond_broadcast (&walk.left_cv);
  pthread_mutex_unlock (&walk.mu);
}

/* Return the number of directories that a traversal may keep open.  */
static int
walk_fd_cap (void)
{
  struct rlimit rl;
  long lim = 1024;
  if (getrlimit (RLIMIT_NOFILE, &rl) == 0)
    lim = rl.rlim_cur == RLIM_INFINITY ? LONG_MAX : rl.rlim_cur;
  /* Leave room for the standard streams, a file per thread and more.  */
  long cap = lim - (pool_target + 1) - 16;
#ifdef WALK_FD_CAP_MAX
  /* For testing the handling of descriptor shortages.  */
  cap = MIN (cap, WALK_FD_CAP_MAX);
#endif
  return MAX (2, MIN (cap, 4096));
}

/* Search directory DIR, a command-line operand, recursively.  */
static void
walk_run (char const *dir)
{
  /* Commit all output that precedes the traversal.  */
  producer_flush_batch ();
  pthread_mutex_lock (&pool_mutex);
  while (commit_seq < next_seq)
    {
      rob_try_commit_locked ();
      if (commit_seq >= next_seq)
        break;
      rob_done_waiters++;
      pthread_cond_wait (&rob_done_cv, &pool_mutex);
      rob_done_waiters--;
    }
  pthread_mutex_unlock (&pool_mutex);

  /* Like fts_open, trim all but one trailing slash.  */
  size_t len = strlen (dir);
  if (2 < len && dir[len - 1] == '/')
    while (1 < len && dir[len - 2] == '/')
      --len;

  struct walk_dir *holder = walk_dir_new (nullptr, 0, "", 0);
  holder->names = ximemdup0 (dir, len);
  holder->ents = xzalloc (sizeof *holder->ents);
  holder->nents = 1;
  holder->ents[0].name = holder->names;
  holder->ents[0].kind = WALK_DIR;
  holder->ents[0].status = true;
  atomic_init (&holder->refs, 2);
  atomic_init (&holder->fd_users, 1);
  struct walk_task *t = xmalloc (sizeof *t);
  *t = (struct walk_task) { .dir = holder, .lo = 0, .hi = 1, .read = true };

  pthread_mutex_lock (&walk.mu);
  walk.logical = (fts_options & FTS_LOGICAL) != 0;
  walk.fd_cap = walk_fd_cap ();
  walk.cur = holder;
  walk.done = false;
  unsigned int gen = ++walk.gen;
  walk_heap_push (&walk.reads, t);
  pthread_mutex_unlock (&walk.mu);

  /* If the worker threads are running anyway, let them join now.
     Otherwise start alone; see WALK_RECRUIT_NS.  */
  walk_recruited = false;
  if (pool_started || WALK_RECRUIT_NS <= 0)
    walk_recruit (gen);
  else
    clock_gettime (CLOCK_MONOTONIC, &walk_start);

  bool saved_buffering = buffering_enabled;
  buffering_enabled = true;
  walk_is_main = true;
  walk_participate (gen);
  walk_is_main = false;
  buffering_enabled = saved_buffering;

  pthread_mutex_lock (&walk.mu);
  while (walk.inside)
    pthread_cond_wait (&walk.left_cv, &walk.mu);
  pthread_mutex_unlock (&walk.mu);
}

static void
producer_run (char *const *files)
{
  for (char *const *f = files; *f; ++f)
    {
      if (STREQ (*f, "-"))
        {
          struct stat st;
          if (pool_target > 1 && !isatty (STDIN_FILENO)
              && fstat (STDIN_FILENO, &st) == 0
              && S_ISREG (st.st_mode)
              && st.st_size >= 2 * 1024 * 1024
              && !SAME_INODE (st, out_stat)
              && lseek (STDIN_FILENO, 0, SEEK_CUR) == 0
              && (!out_invert || (out_before <= 0 && out_after <= 0)))
            {
              chunked_grep_fd (STDIN_FILENO, false, label ? label : _("(standard input)"), &st);
            }
          else if (pool_target > 1 && !isatty (STDIN_FILENO)
                   && out_before <= 0 && out_after <= 0 && max_count == INTMAX_MAX)
            stream_slicer_stdin ();
          else
            producer_enqueue_stdin ();
        }
      else
        {
          struct stat st;
          if (stat (*f, &st) == 0)
            {
              if (skipped_file (*f, true, S_ISDIR (st.st_mode) != 0))
                continue;

              if (S_ISDIR (st.st_mode) && directories == RECURSE_DIRECTORIES)
                walk_run (*f);
              else if (S_ISREG (st.st_mode) && st.st_size >= 2 * 1024 * 1024 && pool_target > 1
                       && (!out_invert || (out_before <= 0 && out_after <= 0)))
                chunked_grep_file (*f, &st);
              else
                producer_enqueue_file (*f, true, true);
            }
          else
            producer_enqueue_file (*f, true, true);
        }
    }
  producer_flush_batch ();
}

/* Create the worker threads other than the first, then work like
   them.  Creating threads takes a while, so the first worker does it
   instead of the thread that needed the workers.  */
static void *
pool_spawner (void *arg)
{
  pthread_attr_t attr;
  pthread_attr_init (&attr);
  pthread_attr_setstacksize (&attr, 1024 * 1024);
  for (int i = 1; i < pool_nthreads; i++)
    {
      if (atomic_load (&pool_stop_spawning)
          || pthread_create (&pool_threads[i], &attr, worker_thread_func,
                             nullptr) != 0)
        break;
      atomic_store (&pool_created, i + 1);
    }
  pthread_attr_destroy (&attr);
  return worker_thread_func (arg);
}

static void
start_worker_pool (int nthreads)
{
  pool_started = true;
  pool_nthreads = nthreads;
  pool_threads = xnmalloc (pool_nthreads, sizeof *pool_threads);
  pool_terminate = false;

  pthread_attr_t attr;
  pthread_attr_init (&attr);
  pthread_attr_setstacksize (&attr, 1024 * 1024);
  if (pthread_create (&pool_threads[0], &attr, pool_spawner, nullptr) == 0)
    atomic_store (&pool_created, 1);
  else
    pool_nthreads = 0;
  pthread_attr_destroy (&attr);
}

/* Start the worker threads if they are not running yet.  */
static void
ensure_pool (void)
{
  if (!pool_started)
    start_worker_pool (pool_target);
}

static void
finish_worker_pool (void)
{
  producer_flush_batch ();
  if (!pool_started)
    return;
  pthread_mutex_lock (&pool_mutex);
  while (commit_seq < next_seq)
    {
      rob_try_commit_locked ();
      if (commit_seq >= next_seq)
        break;
      rob_done_waiters++;
      pthread_cond_wait (&rob_done_cv, &pool_mutex);
      rob_done_waiters--;
    }

  pool_terminate = true;
  pthread_cond_broadcast (&q_not_empty_cv);
  pthread_cond_broadcast (&q_not_full_cv);
  pthread_cond_broadcast (&rob_space_cv);
  pthread_cond_broadcast (&slice_avail_cv);
  pthread_mutex_unlock (&pool_mutex);

  /* Join the first worker first: once it has exited, it has created
     all the threads it will create.  */
  atomic_store (&pool_stop_spawning, true);
  if (0 < pool_nthreads)
    pthread_join (pool_threads[0], nullptr);
  int created = atomic_load (&pool_created);
  for (int i = 1; i < created; ++i)
    pthread_join (pool_threads[i], nullptr);

  cleanup_slice_freelist ();

  free (pool_threads);
  pool_threads = nullptr;
  pool_nthreads = 0;
}

int
main (int argc, char **argv)
{
  char *keys = nullptr;
  idx_t keycc = 0, keyalloc = 0;
  int matcher = -1;
  int opt;
  int prev_optind, last_recursive;
  intmax_t default_context;
  FILE *fp;
  exit_failure = EXIT_TROUBLE;
  initialize_main (&argc, &argv);

  /* Which command-line options have been specified for filename output.
     -1 for -h, 1 for -H, 0 for neither.  */
  int filename_option = 0;

  eolbyte = '\n';
  filename_mask = ~0;

  max_count = INTMAX_MAX;

  /* The value -1 means to use DEFAULT_CONTEXT. */
  out_after = out_before = -1;
  /* Default before/after context: changed by -C/-NUM options */
  default_context = -1;
  /* Changed by -o option */
  only_matching = false;

  /* Internationalization. */
#if defined HAVE_SETLOCALE
  setlocale (LC_ALL, "");
#endif
#if defined ENABLE_NLS
  bindtextdomain (PACKAGE, LOCALEDIR);
  bindtextdomain ("gnulib", GNULIB_LOCALEDIR);
  textdomain (PACKAGE);
#endif

  init_localeinfo (&localeinfo);

  atexit (clean_up_stdout);
  c_stack_action (nullptr);

  last_recursive = 0;

  pattern_table = hash_initialize (0, nullptr, hash_pattern, compare_patterns,
                                   nullptr);
  if (!pattern_table)
    xalloc_die ();

  while (prev_optind = optind,
         (opt = get_nondigit_option (argc, argv, &default_context)) != -1)
    switch (opt)
      {
      case 'A':
        context_length_arg (optarg, &out_after);
        break;

      case 'B':
        context_length_arg (optarg, &out_before);
        break;

      case 'C':
        /* Set output match context, but let any explicit leading or
           trailing amount specified with -A or -B stand. */
        context_length_arg (optarg, &default_context);
        break;

      case 'D':
        if (STREQ (optarg, "read"))
          devices = READ_DEVICES;
        else if (STREQ (optarg, "skip"))
          devices = SKIP_DEVICES;
        else
          die (EXIT_TROUBLE, 0, _("unknown devices method"));
        break;

      case 'E':
        matcher = setmatcher ("egrep", matcher);
        break;

      case 'F':
        matcher = setmatcher ("fgrep", matcher);
        break;

      case 'P':
        matcher = setmatcher ("perl", matcher);
        break;

      case 'G':
        matcher = setmatcher ("grep", matcher);
        break;

      case 'X': /* undocumented on purpose */
        matcher = setmatcher (optarg, matcher);
        break;

      case 'H':
        filename_option = 1;
        break;

      case 'I':
        binary_files = WITHOUT_MATCH_BINARY_FILES;
        break;

      case 'T':
        align_tabs = true;
        break;

      case 'U':
        if (O_BINARY)
          binary = true;
        break;

      case 'V':
        show_version = true;
        break;

      case 'a':
        binary_files = TEXT_BINARY_FILES;
        break;

      case 'b':
        out_byte = true;
        break;

      case 'c':
        count_matches = true;
        break;

      case 'd':
        directories = XARGMATCH ("--directories", optarg,
                                 directories_args, directories_types);
        if (directories == RECURSE_DIRECTORIES)
          last_recursive = prev_optind;
        break;

      case 'e':
        {
          idx_t cc = strlen (optarg);
          ptrdiff_t shortage = keycc - keyalloc + cc + 1;
          if (0 < shortage)
            pattern_array = keys = xpalloc (keys, &keyalloc, shortage, -1, 1);
          char *keyend = mempcpy (keys + keycc, optarg, cc);
          *keyend = '\n';
          keycc = update_patterns (keys, keycc, keycc + cc + 1, "");
        }
        break;

      case 'f':
        {
          if (STREQ (optarg, "-"))
            {
              if (binary)
                xset_binary_mode (STDIN_FILENO, O_BINARY);
              fp = stdin;
            }
          else
            {
              fp = fopen (optarg, binary ? "rb" : "r");
              if (!fp)
                die (EXIT_TROUBLE, errno, "%s", optarg);
            }
          idx_t newkeycc = keycc, cc;
          for (;; newkeycc += cc)
            {
              ptrdiff_t shortage = newkeycc - keyalloc + 2;
              if (0 < shortage)
                pattern_array = keys = xpalloc (keys, &keyalloc,
                                                shortage, -1, 1);
              cc = fread (keys + newkeycc, 1, keyalloc - (newkeycc + 1), fp);
              if (cc == 0)
                break;
            }
          int err = errno;
          if (!ferror (fp))
            {
              err = 0;
              if (fp == stdin)
                clearerr (fp);
              else if (fclose (fp) != 0)
                err = errno;
            }
          if (err)
            die (EXIT_TROUBLE, err, "%s", optarg);
          /* Append final newline if file ended in non-newline. */
          if (newkeycc != keycc && keys[newkeycc - 1] != '\n')
            keys[newkeycc++] = '\n';
          keycc = update_patterns (keys, keycc, newkeycc, optarg);
        }
        break;

      case 'h':
        filename_option = -1;
        break;

      case 'i':
      case 'y':			/* For old-timers . . . */
        match_icase = true;
        break;

      case NO_IGNORE_CASE_OPTION:
        match_icase = false;
        break;

      case 'L':
        /* Like -l, except list files that don't contain matches.
           Inspired by the same option in Hume's gre. */
        list_files = LISTFILES_NONMATCHING;
        break;

      case 'l':
        list_files = LISTFILES_MATCHING;
        break;

      case 'm':
        switch (xstrtoimax (optarg, nullptr, 10, &max_count, ""))
          {
          case LONGINT_OK:
          case LONGINT_OVERFLOW:
            break;

          default:
            die (EXIT_TROUBLE, 0, _("invalid max count"));
          }
        break;

      case 'n':
        out_line = true;
        break;

      case 'o':
        only_matching = true;
        break;

      case 'q':
        exit_on_match = true;
        break;

      case 'R':
        fts_options = basic_fts_options | FTS_LOGICAL;
        FALLTHROUGH;
      case 'r':
        directories = RECURSE_DIRECTORIES;
        last_recursive = prev_optind;
        break;

      case 's':
        suppress_errors = true;
        break;

      case 'v':
        out_invert = true;
        break;

      case 'w':
        wordinit ();
        match_words = true;
        break;

      case 'x':
        match_lines = true;
        break;

      case 'Z':
        filename_mask = 0;
        break;

      case 'z':
        eolbyte = '\0';
        break;

      case BINARY_FILES_OPTION:
        if (STREQ (optarg, "binary"))
          binary_files = BINARY_BINARY_FILES;
        else if (STREQ (optarg, "text"))
          binary_files = TEXT_BINARY_FILES;
        else if (STREQ (optarg, "without-match"))
          binary_files = WITHOUT_MATCH_BINARY_FILES;
        else
          die (EXIT_TROUBLE, 0, _("unknown binary-files type"));
        break;

      case COLOR_OPTION:
        if (optarg)
          {
            if (!c_strcasecmp (optarg, "always")
                || !c_strcasecmp (optarg, "yes")
                || !c_strcasecmp (optarg, "force"))
              color_option = 1;
            else if (!c_strcasecmp (optarg, "never")
                     || !c_strcasecmp (optarg, "no")
                     || !c_strcasecmp (optarg, "none"))
              color_option = 0;
            else if (!c_strcasecmp (optarg, "auto")
                     || !c_strcasecmp (optarg, "tty")
                     || !c_strcasecmp (optarg, "if-tty"))
              color_option = 2;
            else
              show_help = 1;
          }
        else
          color_option = 2;
        break;

      case EXCLUDE_OPTION:
      case INCLUDE_OPTION:
        for (int cmd = 0; cmd < 2; cmd++)
          {
            if (!excluded_patterns[cmd])
              excluded_patterns[cmd] = new_exclude ();
            add_exclude (excluded_patterns[cmd], optarg,
                         ((opt == INCLUDE_OPTION ? EXCLUDE_INCLUDE : 0)
                          | exclude_options (cmd)));
          }
        break;
      case EXCLUDE_FROM_OPTION:
        for (int cmd = 0; cmd < 2; cmd++)
          {
            if (!excluded_patterns[cmd])
              excluded_patterns[cmd] = new_exclude ();
            if (add_exclude_file (add_exclude, excluded_patterns[cmd],
                                  optarg, exclude_options (cmd), '\n')
                != 0)
              die (EXIT_TROUBLE, errno, "%s", optarg);
          }
        break;

      case EXCLUDE_DIRECTORY_OPTION:
        strip_trailing_slashes (optarg);
        for (int cmd = 0; cmd < 2; cmd++)
          {
            if (!excluded_directory_patterns[cmd])
              excluded_directory_patterns[cmd] = new_exclude ();
            add_exclude (excluded_directory_patterns[cmd], optarg,
                         exclude_options (cmd));
          }
        break;

      case GROUP_SEPARATOR_OPTION:
        group_separator = optarg;
        break;

      case LINE_BUFFERED_OPTION:
        line_buffered = true;
        break;

      case LABEL_OPTION:
        label = optarg;
        break;

      case 0:
        /* long options */
        break;

      default:
        usage (EXIT_TROUBLE);
        break;

      }

  if (show_version)
    {
      version_etc (stdout, getprogname (), PACKAGE_NAME, VERSION,
                   (char *) nullptr);
      puts (_("Written by Mike Haertel and others; see\n"
              "<https://git.savannah.gnu.org/cgit/grep.git/tree/AUTHORS>."));
#if HAVE_LIBPCRE
      Pprint_version ();
#endif
      return EXIT_SUCCESS;
    }

  if (show_help)
    usage (EXIT_SUCCESS);

  if (keys)
    {
      if (keycc == 0)
        {
          /* No keys were specified (e.g. -f /dev/null).  Match nothing.  */
          out_invert ^= true;
          match_lines = match_words = false;
          keys[keycc++] = '\n';
        }
    }
  else if (optind < argc)
    {
      /* If a command-line regular expression operand starts with '\-',
         skip the '\'.  This suppresses a stray-backslash warning if a
         script uses the non-POSIX "grep '\-x'" to avoid treating
         '-x' as an option.  */
      char const *pat = argv[optind++];
      bool skip_bs = (matcher != F_MATCHER_INDEX
                      && pat[0] == '\\' && pat[1] == '-');

      /* Make a copy so that it can be reallocated or freed later.  */
      pattern_array = keys = xstrdup (pat + skip_bs);
      idx_t patlen = strlen (keys);
      keys[patlen] = '\n';
      keycc = update_patterns (keys, 0, patlen + 1, "");
    }
  else
    usage (EXIT_TROUBLE);

  /* Strip trailing newline from keys.  */
  keycc--;

  hash_free (pattern_table);

  bool possibly_tty = false;
  struct stat tmp_stat;
  if (! exit_on_match && fstat (STDOUT_FILENO, &tmp_stat) == 0)
    {
      if (S_ISREG (tmp_stat.st_mode))
        out_stat = tmp_stat;
      else if (S_ISCHR (tmp_stat.st_mode))
        {
          struct stat null_stat;
          if (stat ("/dev/null", &null_stat) == 0
              && SAME_INODE (tmp_stat, null_stat))
            dev_null_output = true;
          else
            possibly_tty = true;
        }
    }

  /* POSIX says -c, -l and -q are mutually exclusive.  In this
     implementation, -q overrides -l and -L, which in turn override -c.  */
  if (exit_on_match | dev_null_output)
    list_files = LISTFILES_NONE;
  if ((exit_on_match | dev_null_output) || list_files != LISTFILES_NONE)
    {
      count_matches = false;
      if (max_count == INTMAX_MAX)
        done_on_match = true;
    }
  out_quiet = count_matches | done_on_match | exit_on_match;

  /* If grep might read its own output, commit output exactly; see
     EXACT_STDOUT.  Otherwise grepdesc refuses to read the output.  */
  exact_stdout = (S_ISREG (out_stat.st_mode)
                  && (out_quiet || list_files != LISTFILES_NONE
                      || max_count <= 1));

  if (out_after < 0)
    out_after = default_context;
  if (out_before < 0)
    out_before = default_context;

  /* If it is easy to see that matching cannot succeed (e.g., 'grep -f
     /dev/null'), fail without reading the input.  */
  if ((max_count == 0
       || (keycc == 0 && out_invert && !match_lines && !match_words))
      && list_files != LISTFILES_NONMATCHING)
    return EXIT_FAILURE;

  if (color_option == 2)
    color_option = possibly_tty && should_colorize () && isatty (STDOUT_FILENO);
  init_colorize ();

  if (color_option)
    {
      /* Legacy.  */
      char *userval = getenv ("GREP_COLOR");
      if (userval && *userval)
        for (char *q = userval; *q == ';' || c_isdigit (*q); q++)
          if (!q[1])
            {
              selected_match_color = context_match_color = userval;
              break;
            }

      /* New GREP_COLORS has priority.  */
      parse_grep_colors ();

      /* Warn if GREP_COLOR has an effect, since it's deprecated.  */
      if (selected_match_color == userval || context_match_color == userval)
        error (0, 0, _("warning: GREP_COLOR='%s' is deprecated;"
                       " use GREP_COLORS='mt=%s'"),
               userval, userval);
    }

  initialize_unibyte_mask ();

  if (matcher < 0)
    matcher = G_MATCHER_INDEX;

  if (matcher == F_MATCHER_INDEX
      || matcher == E_MATCHER_INDEX || matcher == G_MATCHER_INDEX)
    {
      if (match_icase)
        setup_ok_fold ();

      /* In a single-byte locale, switch from -F to -G if it is a single
         pattern that matches words, where -G is typically faster.  In a
         multibyte locale, switch if the patterns have an encoding error
         (where -F does not work) or if -i and the patterns will not work
         for -iF.  */
      if (matcher == F_MATCHER_INDEX)
        {
          if (! localeinfo.multibyte
              ? n_patterns == 1 && match_words
              : (contains_encoding_error (keys, keycc)
                 || (match_icase && !fgrep_icase_available (keys, keycc))))
            {
              fgrep_to_grep_pattern (&pattern_array, &keycc);
              keys = pattern_array;
              matcher = G_MATCHER_INDEX;
            }
        }
      /* With two or more patterns, if -F works then switch from either -E
         or -G, as -F is probably faster then.  */
      else if (1 < n_patterns)
        matcher = try_fgrep_pattern (matcher, keys, &keycc);
    }

  raw_keys = ximalloc (keycc + 2);
  memcpy (raw_keys, keys, keycc);
  raw_keys[keycc] = '\n';
  raw_keys[keycc + 1] = '\0';
  raw_keycc = keycc;
  raw_matcher_syntax = matchers[matcher].syntax;
  raw_matcher_index = matcher;
  raw_exact = only_matching | color_option;

  execute = matchers[matcher].execute;
  compiled_pattern =
    matchers[matcher].compile (keys, keycc, matchers[matcher].syntax,
                               only_matching | color_option);
  primary_compiled_pattern = compiled_pattern;
  /* We need one byte prior and one after.  */
  char eolbytes[3] = { 0, eolbyte, 0 };
  idx_t match_size;
  skip_empty_lines = (!execute (compiled_pattern, eolbytes + 1, 1,
                                &match_size, nullptr)
                      == out_invert);

  int num_operands = argc - optind;
  out_file = (filename_option == 0 && num_operands <= 1
              ? - (directories == RECURSE_DIRECTORIES)
              : 0 <= filename_option);

  if (binary)
    xset_binary_mode (STDOUT_FILENO, O_BINARY);

  /* Prefer sysconf for page size, as getpagesize typically returns int.  */
#ifdef _SC_PAGESIZE
  long psize = sysconf (_SC_PAGESIZE);
#else
  long psize = getpagesize ();
#endif
  if (! (0 < psize && psize <= (IDX_MAX - uword_size) / 2))
    abort ();
  pagesize = psize;
  if (fts_options & FTS_LOGICAL && devices == READ_COMMAND_LINE_DEVICES)
    devices = READ_DEVICES;

  char *const *files;
  if (0 < num_operands)
    {
      files = argv + optind;
    }
  else if (directories == RECURSE_DIRECTORIES && 0 < last_recursive)
    {
      static char *const cwd_only[] = { (char *) ".", nullptr };
      files = cwd_only;
      omit_dot_slash = true;
    }
  else
    {
      static char *const stdin_only[] = { (char *) "-", nullptr };
      files = stdin_only;
    }

  if (out_file < 0)
    {
      struct stat st;
      if (files[0] && stat (files[0], &st) == 0)
        out_file = !!S_ISDIR (st.st_mode);
      else
        out_file = 0;
    }

  initial_out_quiet = out_quiet;
  initial_done_on_match = done_on_match;

  int nthreads = get_num_threads ();

  /* Each thread may hold a file open, so stay well within the limit on
     open files.  */
  struct rlimit nofile;
  if (getrlimit (RLIMIT_NOFILE, &nofile) == 0
      && nofile.rlim_cur != RLIM_INFINITY)
    nthreads = MIN (nthreads, MAX (1, ((long) nofile.rlim_cur - 16) / 2));

  struct stat tmp_st;
  bool single_operand = files[0] && !files[1];
  bool is_single_stdin = single_operand && STREQ (*files, "-");
  struct stat in_st;
  bool stdin_is_reg = (is_single_stdin && fstat (STDIN_FILENO, &in_st) == 0 && S_ISREG (in_st.st_mode));
  bool stdin_is_large_reg = (stdin_is_reg
                             && in_st.st_size >= 2 * 1024 * 1024
                             && !SAME_INODE (in_st, out_stat)
                             && lseek (STDIN_FILENO, 0, SEEK_CUR) == 0
                             && (!out_invert || (out_before <= 0 && out_after <= 0)));
  bool can_slice_stdin = (!isatty (STDIN_FILENO)
                          && (!stdin_is_reg || (in_st.st_size >= 2 * 1024 * 1024 && !SAME_INODE (in_st, out_stat)))
                          && out_before <= 0 && out_after <= 0
                          && max_count == INTMAX_MAX);
  bool can_parallelize_stdin = (!isatty (STDIN_FILENO)
                                && (stdin_is_large_reg || can_slice_stdin));
  bool is_single_large_file = (single_operand
                               && !STREQ (*files, "-")
                               && stat (*files, &tmp_st) == 0
                               && S_ISREG (tmp_st.st_mode)
                               && tmp_st.st_size >= 2 * 1024 * 1024
                               && (!out_invert || (out_before <= 0 && out_after <= 0)));
  bool is_dir_recurse = (single_operand
                         && !STREQ (*files, "-")
                         && directories == RECURSE_DIRECTORIES
                         && stat (*files, &tmp_st) == 0
                         && S_ISDIR (tmp_st.st_mode));

  /* Search single-threaded if an operand is the output file that grep
     will read; directory traversals defer such a file instead.  */
  bool output_is_operand = false;
  if (exact_stdout)
    for (char *const *f = files; *f; f++)
      {
        struct stat st;
        if ((STREQ (*f, "-") ? fstat (STDIN_FILENO, &st) : stat (*f, &st)) == 0
            && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))
          output_is_operand = true;
      }

  /* A maximum count applies to each file separately, so a directory
     traversal can search files in parallel even then; the other
     parallel paths do not support it.  */
  if (nthreads <= 1
      || output_is_operand
      || (max_count < INTMAX_MAX && !is_dir_recurse)
      || (is_single_stdin && !can_parallelize_stdin)
      || (single_operand && !is_single_stdin && !is_single_large_file && !is_dir_recurse))
    {
      init_worker_buffer ();
      bool status = true;
      do
        status &= grep_command_line_arg (*files++);
      while (*files);

      return errseen ? EXIT_TROUBLE : status;
    }

  pool_target = nthreads;
  producer_run (files);
  finish_worker_pool ();

  return errseen ? EXIT_TROUBLE : overall_status;
}
