/* LD_PRELOAD shim that perturbs thread scheduling and input for tests.

   Copyright (C) 2026 David Lin <dtwlin@gmail.com>

   Copying and distribution of this file, with or without modification,
   are permitted in any medium without royalty provided the copyright
   notice and this notice are preserved.

   Each hook draws from a per-thread pseudo-random generator seeded
   from CHAOS_SEED, so that a run can be replayed, as far as the
   scheduler allows.  Probabilities are decimal fractions.

     CHAOS_SEED=N           seed (default 1)
     CHAOS_YIELD=P          after a mutex unlock, a condition signal or
                            broadcast, or a thread creation, yield the
                            processor with probability P
     CHAOS_SLEEP=P:USEC     at the same points, sleep up to USEC
                            microseconds with probability P
     CHAOS_SPURIOUS=P       make pthread_cond_wait return without having
                            been signaled with probability P, as POSIX
                            allows
     CHAOS_SHORT_READ=P     make a read from anything but a regular file
                            return a random nonempty prefix of what was
                            asked for with probability P
     CHAOS_SHORT_READ_REG=P likewise for regular files.  Local file
                            systems never do this before end of file, and
                            grep's recursive search relies on that where
                            it can (see walk_real_reg_fs), so use this
                            only for inputs that are not searched that way
     CHAOS_EINTR=P          make read fail with EINTR with probability P

   The hooks act only on calls that go through the dynamic linker, so
   the C library's internal locking and I/O are left alone.  */

#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static double p_yield, p_sleep, p_spurious, p_short_read, p_short_read_reg;
static double p_eintr;
static unsigned int sleep_usec;
static uint64_t seed = 1;

static int (*real_mutex_lock) (pthread_mutex_t *);
static int (*real_mutex_unlock) (pthread_mutex_t *);
static int (*real_cond_signal) (pthread_cond_t *);
static int (*real_cond_broadcast) (pthread_cond_t *);
static int (*real_cond_wait) (pthread_cond_t *, pthread_mutex_t *);
static int (*real_create) (pthread_t *, pthread_attr_t const *,
                           void *(*) (void *), void *);
static ssize_t (*real_read) (int, void *, size_t);

static __thread uint64_t rng;
static __thread int is_main_thread;

static double
env_prob (char const *name)
{
  char const *v = getenv (name);
  return v ? strtod (v, NULL) : 0;
}

static void
resolve (void)
{
  real_mutex_lock = dlsym (RTLD_NEXT, "pthread_mutex_lock");
  real_mutex_unlock = dlsym (RTLD_NEXT, "pthread_mutex_unlock");
  real_cond_signal = dlsym (RTLD_NEXT, "pthread_cond_signal");
  real_cond_broadcast = dlsym (RTLD_NEXT, "pthread_cond_broadcast");
  real_cond_wait = dlsym (RTLD_NEXT, "pthread_cond_wait");
  real_create = dlsym (RTLD_NEXT, "pthread_create");
  real_read = dlsym (RTLD_NEXT, "read");
}

__attribute__ ((constructor)) static void
chaos_init (void)
{
  is_main_thread = 1;
  resolve ();
  char const *s = getenv ("CHAOS_SEED");
  if (s)
    seed = strtoull (s, NULL, 10);
  p_yield = env_prob ("CHAOS_YIELD");
  p_spurious = env_prob ("CHAOS_SPURIOUS");
  p_short_read = env_prob ("CHAOS_SHORT_READ");
  p_short_read_reg = env_prob ("CHAOS_SHORT_READ_REG");
  p_eintr = env_prob ("CHAOS_EINTR");
  char const *sl = getenv ("CHAOS_SLEEP");
  if (sl)
    {
      char *end;
      p_sleep = strtod (sl, &end);
      if (*end == ':')
        sleep_usec = strtoul (end + 1, NULL, 10);
    }
  char const *pr = getenv ("CHAOS_PROBE_FILE");
  if (pr && *pr)
    {
      int fd = open (pr, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (0 <= fd)
        close (fd);
    }
}

static uint64_t
chaos_rand (void)
{
  if (!rng)
    {
#ifdef SYS_gettid
      uint64_t tid = (uint64_t) syscall (SYS_gettid);
#else
      uint64_t tid = (uint64_t) (uintptr_t) pthread_self ();
#endif
      rng = (seed * 0x9E3779B97F4A7C15ull) ^ (tid * 0xBF58476D1CE4E5B9ull) ^ 1;
    }
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

static int
chaos_hit (double p)
{
  return 0 < p && (chaos_rand () >> 11) * 0x1.0p-53 < p;
}

static void
chaos_point (void)
{
  int saved_errno = errno;
  if (chaos_hit (p_sleep) && sleep_usec)
    {
      int fd = is_main_thread ? -1 : open ("/dev/null", O_RDONLY | O_CLOEXEC);
      usleep (chaos_rand () % sleep_usec);
      if (0 <= fd)
        close (fd);
    }
  else if (chaos_hit (p_yield))
    sched_yield ();
  errno = saved_errno;
}

int
pthread_mutex_unlock (pthread_mutex_t *m)
{
  if (!real_mutex_unlock)
    resolve ();
  int r = real_mutex_unlock (m);
  chaos_point ();
  return r;
}

int
pthread_cond_signal (pthread_cond_t *c)
{
  if (!real_cond_signal)
    resolve ();
  int r = real_cond_signal (c);
  chaos_point ();
  return r;
}

int
pthread_cond_broadcast (pthread_cond_t *c)
{
  if (!real_cond_broadcast)
    resolve ();
  int r = real_cond_broadcast (c);
  chaos_point ();
  return r;
}

int
pthread_cond_wait (pthread_cond_t *c, pthread_mutex_t *m)
{
  if (!real_cond_wait)
    resolve ();
  if (chaos_hit (p_spurious))
    {
      /* A spurious wakeup: let others take the mutex, then return as if
         signaled.  Callers must check their condition again.  */
      real_mutex_unlock (m);
      sched_yield ();
      real_mutex_lock (m);
      return 0;
    }
  return real_cond_wait (c, m);
}

int
pthread_create (pthread_t *t, pthread_attr_t const *attr,
                void *(*fn) (void *), void *arg)
{
  if (!real_create)
    resolve ();
  int r = real_create (t, attr, fn, arg);
  chaos_point ();
  return r;
}

ssize_t
read (int fd, void *buf, size_t n)
{
  if (!real_read)
    resolve ();
  if (chaos_hit (p_eintr))
    {
      errno = EINTR;
      return -1;
    }
  if (1 < n && (0 < p_short_read || 0 < p_short_read_reg))
    {
      struct stat st;
      double p = (fstat (fd, &st) == 0 && S_ISREG (st.st_mode)
                  ? p_short_read_reg : p_short_read);
      if (chaos_hit (p))
        n = 1 + chaos_rand () % (n - 1);
    }
  return real_read (fd, buf, n);
}
