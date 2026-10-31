/* grep.c - main driver file for grep.
   Copyright (C) 1992, 1997-2002, 2004-2026 Free Software Foundation, Inc.
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
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <dirent.h>

#ifndef MAP_ANONYMOUS
# ifdef MAP_ANON
#  define MAP_ANONYMOUS MAP_ANON
# else
#  define MAP_ANONYMOUS 0
# endif
#endif

#ifndef MAP_POPULATE
# define MAP_POPULATE 0
#endif

#ifndef MAP_NORESERVE
# define MAP_NORESERVE 0
#endif

#ifndef DTTOIF
# define DTTOIF(dirtype) ((dirtype) << 12)
#endif
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
#include "sc-version.h"
#include "version-etc.h"
#include "xalloc.h"
#include "xbinary-io.h"
#include "xstrtol.h"

#include "grep-test-hooks.h"
#include "grep-tunables.h"

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
   Setting skip_clean_up_stdout before exiting can prevent clean_up_stdout
   from misbehaving on a buggy OS where 'close (STDOUT_FILENO)' fails
   with EACCES.  */
static int stdout_errno;
static _Atomic bool skip_clean_up_stdout;
static _Atomic bool write_error_dying;

struct membuf
{
  char *data;
  size_t len;
  size_t cap;
};

/* Thread-local state of the task that the current thread searches.
   Each entry declares a variable of its own.  Do not group them into
   a struct: the addresses of the output buffers escape, and GCC would
   then reload every field of the struct after each byte stored to an
   output buffer.  X (TYPE, NAME, RESET): task_reset executes RESET,
   and GREP_TLS_KEEP lets a task inherit the value.  The variables of
   GREP_TASK_TLS_SAVED are also saved and restored around a nested
   search; see struct search_tls_state.  Per-task state that task_reset
   must reset, or that a nested search must save and restore, belongs
   in one of these lists.  */
#define GREP_TLS_KEEP ((void) 0)
#define GREP_TASK_TLS_SAVED(X)                                              \
  /* The input file name, or (if standard input) null or a --label          \
     argument.  */                                                          \
  X (char const *, filename, GREP_TLS_KEEP)                                 \
  X (char const *, cached_filename_ptr, cached_filename_ptr = nullptr)      \
  X (size_t, cached_filename_len, GREP_TLS_KEEP)                            \
  /* Beginning of user-visible stuff.  */                                   \
  X (char *, bufbeg, GREP_TLS_KEEP)                                         \
  /* Limit of user-visible stuff.  */                                       \
  X (char *, buflim, GREP_TLS_KEEP)                                         \
  /* Total character count before bufbeg.  */                               \
  X (intmax_t, totalcc, totalcc = 0)                                        \
  /* Total newline count before lastnl.  */                                 \
  X (intmax_t, totalnl, GREP_TLS_KEEP)                                      \
  /* Pointer after last newline counted.  */                                \
  X (char const *, lastnl, lastnl = nullptr)                                \
  /* Pointer after last character output; null if no character has          \
     been output or if it's conceptually before bufbeg.  */                 \
  X (char *, lastout, lastout = nullptr)                                    \
  /* Maximum number of selected lines.  */                                  \
  X (intmax_t, outleft, outleft = max_count)                                \
  /* Pending lines of output.  Always kept 0 if out_quiet is true.  */      \
  X (intmax_t, pending, pending = 0)                                        \
  X (bool, buffering_enabled, GREP_TLS_KEEP)                                \
  /* Suppress all normal output.  */                                        \
  X (bool, out_quiet, out_quiet = initial_out_quiet)                        \
  /* Stop scanning file on first match.  */                                 \
  X (bool, done_on_match, done_on_match = initial_done_on_match)            \
  /* True if output from the current input file has been suppressed         \
     because an output line had an encoding error.  */                      \
  X (bool, encoding_error_output, GREP_TLS_KEEP)                            \
  /* Print width of line numbers and byte offsets.  Nonzero if              \
     ALIGN_TABS.  */                                                        \
  X (int, offset_width, GREP_TLS_KEEP)                                      \
  X (bool, file_used, file_used = false)                                    \
  X (bool, task_had_prtext, task_had_prtext = false)                        \
  /* True if the current buffered task output a line with prline.           \
     Single-threaded grep dies of an earlier write error only after         \
     outputting a line (see prline), so the committer does so only          \
     after committing the output of a task that output one.  */             \
  X (bool, task_had_prline, task_had_prline = false)                        \
  /* True if the current buffered task called prtext without                \
     OUT_QUIET before any other prtext call.  Whether a group               \
     separator precedes that output depends on what was output              \
     before the task, which only the committer knows.  */                   \
  X (bool, task_sep, task_sep = false)                                      \
  X (bool, tls_errseen, tls_errseen = false)                                \
  X (struct membuf, tls_out, membuf_reset (&tls_out))                       \
  X (struct membuf, tls_err, membuf_reset (&tls_err))                       \
  /* Writes recorded in exact mode; see BIG_WRITE_MIN.  */                  \
  X (struct membuf, tls_big, membuf_reset (&tls_big))
#define GREP_TASK_TLS_UNSAVED(X)                                            \
  /* See TLS_MAY_DEFER.  */                                                 \
  X (bool, tls_deferred, tls_deferred = false)                              \
  X (bool, walk_ent_post_grep, walk_ent_post_grep = false)                  \
  X (intmax_t, walk_ent_count, walk_ent_count = 0)

#define GREP_TLS_DECLARE(type, name, reset) static _Thread_local type name;
GREP_TASK_TLS_SAVED (GREP_TLS_DECLARE)
GREP_TASK_TLS_UNSAVED (GREP_TLS_DECLARE)
#undef GREP_TLS_DECLARE

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

/* In exact mode, worker threads must not read the output file, since
   what they would read depends on how much output has been committed.
   If TLS_MAY_DEFER, grepdesc leaves such a file alone and sets
   TLS_DEFERRED; the committer then searches the file itself, when all
   the output that precedes it has been written.  */
