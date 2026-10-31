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
   along with this program.  If not, see <https://www.gnu.org/licenses/>.

   This file is #included by grep.c when grep.c is compiled with
   -DGREP_TEST_HOOKS, so it is part of grep.c's translation unit and
   shares its static functions and variables.  Do not compile it on
   its own.  grep-test-hooks.h describes the hooks.  */

/* The delays requested by GREP_DELAY_AT; see GREP_TEST_DELAY.  */
static struct { char site[32]; unsigned int usec; } test_delays[32];
static int n_test_delays;

/* Parse GREP_DELAY_AT.  Call this before starting threads.  */
static void
init_test_delays (void)
{
  char const *env = getenv ("GREP_DELAY_AT");
  if (!env)
    return;
  if (STREQ (env, "probe"))
    {
      puts ("grep-test-hooks");
      exit (EXIT_SUCCESS);
    }
  for (char const *p = env; *p && n_test_delays < 32; )
    {
      char const *colon = strchr (p, ':');
      if (!colon)
        break;
      size_t len = MIN ((size_t) (colon - p), sizeof test_delays[0].site - 1);
      memcpy (test_delays[n_test_delays].site, p, len);
      test_delays[n_test_delays].site[len] = '\0';
      char *end;
      test_delays[n_test_delays].usec = strtoul (colon + 1, &end, 10);
      n_test_delays++;
      p = *end == ',' ? end + 1 : end;
    }
}

static void
grep_test_delay (char const *site)
{
  for (int i = 0; i < n_test_delays; i++)
    if (STREQ (test_delays[i].site, "*") || STREQ (test_delays[i].site, site))
      {
        usleep (test_delays[i].usec);
        return;
      }
}

/* The names of the routes; see GREP_TEST_ROUTE.  */
static char const *const test_route_names[] =
{
#define GREP_ROUTE_NAME(id, name) name,
  GREP_ROUTES (GREP_ROUTE_NAME)
#undef GREP_ROUTE_NAME
};

/* The descriptor of the GREP_ROUTE_LOG file, or -1 if there is none.  */
static int test_route_fd = -1;

/* Open the file named by GREP_ROUTE_LOG.  Call this before starting
   threads.  */
static void
init_test_routes (void)
{
  char const *env = getenv ("GREP_ROUTE_LOG");
  if (!env || !*env)
    return;
  if (STREQ (env, "list"))
    {
      for (size_t i = 0;
           i < sizeof test_route_names / sizeof *test_route_names; i++)
        puts (test_route_names[i]);
      exit (EXIT_SUCCESS);
    }
  int fd = open (env, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
  if (fd < 0)
    die (EXIT_TROUBLE, errno, "GREP_ROUTE_LOG=%s", env);
  /* Keep descriptors 0-2 free, so that a closed standard stream stays
     closed as far as the rest of grep can tell.  */
  if (fd <= STDERR_FILENO)
    {
      int high = fcntl (fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      if (high < 0)
        die (EXIT_TROUBLE, errno, "GREP_ROUTE_LOG=%s", env);
      close (fd);
      fd = high;
    }
  test_route_fd = fd;
}

static void
grep_test_route (enum grep_route route, char const *name)
{
  if (test_route_fd < 0)
    return;
  int saved_errno = errno;
  char line[512];
  char const *r = test_route_names[route];
  size_t n = strlen (r);
  memcpy (line, r, n);
  line[n++] = '\t';
  for (char const *p = name ? name : "-"; *p && n < sizeof line - 1; p++)
    line[n++] = *p == '\t' || *p == '\n' ? '?' : *p;
  line[n++] = '\n';
  /* One write per line, so that lines from different threads do not
     interleave in the O_APPEND file.  */
  ignore_value (write (test_route_fd, line, n));
  errno = saved_errno;
}

/* The descriptor of the GREP_PREFILTER_LOG file, or -1 if none.  */
static int test_prefilter_fd = -1;

/* Open the file named by GREP_PREFILTER_LOG.  Call this before
   compiling patterns or starting threads.  */
static void
init_test_prefilter (void)
{
  char const *env = getenv ("GREP_PREFILTER_LOG");
  if (!env || !*env)
    return;
  int fd = open (env, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
  if (fd < 0)
    die (EXIT_TROUBLE, errno, "GREP_PREFILTER_LOG=%s", env);
  if (fd <= STDERR_FILENO)
    {
      int high = fcntl (fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      if (high < 0)
        die (EXIT_TROUBLE, errno, "GREP_PREFILTER_LOG=%s", env);
      close (fd);
      fd = high;
    }
  test_prefilter_fd = fd;
}

void
grep_test_prefilter (char const *kind, char const *pat, idx_t patlen,
                     char const *fmt, ...)
{
  if (test_prefilter_fd < 0)
    return;
  int saved_errno = errno;
  char tail[256];
  va_list ap;
  va_start (ap, fmt);
  int tail_len = vsnprintf (tail, sizeof tail, fmt, ap);
  va_end (ap);
  if (tail_len < 0)
    tail_len = 0;
  else if ((size_t) tail_len >= sizeof tail)
    tail_len = sizeof tail - 1;

  size_t klen = strlen (kind);
  idx_t plen = 0 < patlen ? patlen : 0;
  char *buf = ximalloc (klen + 1 + 4 * plen + 1 + tail_len + 1);
  size_t n = 0;
  memcpy (buf + n, kind, klen);
  n += klen;
  buf[n++] = '\t';
  static char const hex[] = "0123456789abcdef";
  for (idx_t i = 0; i < plen; i++)
    {
      unsigned char uc = pat[i];
      if (0x21 <= uc && uc <= 0x7e && uc != '\\')
        buf[n++] = uc;
      else
        {
          buf[n++] = '\\';
          buf[n++] = 'x';
          buf[n++] = hex[uc >> 4];
          buf[n++] = hex[uc & 0xf];
        }
    }
  buf[n++] = '\t';
  memcpy (buf + n, tail, tail_len);
  n += tail_len;
  buf[n++] = '\n';

  size_t off = 0;
  while (off < n)
    {
      ssize_t w = write (test_prefilter_fd, buf + off, n - off);
      if (w <= 0)
        break;
      off += w;
    }
  free (buf);
  errno = saved_errno;
}
