/* Test hooks of grep: delays at thread hand-off points, the route
   trace, and the prefilter extraction trace.
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

/* grep.c defines GREP_TEST_HOOKS_INTERNAL before including this file,
   and defines the hooks in grep-test-hooks.c when compiled with
   -DGREP_TEST_HOOKS (grep-hooks and grep-shrunk; see src/Makefile.am).
   dfasearch.c and pcresearch.c also include this file for
   GREP_TEST_PREFILTER.  */

#ifndef GREP_TEST_HOOKS_H
#define GREP_TEST_HOOKS_H

/* Test hooks.  A build with -DGREP_TEST_HOOKS sleeps at the named
   points where threads hand data over to each other, as requested by
   the environment variable GREP_DELAY_AT="SITE:USEC[,SITE:USEC]...",
   where SITE "*" matches every point.  This widens race windows so
   that tests can catch races deterministically.  Other builds compile
   the hooks away.  */
#if defined GREP_TEST_HOOKS && defined GREP_TEST_HOOKS_INTERNAL
static void grep_test_delay (char const *site);
# define GREP_TEST_DELAY(site) grep_test_delay (site)
#else
# define GREP_TEST_DELAY(site) ((void) 0)
#endif

/* Route trace.  Each place where grep decides how to search an operand
   calls GREP_TEST_ROUTE (ROUTE, NAME).  In a build with
   -DGREP_TEST_HOOKS, setting the environment variable
   GREP_ROUTE_LOG=FILE appends one line "ROUTE<TAB>NAME" to FILE for
   each such call, so a test can check which path searched its input.
   GREP_ROUTE_LOG=list prints every route and exits.  Lines from
   different threads may appear in any order.  NAME is the operand (or
   nullptr for none); a tab or newline in it is written as '?'.

   Other builds compile the trace away: the macro expands to a no-op
   that only checks at compile time that ROUTE names a route, and does
   not evaluate NAME.  NAME must therefore have no side effects.

   To add a route, add a line to GREP_ROUTES, call GREP_TEST_ROUTE at
   the new decision point, and add a case to tests/parallel-route-trace,
   which fails if a route has no case.

   Route                 Taken when
   serial                single-threaded search of an operand
   file-queue            a file operand goes to the worker queue
   file-chunk            a large file operand is searched in chunks
   stdin-queue           standard input goes to the worker queue
   stdin-chunk           a large regular-file stdin is searched in chunks
   stdin-slice           a pipe on stdin is cut into slices
   walk-files            16 or more file operands go to the walker
   walk-dirs             directory operands go to the recursive walker
   walk-bin-scan         -q on a large binary file in a walk, in slices
   quiet-mmap            -q on a large binary file, in one mmap scan
   chunk-quiet-probe     -q probes the first 96 KiB before chunking
   chunk-fallback        chunking gave up (NUL early, mmap failure)
   chunk-count           chunks counted (-c, -l, -L, -q)
   chunk-stateless       chunks printed without line numbers or context
   chunk-nul-sequential  pass 1 found a NUL after the first 96 KiB; the
                         file is searched sequentially instead
   chunk-nul-research    single-pass chunks found a NUL; the file is
                         searched again sequentially
   chunk-context         pass-1 matches formatted directly (context, or
                         fewer matches than the cap)
   chunk-enc-sequential  context output dropped a line with an encoding
                         error; the file is searched sequentially
   chunk-lineno          chunks searched again with line numbers (-v,
                         or too many matches)  */
#define GREP_ROUTES(X)                                  \
  X (SERIAL, "serial")                                  \
  X (FILE_QUEUE, "file-queue")                          \
  X (FILE_CHUNK, "file-chunk")                          \
  X (STDIN_QUEUE, "stdin-queue")                        \
  X (STDIN_CHUNK, "stdin-chunk")                        \
  X (STDIN_SLICE, "stdin-slice")                        \
  X (WALK_FILES, "walk-files")                          \
  X (WALK_DIRS, "walk-dirs")                            \
  X (WALK_BIN_SCAN, "walk-bin-scan")                    \
  X (QUIET_MMAP, "quiet-mmap")                          \
  X (CHUNK_QUIET_PROBE, "chunk-quiet-probe")            \
  X (CHUNK_FALLBACK, "chunk-fallback")                  \
  X (CHUNK_COUNT, "chunk-count")                        \
  X (CHUNK_STATELESS, "chunk-stateless")                \
  X (CHUNK_NUL_SEQUENTIAL, "chunk-nul-sequential")      \
  X (CHUNK_NUL_RESEARCH, "chunk-nul-research")          \
  X (CHUNK_CONTEXT, "chunk-context")                    \
  X (CHUNK_ENC_SEQUENTIAL, "chunk-enc-sequential")      \
  X (CHUNK_LINENO, "chunk-lineno")

enum grep_route
{
#define GREP_ROUTE_ENUM(id, name) GREP_ROUTE_##id,
  GREP_ROUTES (GREP_ROUTE_ENUM)
#undef GREP_ROUTE_ENUM
};

#if defined GREP_TEST_HOOKS && defined GREP_TEST_HOOKS_INTERNAL
static void grep_test_route (enum grep_route route, char const *name);
# define GREP_TEST_ROUTE(route, name) grep_test_route (GREP_ROUTE_##route, name)
#else
# define GREP_TEST_ROUTE(route, name) ((void) GREP_ROUTE_##route)
#endif

/* Prefilter extraction trace.  In a build with -DGREP_TEST_HOOKS,
   setting GREP_PREFILTER_LOG=FILE appends one line per prefilter step
   to FILE: KIND, a byte-escaped copy of PAT[0..PATLEN), and the
   formatted extraction fields.  Other builds compile it away.  */
#ifdef GREP_TEST_HOOKS
extern void grep_test_prefilter (char const *kind, char const *pat,
                                 idx_t patlen, char const *fmt, ...)
  _GL_ATTRIBUTE_FORMAT_PRINTF_STANDARD (4, 5);
# define GREP_TEST_PREFILTER(...) grep_test_prefilter (__VA_ARGS__)
#else
# define GREP_TEST_PREFILTER(...) ((void) 0)
#endif

#endif /* GREP_TEST_HOOKS_H */