static _Thread_local bool tls_may_defer;

/* WALK_CUR_TASK is the batch that the current thread is searching, if
   any; see walk_split_rest.  */
struct walk_task;
static _Thread_local struct walk_task *walk_cur_task;
static void walk_split_rest (void);

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
record_stdout_errno (int err)
{
  if (err == EPIPE)
    raise (SIGPIPE);
  stdout_errno = err;
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
    record_stdout_errno (errno);
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
    record_stdout_errno (errno);
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
          note_big_write ((size_t) n);
          membuf_append (&tls_out, buf, n);
        }
      else if (n >= 0)
        {
          char *dyn = xmalloc (n + 1);
          vsnprintf (dyn, n + 1, format, ap2);
          note_big_write ((size_t) n);
          membuf_append (&tls_out, dyn, n);
          free (dyn);
        }
      va_end (ap2);
    }
  else
    {
      if (vfprintf (stdout, format, ap) < 0)
        record_stdout_errno (errno);
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
    record_stdout_errno (errno);
}

static void
fflush_errno (void)
{
  if (buffering_enabled)
    return;
  if (fflush (stdout) != 0)
    record_stdout_errno (errno);
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
/* Omit leading "./" from file names in diagnostics.  */
static bool omit_dot_slash;
static bool errseen;

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
  return (S_ISREG (st->st_mode) && 0 <= st->st_size) || S_TYPEISSHM (st) || S_TYPEISTMO (st);
}

static _Thread_local bool known_reg_nostat;

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

struct walk_dir;
struct walk_ent;
static char *walk_ent_path (struct walk_dir const *d, struct walk_ent const *e);
static char const *walk_shown_name (char const *path);
static _Thread_local struct walk_dir const *walk_cur_dir;
static _Thread_local struct walk_ent const *walk_cur_ent;
static _Thread_local bool walk_ent_defer_post;

char const *
input_filename (void)
{
  if (!filename)
    {
      if (walk_cur_dir && walk_cur_ent)
        filename = walk_shown_name (walk_ent_path (walk_cur_dir, walk_cur_ent));
      else
        filename = _("(standard input)");
    }
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
  if (! stdout_errno && ! atomic_load (&skip_clean_up_stdout))
    {
      if (fflush (stdout) != 0)
        {
          int err = errno;
          if (err == EPIPE)
            raise (SIGPIPE);
          error (0, err, _("write error"));
          _exit (EXIT_TROUBLE);
        }
      close_stdout ();
    }
}

static _Noreturn void
die_write_error (void)
{
  if (stdout_errno == EPIPE)
    raise (SIGPIPE);
  atomic_store (&skip_clean_up_stdout, true);
  if (atomic_exchange (&write_error_dying, true))
    for (;;)
      pause ();
  die (EXIT_TROUBLE, stdout_errno, _("write error"));
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

/* Skip the easy bytes in [B, LIM), and return a pointer to the first
   non-easy byte or LIM.  The easy bytes all have UNIBYTE_MASK off.  */
static char const * _GL_ATTRIBUTE_PURE
skip_easy_bytes_lim (char const *b, char const *lim)
{
  /* Search a byte at a time until the pointer is aligned.  */
  for (; (uintptr_t) b % uword_size != 0 && b < lim; b++)
    if (to_uchar (*b) & unibyte_mask)
      return b;

  /* Search a word at a time while full words remain in [b, lim).  */
  for (; lim - b >= uword_size; b += uword_size)
    {
      uword const *s = CAST_ALIGNED (uword const *, b);
      if (*s & unibyte_mask)
        break;
    }

  /* Finish byte by byte up to lim.  */
  for (; b < lim; b++)
    if (to_uchar (*b) & unibyte_mask)
      return b;

  return lim;
}

/* Return true if BUF, of size SIZE, has an encoding error.  */
static bool
buf_has_encoding_errors (char *buf, idx_t size)
{
  if (! unibyte_mask)
    return false;

  char const *lim = buf + size;
  char const *p = skip_easy_bytes_lim (buf, lim);
  if (p == lim)
    return false;

  mbstate_t mbs; mbszero (&mbs);
  ptrdiff_t clen;

  for (; p < lim; p = skip_easy_bytes_lim (p + clen, lim))
    {
      clen = imbrlen (p, lim - p, &mbs);
      if (clen < 0)
        return true;
    }

  return false;
}


/* Return true if BUF, of size SIZE, has a null byte.  */
static bool
buf_has_nulls (char const *buf, idx_t size)
{
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

struct simple_include {
  char const *suffix;
  size_t len;
};
static struct simple_include simple_includes[16];
static size_t num_simple_includes;
static bool use_simple_includes;

/* Return true if the file with NAME should be skipped.
   If COMMAND_LINE, it is a command-line argument.
   If IS_DIR, it is a directory.  */
static bool
skipped_file (char const *name, bool command_line, bool is_dir);

static inline bool
walk_file_skipped (char const *name, unsigned short namelen)
{
  if (use_simple_includes)
    {
      for (size_t i = 0; i < num_simple_includes; i++)
        {
          if (namelen >= simple_includes[i].len
              && memcmp (name + namelen - simple_includes[i].len,
                         simple_includes[i].suffix, simple_includes[i].len) == 0)
            return false;
        }
      return true;
    }
  return skipped_file (name, false, false);
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

/* Keep these in sync with the 'matchers' table.  */
enum { E_MATCHER_INDEX = 1, F_MATCHER_INDEX = 2, G_MATCHER_INDEX = 0 };
static int pool_target;

static _Thread_local char *buffer;		/* Base of buffer. */
static _Thread_local idx_t bufalloc;		/* Allocated buffer size, counting slop. */
static _Thread_local int bufdesc;		/* File descriptor. */
static idx_t pagesize;		/* alignment of memory pages */
static _Thread_local idx_t good_readsize;	/* good size to pass to 'read' */
static _Thread_local off_t bufoffset;		/* Read offset.  */
static _Thread_local off_t after_last_match;	/* Pointer after last matching line that
                                   would have been output if we were
                                   outputting characters. */
static _Thread_local bool skip_nuls;		/* Skip '\0' in data.  */
static bool skip_empty_lines;	/* Skip empty lines in data.  */
static bool patterns_have_nul;	/* True if any raw pattern contains '\0'.  */
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

#ifdef RLIMIT_AS
  struct rlimit aslim;
  bool as_limited = (getrlimit (RLIMIT_AS, &aslim) == 0
                     && aslim.rlim_cur != RLIM_INFINITY
                     && aslim.rlim_cur < (rlim_t) 4 * 1024 * 1024 * 1024);
#else
  bool as_limited = false;
#endif

  if (!as_limited)
    {
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
#ifdef MADV_NOHUGEPAGE
              /* Reads slide through the start of the buffer, so huge pages
                 would not save page faults; they would only make the first
                 read of each thread clear 2 MiB.  */
              madvise (ptr, alloc_sz, MADV_NOHUGEPAGE);
#endif
              buffer = ptr;
              bufalloc = alloc_sz;
              buffer_is_mmap = true;
              good_readsize = ALIGN_TO (GOOD_READSIZE_MIN, pagesize);
              return;
            }
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
reset (int fd, struct stat *st)
{
  init_worker_buffer ();
  bufbeg = buflim = ALIGN_TO (buffer + 1, pagesize);
  bufbeg[-1] = eolbyte;
  bufdesc = fd;
  eof_known = false;
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
fillbuf (idx_t save, struct stat *st)
{
  char *readbuf;

  /* After BUFLIM, we need room for a good-sized read plus a
     trailing uword.  Wrap within a hot L2-sized window unless a
     single long line forces a larger save.  */
  idx_t min_after_buflim = good_readsize + uword_size;
  idx_t hot_limit = 2 * good_readsize + pagesize + uword_size;
  idx_t limit = (ALIGN_TO (buffer + 1 + save, pagesize) < buflim
                 ? MIN (bufalloc, hot_limit)
                 : bufalloc);

  if (min_after_buflim <= buffer + limit - buflim)
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
          else if (fillsize >= 0 && st->st_size < 0)
            {
              if (fillsize < good_readsize)
                {
                  st->st_size = fillsize;
                  eof_known = true;
                }
              else
                {
                  if (fstat (bufdesc, st) != 0)
                    {
                      fillsize = 0;
                      cc = false;
                    }
                  if (walk_cur_task && S_ISREG (st->st_mode) && WALK_SPLIT_SIZE <= st->st_size)
                    walk_split_rest ();
                }
            }
          /* Pseudo-files may report sizes of 0 or 4096, and files may
             grow, so trust the size only if one read returned exactly
             that many bytes and was not limited by the buffer.  */
          else if (bufoffset == 0 && S_ISREG (st->st_mode) && 0 < st->st_size
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

static bool initial_done_on_match;
static bool exit_on_match;	/* Exit on first match.  */
static bool dev_null_output;	/* Stdout is known to be /dev/null.  */
static bool possibly_tty;	/* Stdout is a non-/dev/null character device.  */
static bool binary;		/* Use binary rather than text I/O.  */
static bool stdin_is_closed;	/* Standard input was closed at startup.  */

static bool global_used;

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

/* Format nonnegative VAL into the end of BUF[0..31], padded with
   leading spaces to at least WIDTH bytes, and return the start
   pointer; store the formatted length in *OUT_LEN.  */
static char *
format_intmax_width (char buf[32], intmax_t val, int width, size_t *out_len)
{
  uintmax_t v = (uintmax_t) val;
  char *p = buf + 32;
  do
    {
      *--p = '0' + (v % 10);
      v /= 10;
    }
  while (v != 0);
  int len = (int) ((buf + 32) - p);
  while (len < width && p > buf)
    {
      *--p = ' ';
      len++;
    }
  *out_len = (size_t) ((buf + 32) - p);
  return p;
}

/* Return the field width for line numbers and byte offsets if
   ALIGN_TABS is enabled, or 0 otherwise.  If ST is non-null and has a
   usable file size, bound the width by ST->st_size; otherwise use
   INTMAX_MAX.  */
static inline int
compute_offset_width (struct stat const *st)
{
  if (!align_tabs)
    return 0;
  int width = 0;
  /* Width is log of maximum number.  Line numbers are origin-1.  */
  intmax_t num = st && usable_st_size (st) ? st->st_size : INTMAX_MAX;
  num += out_line && num < INTMAX_MAX;
  do
    width++;
  while ((num /= 10) != 0);
  return width;
}

/* Print the current filename.  */
static void
print_filename (void)
{
  pr_sgr_start_if (filename_color);
  char const *fn = input_filename ();
  if (buffering_enabled)
    {
      if (fn != cached_filename_ptr)
        {
          cached_filename_ptr = fn;
          cached_filename_len = strlen (fn);
        }
      note_big_write (cached_filename_len);
      membuf_append (&tls_out, fn, cached_filename_len);
    }
  else
    fputs_errno (fn);
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
  char numbuf[32];
  size_t numlen;
  char *numstr = format_intmax_width (numbuf, pos, offset_width, &numlen);
  fwrite_errno (numstr, 1, numlen);
  pr_sgr_end_if (color);
}

/* Print the match count COUNT for the current file, preceded by the
   file name if OUT_FILE.  */
static inline void
print_file_count (intmax_t count)
{
  if (out_file)
    {
      print_filename ();
      if (filename_mask)
        print_sep (SEP_CHAR_SELECTED);
      else
        putchar_errno (0);
    }
  char numbuf[33];
  size_t numlen;
  char *numstr = format_intmax_width (numbuf, count, 0, &numlen);
  numstr[numlen] = '\n';
  fwrite_errno (numstr, 1, numlen + 1);
  if (line_buffered)
    fflush_errno ();
}

/* Print the current file name for -l or -L if STATUS (true when no
   lines matched) matches LIST_FILES.  */
static inline void
print_listed_file (bool status)
{
  if (list_files == (status ? LISTFILES_NONMATCHING : LISTFILES_MATCHING))
    {
      print_filename ();
      putchar_errno ('\n' & filename_mask);
      if (line_buffered)
        fflush_errno ();
    }
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

  if (!buffering_enabled && stdout_errno)
    die_write_error ();
  task_had_prline = true;

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
                      GREP_TEST_DELAY ("quiet_exit");
                      atomic_store (&skip_clean_up_stdout, true);
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
static bool walk_bin_scan (char const *map, size_t size, size_t scan_offset);

/* Record that a quiet match in the current file satisfied the search,
   exiting immediately when unbuffered -q is active.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE void
grep_mark_quiet_match_used (void)
{
  if (list_files == LISTFILES_NONE)
    {
      file_used = true;
      if (!buffering_enabled)
        global_used = true;
      task_had_prtext = true;
      if (exit_on_match && !buffering_enabled)
        {
          GREP_TEST_DELAY ("quiet_exit");
          atomic_store (&skip_clean_up_stdout, true);
          exit (EXIT_SUCCESS);
        }
    }
}

/* Complete a single-buffer file scan when the deferred-nulls pre-scan
   found a match at DEFER_OFF and the buffer contains no NUL bytes.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE void
grep_finish_defer_nulls (ptrdiff_t defer_off, intmax_t *nlines_inout,
                         bool *ineof_out)
{
  if (list_files == LISTFILES_MATCHING
      || (done_on_match && out_quiet && !count_matches))
    {
      grep_mark_quiet_match_used ();
      *nlines_inout = 1;
      *ineof_out = true;
      return;
    }

  lastnl = bufbeg;
  if (lastout)
    lastout = bufbeg;
  char *beg = (out_before <= 0) ? bufbeg + defer_off : bufbeg;
  char *lim = buflim;
  if (outleft)
    *nlines_inout += grepbuf (beg, lim);
  if (pending)
    prpending (lim);
  *ineof_out = true;
}

/* When scanning a large regular file in quiet/done_on_match mode (-q or
   binary match probing) where NUL zapping does not affect literal -F
   matches, mmap and scan the remainder of FD directly.  Return true if
   the mmap scan completed the file.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE bool
grep_try_fast_binary_mmap (int fd, struct stat *st, idx_t residue,
                           intmax_t *nlines_inout)
{
  if (st->st_size < 0)
    fstat (fd, st);
  if (!S_ISREG (st->st_mode) || st->st_size < 2 * 1024 * 1024)
    return false;

  if (walk_cur_task)
    walk_split_rest ();
  size_t scan_start = (size_t) bufoffset - (size_t) residue;
  char const *map = mmap (nullptr, st->st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED)
    return false;

#ifdef MADV_SEQUENTIAL
  madvise ((void *) map, st->st_size, MADV_SEQUENTIAL);
#elif defined POSIX_MADV_SEQUENTIAL
  posix_madvise ((void *) map, st->st_size, POSIX_MADV_SEQUENTIAL);
#endif
  if (walk_cur_task && st->st_size >= WALK_BIN_SCAN_MIN && pool_target > 1)
    {
      GREP_TEST_ROUTE (WALK_BIN_SCAN, input_filename ());
      if (walk_bin_scan (map, st->st_size, scan_start))
        *nlines_inout = 1;
    }
  else if (scan_start < (size_t) st->st_size)
    {
      GREP_TEST_ROUTE (QUIET_MMAP, input_filename ());
      idx_t msize = 0;
      ptrdiff_t off = execute (compiled_pattern, map + scan_start,
                               st->st_size - scan_start, &msize, nullptr);
      if (off >= 0)
        *nlines_inout = 1;
    }
  munmap ((void *) map, st->st_size);
  if (*nlines_inout > 0)
    grep_mark_quiet_match_used ();
  return true;
}

/* Search a given (non-directory) file.  Return a count of lines printed.
   Set *INEOF to true if end-of-file reached.  */
static intmax_t
grep (int fd, struct stat *st, bool *ineof)
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

  offset_width = compute_offset_width (st);

  for (bool firsttime = true; ; firsttime = false)
    {
      ptrdiff_t defer_off = -1;
      bool defer_nulls = (firsttime && eof_known && !out_invert
                          && list_files != LISTFILES_NONMATCHING
                          && raw_matcher_index == F_MATCHER_INDEX
                          && !match_lines
                          && skip_empty_lines && !patterns_have_nul
                          && bufbeg < buflim && buflim[-1] == eol
                          && binary_files != TEXT_BINARY_FILES && eol);
      if (defer_nulls)
        {
          idx_t msize = 0;
          defer_off = execute (compiled_pattern, bufbeg, buflim - bufbeg,
                               &msize, nullptr);
          if (defer_off < 0)
            {
              *ineof = true;
              goto finish_grep;
            }
        }

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

      if (defer_off >= 0 && nlines_first_null < 0)
        {
          grep_finish_defer_nulls (defer_off, &nlines, ineof);
          goto finish_grep;
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

      bool can_fast_mmap = (fd != STDIN_FILENO
                            && out_quiet && done_on_match && !out_invert
                            && !count_matches
                            && skip_empty_lines && !patterns_have_nul
                            && (nlines_first_null == 0
                                || (nlines_first_null < 0
                                    && binary_files != WITHOUT_MATCH_BINARY_FILES))
                            && raw_matcher_index == F_MATCHER_INDEX
                            && !match_lines && !match_words && eol != '\0'
                            && (!localeinfo.multibyte || localeinfo.using_utf8));
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

      if (can_fast_mmap && firsttime && !eof_known
          && grep_try_fast_binary_mmap (fd, st, residue, &nlines))
        goto finish_grep;

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
      return true;

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

  if (known_reg_nostat)
    {
      known_reg_nostat = false;
      st.st_mode = S_IFREG;
      st.st_size = -1;
      count = grep (desc, &st, &ineof);
      goto post_grep;
    }

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

  if (walk_cur_task && S_ISREG (st.st_mode) && WALK_SPLIT_SIZE <= st.st_size)
    walk_split_rest ();

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

      char *full_arg = xstrdup (input_filename ());
      walk_ent_defer_post = false;
      walk_cur_dir = nullptr;
      walk_cur_ent = nullptr;

      fts_arg[0] = full_arg;
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
      free (full_arg);
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
 post_grep:
  status = !count;
  if (walk_ent_defer_post && tls_err.len == 0
      && (count_matches || list_files != LISTFILES_NONE))
    {
      if (count_matches
          || list_files == (status ? LISTFILES_NONMATCHING : LISTFILES_MATCHING))
        {
          walk_ent_post_grep = true;
          walk_ent_count = count;
        }
      goto closeout;
    }

  if (count_matches)
    print_file_count (count);

  if (list_files == LISTFILES_NONE)
    finalize_input (desc, &st, ineof);
  else
    print_listed_file (status);

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
      if (stdin_is_closed)
        {
          suppressible_error (EBADF);
          return true;
        }
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
  if (matchers[raw_matcher_index].compile == Fcompile)
    {
      void *pat = Fclone (primary_compiled_pattern);
      if (pat)
        {
          pthread_mutex_unlock (&compile_mutex);
          return pat;
        }
    }
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
              if (localeinfo.using_utf8 && (unsigned char) i < 128
                  && (unsigned char) toupper (i) < 128
                  && (unsigned char) tolower (i) < 128
                  && toupper ('i') == 'I' && tolower ('I') == 'i')
                {
                  if ((folded[n] == 0x0131 && (i == 'i' || i == 'I'))
                      || (folded[n] == 0x017F && (i == 's' || i == 'S')))
                    continue;
                }
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
  idx_t n_alts = 0;

  while (len != 0)
    {
      switch (*q)
        {
        case '$': case '*': case '.': case '[': case '^':
          goto fail;

        case '(': case '+': case '?': case '{':
          /* There is no "case ')'" here, as "grep -E ')'" acts like
             "grep -E '\)'".  */
          if (matcher != G_MATCHER_INDEX)
            goto fail;
          break;

        case '|':
          if (matcher == E_MATCHER_INDEX)
            {
              if (p > new_keys && p[-1] != '\n'
                  && len > 1 && q[1] != '\n' && q[1] != '|')
                {
                  *p++ = '\n';
                  n_alts++;
                  q++;
                  len--;
                  continue;
                }
              goto fail;
            }
          break;

        case '\\':
          if (len <= 1)
            goto fail;
          if (matcher == G_MATCHER_INDEX && q[1] == '|')
            {
              if (p > new_keys && p[-1] != '\n'
                  && len > 2 && q[2] != '\n'
                  && !(len > 3 && q[2] == '\\' && q[3] == '|'))
                {
                  *p++ = '\n';
                  n_alts++;
                  q += 2;
                  len -= 2;
                  continue;
                }
              goto fail;
            }
          switch (q[1])
            {
            case '\\': case '^': case '$': case '*': case '.': case '[': case ']':
              q++, len--;
              break;

            case '(': case '+': case '?': case '{': case '}': case '|':
              /* Pass '\)' to GEAcompile so it can complain.  Otherwise,
                 "grep '\)'" would act like "grep ')'" while "grep '.*\)'"
                 would be an error.  */
            case ')':
              if (matcher == G_MATCHER_INDEX)
                goto fail;
              q++, len--;
              break;

            default:
              goto fail;
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

  if (matcher != F_MATCHER_INDEX && n_patterns + n_alts <= 1
      && (match_words && !localeinfo.multibyte))
    goto fail;

  if (n_alts > 0 || *len_p != p - new_keys)
    {
      *len_p = p - new_keys;
      char *keys_end = mempcpy (keys, new_keys, p - new_keys);
      *keys_end = '\n';
    }
  n_patterns += n_alts;
  result = F_MATCHER_INDEX;

 fail:
  free (new_keys);
  return result;
}

#include "parallel.c"

#include "chunksearch.c"

#include "walk.c"

/* Whether a regular file of status *ST can be searched in chunks.  */
static bool
can_chunk_reg_file (struct stat const *st)
{
  return (S_ISREG (st->st_mode) && CHUNK_FILE_MIN <= st->st_size
          && (!out_invert || (out_before < 0 && out_after < 0))
          && max_count == INTMAX_MAX);
}

/* How several threads can search standard input.  */
enum stdin_route { STDIN_ROUTE_QUEUE, STDIN_ROUTE_CHUNK, STDIN_ROUTE_SLICE };

/* Return how several threads can search standard input, and store its
   status in *ST if fstat succeeds.  Callers check POOL_TARGET.  */
static enum stdin_route
classify_stdin_route (struct stat *st)
{
  if (stdin_is_closed || isatty (STDIN_FILENO)
      || fstat (STDIN_FILENO, st) != 0)
    return STDIN_ROUTE_QUEUE;
  if (S_ISREG (st->st_mode))
    return (can_chunk_reg_file (st) && !SAME_INODE (*st, out_stat)
            && lseek (STDIN_FILENO, 0, SEEK_CUR) == 0
            && list_files == LISTFILES_NONE && !exit_on_match
            ? STDIN_ROUTE_CHUNK : STDIN_ROUTE_QUEUE);
  return (out_before < 0 && out_after < 0 && max_count == INTMAX_MAX
          && !initial_done_on_match && !line_buffered && !possibly_tty
          && binary_files != WITHOUT_MATCH_BINARY_FILES
          ? STDIN_ROUTE_SLICE : STDIN_ROUTE_QUEUE);
}

static void
producer_run (char *const *files)
{
  size_t nfiles = 0;
  bool has_stdin = false;
  for (char *const *f = files; *f; ++f)
    {
      if (STREQ (*f, "-"))
        has_stdin = true;
      nfiles++;
    }
  if (!has_stdin && nfiles >= 16 && pool_target > 1 && directories != RECURSE_DIRECTORIES)
    {
      GREP_TEST_ROUTE (WALK_FILES, files[0]);
      walk_run_cmd_files (files, nfiles);
      return;
    }

  for (char *const *f = files; *f; ++f)
    {
      if (STREQ (*f, "-"))
        {
          producer_flush_batch ();
          if (pool_started)
            rob_wait_all_committed ();
          struct stat st;
          switch (pool_target > 1 ? classify_stdin_route (&st)
                  : STDIN_ROUTE_QUEUE)
            {
            case STDIN_ROUTE_CHUNK:
              GREP_TEST_ROUTE (STDIN_CHUNK, label);
              chunked_grep_fd (STDIN_FILENO, false, label ? label : _("(standard input)"), &st);
              rob_wait_all_committed ();
              break;
            case STDIN_ROUTE_SLICE:
              GREP_TEST_ROUTE (STDIN_SLICE, label);
              stream_slicer_stdin ();
              break;
            case STDIN_ROUTE_QUEUE:
              GREP_TEST_ROUTE (STDIN_QUEUE, label);
              producer_enqueue_stdin ();
              break;
            }
        }
      else
        {
          struct stat st;
          if (stat (*f, &st) == 0
              && !skipped_file (*f, true, S_ISDIR (st.st_mode) != 0))
            {
              if (S_ISDIR (st.st_mode) && directories == RECURSE_DIRECTORIES)
                {
                  char *const *f_end = f + 1;
                  while (*f_end)
                    {
                      struct stat st_next;
                      if (stat (*f_end, &st_next) == 0 && S_ISDIR (st_next.st_mode)
                          && !skipped_file (*f_end, true, true))
                        f_end++;
                      else
                        break;
                    }
                  GREP_TEST_ROUTE (WALK_DIRS, *f);
                  walk_run_dirs (f, f_end - f);
                  f = f_end - 1;
                }
              else if (pool_target > 1 && can_chunk_reg_file (&st))
                {
                  GREP_TEST_ROUTE (FILE_CHUNK, *f);
                  chunked_grep_file (*f, &st);
                }
              else
                {
                  GREP_TEST_ROUTE (FILE_QUEUE, *f);
                  producer_enqueue_file (*f, true, true);
                }
            }
          else
            {
              GREP_TEST_ROUTE (FILE_QUEUE, *f);
              producer_enqueue_file (*f, true, true);
            }
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
      GREP_TEST_DELAY ("spawn");
      if (atomic_load (&pool_stop_spawning))
        break;
      if (pthread_create (&pool_threads[i], &attr, worker_thread_func,
                          nullptr) != 0)
        {
          pthread_mutex_lock (&pool_mutex);
          pool_nthreads = i;
          pthread_cond_broadcast (&q_not_full_cv);
          pthread_mutex_unlock (&pool_mutex);
          break;
        }
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
  /* Count the first worker before it starts: it raises the count as it
     creates the others, possibly before pthread_create returns here.  */
  atomic_store (&pool_created, 1);
  if (pthread_create (&pool_threads[0], &attr, pool_spawner, nullptr) != 0)
    {
      atomic_store (&pool_created, 0);
      pool_nthreads = 0;
    }
  pthread_attr_destroy (&attr);
  GREP_TEST_DELAY ("pool_started");
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
  if (pool_started)
    {
      pthread_mutex_lock (&pool_mutex);
      rob_wait_all_committed_locked ();

      pool_terminate = true;
      pthread_cond_broadcast (&q_not_empty_cv);
      pthread_cond_broadcast (&q_not_full_cv);
      pthread_cond_broadcast (&rob_space_cv);
      pthread_cond_broadcast (&slice_avail_cv);
      pthread_mutex_unlock (&pool_mutex);

      GREP_TEST_DELAY ("pool_join");
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

  free_compiled_pattern (primary_compiled_pattern);
  primary_compiled_pattern = nullptr;
  compiled_pattern = nullptr;
}

static void
print_version_info (void)
{
  version_etc (stdout, getprogname (), PACKAGE_NAME, VERSION,
               (char *) nullptr);
  puts (_("Written by Mike Haertel and others; see\n"
          "<https://git.savannah.gnu.org/cgit/grep.git/tree/AUTHORS>."));
#if HAVE_LIBPCRE
  Pprint_version ();
#else
  putchar ('\n');
#endif
#if defined __AVX2__
  char const *simd = "AVX2";
#else
  char const *simd = "SWAR";
#endif
  int nthreads = get_num_threads ();
  printf (_("Supercharged grep %s (up to %d %s, %s)"
            " <https://github.com/dtwlin/grep>\n"),
          SUPERCHARGED_VERSION,
          nthreads, nthreads == 1 ? "thread" : "threads", simd);
}

static void
configure_output_and_context_modes (intmax_t default_context)
{
  possibly_tty = false;
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
}

static void
setup_color_options (void)
{
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
}

static void
compile_matcher_and_patterns (int matcher, char *keys, idx_t keycc)
{
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
      /* If -F works, switch from either -E or -G when the pattern can be
         reduced to fixed strings (excluding single-pattern -w in a
         single-byte locale, where -G is faster).  */
      else if (1 < n_patterns || match_icase || !match_words
               || localeinfo.multibyte || memchr (keys, '|', keycc) != nullptr)
        matcher = try_fgrep_pattern (matcher, keys, &keycc);
    }

  raw_keys = ximalloc (keycc + 2);
  memcpy (raw_keys, keys, keycc);
  raw_keys[keycc] = '\n';
  raw_keys[keycc + 1] = '\0';
  raw_keycc = keycc;
  patterns_have_nul = memchr (raw_keys, '\0', raw_keycc) != nullptr;
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
}

static int
compute_worker_threads (char *const *files)
{
  /* Look at standard input only if it is the only operand, and at most
     once at a single other operand.  */
  bool single_operand = files[0] && !files[1];
  bool is_single_stdin = single_operand && STREQ (*files, "-");
  bool can_parallelize_stdin = false;
  if (is_single_stdin)
    {
      /* If standard input cannot even be examined (e.g., it is closed),
         let single-threaded grep diagnose it.  STDIN_IS_CLOSED is not
         set yet; a closed standard input fails fstat here.  */
      struct stat in_st;
      can_parallelize_stdin = (classify_stdin_route (&in_st)
                               != STDIN_ROUTE_QUEUE);
    }
  bool is_single_large_file = false;
  bool is_dir_recurse = false;
  struct stat tmp_st;
  if (single_operand && !is_single_stdin && stat (*files, &tmp_st) == 0
      && !skipped_file (*files, true, S_ISDIR (tmp_st.st_mode) != 0))
    {
      is_single_large_file = can_chunk_reg_file (&tmp_st);
      is_dir_recurse = (directories == RECURSE_DIRECTORIES
                        && S_ISDIR (tmp_st.st_mode));
    }

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

  /* A maximum count applies to each file separately, so files can be
     searched in parallel even then, but not in chunks.  */
  bool serial = (output_is_operand
                 || (max_count < INTMAX_MAX && single_operand
                     && !is_dir_recurse)
                 || (is_single_stdin && !can_parallelize_stdin)
                 || (single_operand && !is_single_stdin
                     && !is_single_large_file && !is_dir_recurse));

  /* Count the usable processors only if threads could help.  */
  int nthreads = 1;
  if (!serial)
    {
      nthreads = get_num_threads ();

      /* Each thread may hold a file open, so stay well within the limit
         on open files.  */
      struct rlimit nofile;
      if (getrlimit (RLIMIT_NOFILE, &nofile) == 0
          && nofile.rlim_cur != RLIM_INFINITY)
        nthreads = MIN (nthreads, MAX (1, ((long) nofile.rlim_cur - 16) / 2));

#ifdef RLIMIT_AS
      /* Each worker thread reserves a stack, a read buffer, and a
         per-thread allocator arena (typically 64 MiB on 64-bit glibc).  */
      struct rlimit aslim;
      if (getrlimit (RLIMIT_AS, &aslim) == 0
          && aslim.rlim_cur != RLIM_INFINITY)
        nthreads = MIN (nthreads,
                        MAX (1, (long) (aslim.rlim_cur / (128 * 1024 * 1024))));
#endif
    }
  return nthreads;
}

/* Append a command-line -e pattern argument ARG to *KEYS_INOUT.  */
static void
append_pattern_option_e (char **keys_inout, idx_t *keycc_inout,
                         idx_t *keyalloc_inout, char const *arg)
{
  char *keys = *keys_inout;
  idx_t keycc = *keycc_inout;
  idx_t keyalloc = *keyalloc_inout;
  idx_t cc = strlen (arg);
  ptrdiff_t shortage = keycc - keyalloc + cc + 1;
  if (0 < shortage)
    pattern_array = keys = xpalloc (keys, &keyalloc, shortage, -1, 1);
  char *keyend = mempcpy (keys + keycc, arg, cc);
  *keyend = '\n';
  *keys_inout = keys;
  *keyalloc_inout = keyalloc;
  *keycc_inout = update_patterns (keys, keycc, keycc + cc + 1, "");
}

/* Append patterns from file ARG (or stdin if "-") to *KEYS_INOUT.  */
static void
append_pattern_option_f (char **keys_inout, idx_t *keycc_inout,
                         idx_t *keyalloc_inout, char const *arg)
{
  char *keys = *keys_inout;
  idx_t keycc = *keycc_inout;
  idx_t keyalloc = *keyalloc_inout;
  FILE *fp;

  if (STREQ (arg, "-"))
    {
      if (binary)
        xset_binary_mode (STDIN_FILENO, O_BINARY);
      fp = stdin;
    }
  else
    {
      fp = fopen (arg, binary ? "rb" : "r");
      if (!fp)
        die (EXIT_TROUBLE, errno, "%s", arg);
    }

  idx_t newkeycc = keycc, cc;
  for (;; newkeycc += cc)
    {
      ptrdiff_t shortage = newkeycc - keyalloc + 2;
      if (0 < shortage)
        pattern_array = keys = xpalloc (keys, &keyalloc, shortage, -1, 1);
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
    die (EXIT_TROUBLE, err, "%s", arg);

  if (newkeycc != keycc && keys[newkeycc - 1] != '\n')
    keys[newkeycc++] = '\n';

  *keys_inout = keys;
  *keyalloc_inout = keyalloc;
  *keycc_inout = update_patterns (keys, keycc, newkeycc, arg);
}

static void
parse_color_option (char const *arg)
{
  if (!arg)
    {
      color_option = 2;
      return;
    }
  if (!c_strcasecmp (arg, "always")
      || !c_strcasecmp (arg, "yes")
      || !c_strcasecmp (arg, "force"))
    color_option = 1;
  else if (!c_strcasecmp (arg, "never")
           || !c_strcasecmp (arg, "no")
           || !c_strcasecmp (arg, "none"))
    color_option = 0;
  else if (!c_strcasecmp (arg, "auto")
           || !c_strcasecmp (arg, "tty")
           || !c_strcasecmp (arg, "if-tty"))
    color_option = 2;
  else
    show_help = 1;
}

static void
add_file_exclude_pattern (int opt, char const *arg)
{
  if (opt == EXCLUDE_OPTION)
    use_simple_includes = false;
  else if (use_simple_includes)
    {
      if (arg[0] == '*' && arg[1] == '.'
          && strpbrk (arg + 1, "*?[\\]") == nullptr
          && num_simple_includes < 16)
        {
          simple_includes[num_simple_includes++] = (struct simple_include) {
            arg + 1, strlen (arg + 1)
          };
        }
      else
        use_simple_includes = false;
    }

  for (int cmd = 0; cmd < 2; cmd++)
    {
      if (!excluded_patterns[cmd])
        excluded_patterns[cmd] = new_exclude ();
      add_exclude (excluded_patterns[cmd], arg,
                   ((opt == INCLUDE_OPTION ? EXCLUDE_INCLUDE : 0)
                    | exclude_options (cmd)));
    }
}

/* Parse command-line options from ARGC/ARGV, updating pattern buffers and
   mode flags.  */
static void
parse_grep_options (int argc, char **argv, char **keys_inout,
                    idx_t *keycc_inout, idx_t *keyalloc_inout,
                    int *matcher_inout, int *filename_option_inout,
                    int *last_recursive_inout, intmax_t *default_context_inout)
{
  int opt;
  int prev_optind;

  while (prev_optind = optind,
         (opt = get_nondigit_option (argc, argv, default_context_inout)) != -1)
    switch (opt)
      {
      case 'A':
        context_length_arg (optarg, &out_after);
        break;

      case 'B':
        context_length_arg (optarg, &out_before);
        break;

      case 'C':
        context_length_arg (optarg, default_context_inout);
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
        *matcher_inout = setmatcher ("egrep", *matcher_inout);
        break;

      case 'F':
        *matcher_inout = setmatcher ("fgrep", *matcher_inout);
        break;

      case 'P':
        *matcher_inout = setmatcher ("perl", *matcher_inout);
        break;

      case 'G':
        *matcher_inout = setmatcher ("grep", *matcher_inout);
        break;

      case 'X': /* undocumented on purpose */
        *matcher_inout = setmatcher (optarg, *matcher_inout);
        break;

      case 'H':
        *filename_option_inout = 1;
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
          *last_recursive_inout = prev_optind;
        break;

      case 'e':
        append_pattern_option_e (keys_inout, keycc_inout, keyalloc_inout,
                                 optarg);
        break;

      case 'f':
        append_pattern_option_f (keys_inout, keycc_inout, keyalloc_inout,
                                 optarg);
        break;

      case 'h':
        *filename_option_inout = -1;
        break;

      case 'i':
      case 'y':
        match_icase = true;
        break;

      case NO_IGNORE_CASE_OPTION:
        match_icase = false;
        break;

      case 'L':
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
        *last_recursive_inout = prev_optind;
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
        parse_color_option (optarg);
        break;

      case EXCLUDE_OPTION:
      case INCLUDE_OPTION:
        add_file_exclude_pattern (opt, optarg);
        break;

      case EXCLUDE_FROM_OPTION:
        use_simple_includes = false;
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

  if (num_simple_includes == 0)
    use_simple_includes = false;
}

#ifdef GREP_TEST_HOOKS
# include "grep-test-hooks.c"
#endif

int
main (int argc, char **argv)
{
  char *keys = nullptr;
  idx_t keycc = 0, keyalloc = 0;
  int matcher = -1;
  int last_recursive = 0;
  intmax_t default_context = -1;
  int filename_option = 0;

  exit_failure = EXIT_TROUBLE;
  initialize_main (&argc, &argv);

  eolbyte = '\n';
  filename_mask = ~0;
  max_count = INTMAX_MAX;
  out_after = out_before = -1;
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

  /* The mbrlen and mbrtowc replacements may cache facts about the
     locale when first called.  Call mbrlen now, so that worker threads
     do not race to fill in the cache.  */
  {
    mbstate_t mbs;
    mbszero (&mbs);
    mbrlen ("", 1, &mbs);
  }

  atexit (clean_up_stdout);
  c_stack_action (nullptr);
#ifdef GREP_TEST_HOOKS
  init_test_delays ();
  init_test_routes ();
#endif

  pattern_table = hash_initialize (0, nullptr, hash_pattern, compare_patterns,
                                   nullptr);
  if (!pattern_table)
    xalloc_die ();

  use_simple_includes = true;
  num_simple_includes = 0;

  parse_grep_options (argc, argv, &keys, &keycc, &keyalloc, &matcher,
                      &filename_option, &last_recursive, &default_context);

  if (show_version)
    {
      print_version_info ();
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

  configure_output_and_context_modes (default_context);

  /* If it is easy to see that matching cannot succeed (e.g., 'grep -f
     /dev/null'), fail without reading the input.  */
  if ((max_count == 0
       || (keycc == 0 && out_invert && !match_lines && !match_words))
      && list_files != LISTFILES_NONMATCHING)
    return EXIT_FAILURE;

  setup_color_options ();
  compile_matcher_and_patterns (matcher, keys, keycc);

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
    files = argv + optind;
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
      if (files[0]
          && (STREQ (files[0], "-")
              ? fstat (STDIN_FILENO, &st)
              : stat (files[0], &st)) == 0)
        out_file = !!S_ISDIR (st.st_mode);
      else
        out_file = 0;
    }

  initial_out_quiet = out_quiet;
  initial_done_on_match = done_on_match;

  int nthreads = compute_worker_threads (files);

  if (nthreads <= 1)
    {
      init_worker_buffer ();
      bool status = true;
      do
        {
          GREP_TEST_ROUTE (SERIAL, *files);
          status &= grep_command_line_arg (*files++);
        }
      while (*files);

      free_compiled_pattern (primary_compiled_pattern);
      primary_compiled_pattern = nullptr;
      compiled_pattern = nullptr;
      return errseen ? EXIT_TROUBLE : status;
    }

  stdin_is_closed = fcntl (STDIN_FILENO, F_GETFD) < 0 && errno == EBADF;
  if (stdin_is_closed)
    fcntl (-1, F_DUPFD_CLOEXEC, 0);
  pool_target = nthreads;
  producer_run (files);
  finish_worker_pool ();

  return errseen ? EXIT_TROUBLE : overall_status;
}
