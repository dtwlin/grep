/* The parallel tree walker of grep.
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

   This file is #included by grep.c, so it is part of grep.c's
   translation unit and shares its static functions and variables.
   Do not compile it on its own.  Moving code between fragments can
   change inlining anywhere in the program; check every move with
   benchmarks/refactor/asmdiff.py.  */

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
  intmax_t count;               /* -c count when post_grep is true */
  struct walk_out *out;         /* output to commit, or null */
  struct walk_dir *sub;         /* WALK_DIR: the directory, once read */
  unsigned short namelen;       /* length of name */
  unsigned char type;           /* d_type */
  unsigned char kind;           /* enum walk_kind */
  bool status;                  /* true if no lines were selected */
  bool errseen;                 /* an error was diagnosed */
  bool used;                    /* prtext was called */
  bool had_prline;              /* see TASK_HAD_PRLINE */
  bool sep;                     /* see TASK_SEP */
  bool defer;                   /* the output file; see TLS_MAY_DEFER */
  bool post_grep;               /* emit -c/-l/-L output in walk_commit_ent */
  atomic_bool done;             /* the entry can be committed */
};

/* A directory being traversed.  */
struct walk_dir
{
  struct walk_dir *parent;      /* null for the holder of the root */
  size_t pindex;                /* index of this directory in PARENT */
  size_t depth;                 /* 0 for the holder of the root */
  uint64_t key_lo, key_hi;      /* fts order key range */
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
  bool in_fd_list;              /* registered on walk.fd_list */
  bool real_reg_fs;             /* DT_REG entries are real regular files */
  struct walk_dir *fd_prev, *fd_next;   /* list of directories with FD */
};

/* A task: read the directory DIR->ents[LO], or search the files among
   DIR->ents[LO..HI-1].  */
struct walk_task
{
  struct walk_dir *dir;
  size_t lo, hi;
  uint64_t key;
  bool read;
};

/* A binary heap of tasks, earliest in fts order first.  */
struct walk_heap
{
  struct walk_task **v;
  idx_t n, alloc;
};

struct walk_bin_job
{
  char const *data;
  size_t size;
  atomic_size_t next_offset;
  atomic_bool found;
  atomic_int active_workers;
  size_t slice_size;
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
  atomic_size_t buffered_bytes; /* bytes held in uncommitted walk_out records */
  struct walk_bin_job *bin_job;

  bool logical;                 /* -R: use full names */
  int fd_cap;                   /* soft limit on open directories */

  pthread_mutex_t fd_mu;        /* protects the members below */
  atomic_int open_fds;          /* directories with open descriptors */
  struct walk_dir *fd_list;     /* those directories */

  pthread_mutex_t commit_mu;    /* held by the committer */
  atomic_ulong commit_req;      /* commit attempts so far */
  _Atomic (struct walk_dir *) cur; /* directory of the commit cursor */
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

/* Store into WALK_PBUF the fts_path of entry NAME (length NLEN) of directory D, and
   return it.  */
static char *
walk_path_len (struct walk_dir const *d, char const *name, size_t nlen)
{
  size_t dlen = d->pathlen;
  if (dlen && d->path[dlen - 1] == '/')
    dlen--;
  idx_t need = dlen + 1 + nlen + 1;
  if (walk_pbufsize < need)
    {
      free (walk_pbuf);
      walk_pbufsize = need;
      walk_pbuf = xpalloc (nullptr, &walk_pbufsize, need, -1, 1);
    }
  char *p = mempcpy (walk_pbuf, d->path, dlen);
  *p++ = '/';
  memcpy (p, name, nlen);
  p[nlen] = '\0';
  cached_filename_ptr = nullptr;
  return walk_pbuf;
}

/* Store into WALK_PBUF the fts_path of entry NAME of directory D, and
   return it.  */
static char *
walk_path (struct walk_dir const *d, char const *name)
{
  return walk_path_len (d, name, strlen (name));
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
  if (!d->parent)
    return (char *) e->name;
  size_t nlen = (e->namelen < 65535 && e->namelen > 0) ? e->namelen : strlen (e->name);
  return walk_path_len (d, e->name, nlen);
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
  size_t bytes = sizeof (struct errrec) + len;
  struct walk_out *o = ximalloc (sizeof *o + bytes);
  o->outlen = o->nbig = 0;
  o->errlen = bytes;
  struct errrec h = { .outpos = 0, .len = len, .after_sep = false };
  char *p = (char *) o->big;
  memcpy (p, &h, sizeof h);
  memcpy (p + sizeof h, text, len);
  free (text);
  atomic_fetch_add (&walk.buffered_bytes, bytes);
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
  if (a->key != b->key)
    return a->key < b->key;
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
  if (o)
    {
      size_t bytes = o->nbig * 2 * sizeof (size_t) + o->outlen + o->errlen;
      atomic_fetch_sub (&walk.buffered_bytes, bytes);
      free (o);
    }
}

static void
walk_unref (struct walk_dir *d)
{
  GREP_TEST_DELAY ("walk_unref");
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
  d->in_fd_list = true;
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
  if (d->in_fd_list)
    {
      atomic_fetch_sub (&walk.open_fds, 1);
      if (d->fd_prev)
        d->fd_prev->fd_next = d->fd_next;
      else if (walk.fd_list == d)
        walk.fd_list = d->fd_next;
      if (d->fd_next)
        d->fd_next->fd_prev = d->fd_prev;
      d->fd_prev = nullptr;
      d->fd_next = nullptr;
      d->in_fd_list = false;
    }
}

/* A task that needed the descriptor of D is done with it.  */
static void
walk_fd_release (struct walk_dir *d)
{
  GREP_TEST_DELAY ("walk_fd_release");
  if (!walk.logical && atomic_fetch_sub (&d->fd_users, 1) == 1)
    {
      pthread_mutex_lock (&walk.fd_mu);
      if (!d->in_fd_list)
        {
          int fd = atomic_load_explicit (&d->fd, memory_order_relaxed);
          if (0 <= fd)
            close (fd);
          atomic_store_explicit (&d->fd, -1, memory_order_relaxed);
        }
      else
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
  if (!parent)
    {
      d->key_lo = 0;
      d->key_hi = UINT64_MAX;
    }
  else
    {
      uint64_t pspan = (parent->key_hi - parent->key_lo) / (parent->nents + 1);
      d->key_lo = parent->key_lo + pindex * pspan + (pspan > 0);
      d->key_hi = parent->key_lo + (pindex + 1) * pspan;
    }
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
  e->namelen = (unsigned short) MIN (len - 1, 65535);
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
# if defined _DIRENT_HAVE_D_TYPE || defined DT_UNKNOWN
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

/* Return true if DT_REG entries in the directory open on FD are real
   regular files whose first read either fills the buffer or reaches EOF
   (unlike /proc or /sys seq_files).  */
static bool
walk_real_reg_fs (int fd)
{
#if defined __linux__ && HAVE_SYS_VFS_H && HAVE_FSTATFS
  struct statfs fs;
  if (fstatfs (fd, &fs) == 0)
    switch ((unsigned long) fs.f_type)
      {
      case 0xEF53UL:            /* ext2/ext3/ext4 */
      case 0x01021994UL:        /* tmpfs */
      case 0x58465342UL:        /* XFS */
      case 0x9123683EUL:        /* Btrfs */
      case 0x858458F6UL:        /* ramfs */
      case 0xF2F52010UL:        /* F2FS */
      case 0x3153464AUL:        /* JFS */
      case 0x52654973UL:        /* ReiserFS */
      case 0x2FC12FC1UL:        /* ZFS */
      case 0xCA451A4EUL:        /* bcachefs */
        return true;
      }
#else
  (void) fd;
#endif
  return false;
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
  else if (walk_file_skipped (e->name, e->namelen) || S_ISLNK (st->st_mode)
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
          e->kind = (walk_file_skipped (e->name, e->namelen)
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
                  e->kind = (walk_file_skipped (e->name, e->namelen)
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
  task_reset ();
  tls_may_defer = may_defer && exact_stdout;

  char const *search_name;
  if (!d->parent)
    {
      filename = walk_shown_name (e->name);
      walk_cur_dir = nullptr;
      walk_cur_ent = nullptr;
      search_name = e->name;
    }
  else if (walk.logical)
    {
      char *path = walk_ent_path (d, e);
      filename = walk_shown_name (path);
      walk_cur_dir = nullptr;
      walk_cur_ent = nullptr;
      search_name = path;
    }
  else
    {
      filename = nullptr;
      walk_cur_dir = d;
      walk_cur_ent = e;
      search_name = e->name;
    }

  known_reg_nostat = (d->parent != nullptr && !walk.logical && e->type == DT_REG
                      && d->real_reg_fs && !align_tabs
                      && !(S_ISREG (out_stat.st_mode)
                           && (e->ino == 0 || e->ino == out_stat.st_ino)));
  walk_ent_defer_post = true;
  e->status = grepfile (dfd, search_name,
                        walk.logical || !d->parent, !d->parent);
  walk_ent_defer_post = false;
  e->post_grep = walk_ent_post_grep;
  e->count = walk_ent_count;
  known_reg_nostat = false;
  walk_cur_dir = nullptr;
  walk_cur_ent = nullptr;
  tls_may_defer = false;
  e->errseen = tls_errseen;
  e->used = task_had_prtext;
  e->had_prline = task_had_prline;
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
      /* A buffer that has never been used may be null; do not pass it
         to memcpy even with a zero length.  */
      char *p = (char *) o->big;
      if (tls_big.len)
        p = mempcpy (p, tls_big.data, tls_big.len);
      if (tls_out.len)
        p = mempcpy (p, tls_out.data, tls_out.len);
      if (tls_err.len)
        memcpy (p, tls_err.data, tls_err.len);
      atomic_fetch_add (&walk.buffered_bytes, tls_big.len + tls_out.len + tls_err.len);
      e->out = o;
    }
}

static struct walk_task * walk_push_dir (struct walk_dir *d, int fd);
static void walk_search_files (struct walk_task *t);
static void walk_commit (void);

/* Open directory entry E of parent directory P (with full path PATH),
   handling logical/physical symlink rules, exclusion checks, and directory
   loop detection.  Store the directory's stat in *ST_OUT and return its
   file descriptor, or -1 if the entry was skipped, failed, or searched
   as a non-directory file.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE int
walk_open_dir_entry (struct walk_dir *p, struct walk_ent *e, char const *path,
                     struct stat *st_out)
{
  bool root = !p->parent;
  int fd;

  if (walk.logical)
    {
      if (!root)
        {
          if (stat (path, st_out) != 0)
            {
              int err = errno;
              if (err == ENOENT && lstat (path, st_out) == 0)
                goto search_as_file;
              walk_ent_stat_error (p, e, err);
              return -1;
            }
          if (!S_ISDIR (st_out->st_mode))
            goto search_as_file;
          if (skipped_file (e->name, false, true))
            return -1;
          if (walk_is_loop (p, st_out->st_dev, st_out->st_ino))
            {
              walk_ent_loop (p, e);
              return -1;
            }
        }
      fd = walk_fd_safer (open (path, WALK_DIR_FLAGS));
      if (fd < 0)
        {
          walk_ent_error (p, e, errno);
          return -1;
        }
    }
  else if (root)
    {
      fd = walk_fd_safer (open (path, WALK_DIR_FLAGS));
      if (fd < 0)
        {
          walk_ent_error (p, e, errno);
          return -1;
        }
    }
  else
    {
      int pfd = walk_dir_fd (p);
      if (pfd < 0)
        {
          walk_ent_error (p, e, errno);
          return -1;
        }
      if (skipped_file (e->name, false, true))
        {
          if (fstatat (pfd, e->name, st_out, AT_SYMLINK_NOFOLLOW) != 0)
            walk_ent_stat_error (p, e, errno);
          return -1;
        }
      fd = walk_fd_safer (openat (pfd, e->name, WALK_DIR_FLAGS | O_NOFOLLOW));
      if (fd < 0)
        {
          int err = errno;
          if (fstatat (pfd, e->name, st_out, AT_SYMLINK_NOFOLLOW) != 0)
            walk_ent_stat_error (p, e, errno);
          else if (!S_ISDIR (st_out->st_mode))
            goto search_as_file;
          else if (walk_is_loop (p, st_out->st_dev, st_out->st_ino))
            walk_ent_loop (p, e);
          else
            walk_ent_error (p, e, err);
          return -1;
        }
    }

  if (fstat (fd, st_out) != 0)
    {
      int err = errno;
      close (fd);
      walk_ent_error (p, e, err);
      return -1;
    }
  if (!root && !walk.logical && walk_is_loop (p, st_out->st_dev, st_out->st_ino))
    {
      close (fd);
      walk_ent_loop (p, e);
      return -1;
    }
  return fd;

 search_as_file:
  if (!skipped_file (e->name, false, false)
      && !S_ISLNK (st_out->st_mode)
      && !(skip_devices (false) && is_device_mode (st_out->st_mode)))
    {
      e->kind = WALK_FILE;
      int pfd = walk.logical ? AT_FDCWD : walk_dir_fd (p);
      if (pfd < 0)
        walk_ent_error (p, e, errno);
      else
        walk_search (p, pfd, e, true);
    }
  return -1;
}

/* Read the directory named by entry PINDEX of P, as fts would when it
   reaches that entry, and record what to commit for it.  */
static struct walk_task *
walk_read_dir (struct walk_dir *p, size_t pindex)
{
  struct walk_ent *e = &p->ents[pindex];
  bool root = !p->parent;
  char *path = walk_ent_path (p, e);
  struct stat st;
  int fd = walk_open_dir_entry (p, e, path, &st);
  if (fd < 0)
    return nullptr;

  size_t plen = strlen (path);
  if (root && 2 < plen && path[plen - 1] == '/')
    while (1 < plen && path[plen - 2] == '/')
      --plen;

  struct walk_dir *d = walk_dir_new (p, pindex, path, plen);
  d->dev = st.st_dev;
  d->ino = st.st_ino;
  d->real_reg_fs = (!root && st.st_dev == p->dev
                    ? p->real_reg_fs
                    : walk_real_reg_fs (fd));
  int err = walk_read_entries (fd, d);
  if (err && !d->nents)
    {
      /* FTS_DNR.  */
      close (fd);
      walk_unref (d);
      walk_ent_error (p, e, err);
      return nullptr;
    }
  if (err)
    {
      /* FTS_ERR, reported after the entries.  */
      d->post = walk_diagnostic (err, "%s", walk_shown_name (d->path));
      e->errseen = true;
    }
  walk_sort (d, fd);
  walk_classify (d, fd);
  e->sub = d;
  return walk_push_dir (d, fd);
}

/* Queue the tasks for the entries of the directory D, which has just
   been read.  */
static struct walk_task *
walk_push_dir (struct walk_dir *d, int fd)
{
  /* Build the tasks before taking the lock.  */
  struct walk_task *stackv[64];
  struct walk_task **v = stackv;
  idx_t nv = 0, valloc = sizeof stackv / sizeof *stackv;
  size_t batch_lo = 0, batch_files = 0;
  uint64_t span = (d->key_hi - d->key_lo) / (d->nents + 1);
  for (size_t i = 0; i <= d->nents; i++)
    {
      struct walk_ent *e = i < d->nents ? &d->ents[i] : nullptr;
      bool flush = batch_files && (!e || batch_files == WALK_BATCH
                                   || (e->kind == WALK_DIR && nv == 0));
      for (int pass = 0; pass < 2; pass++)
        {
          struct walk_task *t = nullptr;
          if (pass == 0 && flush)
            {
              t = xmalloc (sizeof *t);
              *t = (struct walk_task) { .dir = d, .lo = batch_lo, .hi = i,
                                        .key = d->key_lo + batch_lo * span,
                                        .read = false };
              batch_files = 0;
            }
          else if (pass == 1 && e && e->kind == WALK_DIR)
            {
              t = xmalloc (sizeof *t);
              *t = (struct walk_task) { .dir = d, .lo = i, .hi = i + 1,
                                        .key = d->key_lo + i * span,
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

  struct walk_task *next = nullptr;
  if (nv > 0)
    {
      if (!v[0]->read || walk.logical || atomic_load (&walk.open_fds) < walk.fd_cap)
        next = v[0];
    }
  atomic_fetch_add (&d->refs, nv);
  if (walk.logical || nv == 0)
    {
      if (0 <= fd)
        close (fd);
    }
  else if (nv == 1 && next != nullptr)
    {
      atomic_store_explicit (&d->fd, fd, memory_order_relaxed);
      atomic_store_explicit (&d->fd_users, 1, memory_order_relaxed);
    }
  else
    {
      atomic_fetch_add (&d->fd_users, nv);
      pthread_mutex_lock (&walk.fd_mu);
      walk_set_fd_locked (d, fd);
      pthread_mutex_unlock (&walk.fd_mu);
    }

  idx_t start_k = (next != nullptr) ? 1 : 0;
  if (start_k < nv)
    {
      pthread_mutex_lock (&walk.mu);
      for (idx_t k = start_k; k < nv; k++)
        walk_heap_push (v[k]->read ? &walk.reads : &walk.files, v[k]);
      idx_t pushed = nv - start_k;
      if (pushed < walk.idle)
        for (idx_t k = 0; k < pushed; k++)
          pthread_cond_signal (&walk.cv);
      else if (walk.idle)
        pthread_cond_broadcast (&walk.cv);
      pthread_mutex_unlock (&walk.mu);
    }
  if (v != stackv)
    free (v);

  return next;
}

/* Ask the worker threads to join traversal number GEN.  */
static void
walk_recruit (unsigned int gen)
{
  walk_recruited = true;
  ensure_pool ();
  GREP_TEST_DELAY ("walk_recruit");
  pthread_mutex_lock (&pool_mutex);
  for (int i = 0; i < pool_nthreads; i++)
    {
      /* Wake a worker for each task now, not after the loop: if the
         queue fills up, only workers that take tasks make room.  */
      while (q_count >= WORK_QUEUE_CAP && i < pool_nthreads && !pool_terminate)
        pthread_cond_wait (&q_not_full_cv, &pool_mutex);
      if (i >= pool_nthreads || pool_terminate)
        break;
      work_queue[q_tail] = (struct work_task) { .type = TASK_TYPE_WALK,
                                                .seq = gen };
      q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
      q_count++;
      pthread_cond_signal (&q_not_empty_cv);
    }
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

/* The entry of WALK_CUR_TASK whose file the current thread searches.  */
static _Thread_local size_t walk_cur_index;

/* Queue entries LO..T->hi-1 of the batch T as a new task, and keep the
   entries before LO in T.  */
static void
walk_split (struct walk_task *t, size_t lo)
{
  if (walk_is_main && !walk_recruited)
    walk_recruit (walk.gen);
  struct walk_dir *d = t->dir;
  struct walk_task *n = xmalloc (sizeof *n);
  uint64_t span = (d->key_hi - d->key_lo) / (d->nents + 1);
  *n = (struct walk_task) { .dir = d, .lo = lo, .hi = t->hi,
                            .key = d->key_lo + lo * span,
                            .read = false };
  t->hi = lo;

  /* T keeps its own reference and descriptor use until it ends, so
     neither count can drop to zero before N takes its own.  */
  atomic_fetch_add (&d->refs, 1);
  if (!walk.logical && d->parent)
    {
      atomic_fetch_add (&d->fd_users, 1);
      pthread_mutex_lock (&walk.fd_mu);
      if (!d->in_fd_list)
        {
          int cur_fd = atomic_load_explicit (&d->fd, memory_order_relaxed);
          if (0 <= cur_fd)
            walk_set_fd_locked (d, cur_fd);
        }
      pthread_mutex_unlock (&walk.fd_mu);
    }

  GREP_TEST_DELAY ("walk_split");
  pthread_mutex_lock (&walk.mu);
  walk_heap_push (&walk.files, n);
  if (walk.idle)
    pthread_cond_signal (&walk.cv);
  pthread_mutex_unlock (&walk.mu);
}

/* The current thread is about to search a large file of its batch; let
   other threads search the files after it meanwhile.  */
static void
walk_split_rest (void)
{
  struct walk_task *t = walk_cur_task;
  for (size_t i = walk_cur_index + 1; i < t->hi; i++)
    if (t->dir->ents[i].kind == WALK_FILE)
      {
        walk_split (t, walk_cur_index + 1);
        break;
      }
}

/* Search the files of task T.  */
static void
walk_search_files (struct walk_task *t)
{
  struct walk_dir *d = t->dir;
  int dfd = (walk.logical || !d->parent) ? AT_FDCWD : walk_dir_fd (d);
  int fderr = errno;
  struct timespec start, prev;
  clock_gettime (CLOCK_MONOTONIC, &start);
  prev = start;
  for (size_t i = t->lo; i < t->hi; i++)
    {
      struct walk_ent *e = &d->ents[i];
      if (e->kind != WALK_FILE)
        continue;
      if (dfd < 0 && !walk.logical && d->parent)
        walk_ent_error (d, e, fderr);
      else
        {
          walk_cur_task = t;
          walk_cur_index = i;
          walk_search (d, dfd, e, true);
          walk_cur_task = nullptr;
          if (e->defer)
            {
              /* The committer will need the directory.  */
              atomic_fetch_add (&d->refs, 1);
              if (d->parent)
                {
                  atomic_fetch_add (&d->fd_users, 1);
                  if (!walk.logical)
                    {
                      pthread_mutex_lock (&walk.fd_mu);
                      if (!d->in_fd_list)
                        {
                          int cur_fd = atomic_load_explicit (&d->fd, memory_order_relaxed);
                          if (0 <= cur_fd)
                            walk_set_fd_locked (d, cur_fd);
                        }
                      pthread_mutex_unlock (&walk.fd_mu);
                    }
                }
            }
        }
      atomic_store_explicit (&e->done, true, memory_order_release);

      /* With -q, a match may end the search; do not wait for the rest
         of the batch before committing it.  */
      if (exit_on_match && !e->status)
        walk_commit ();
      walk_maybe_recruit ();

      if (((i - t->lo) & 3) == 3 && WALK_SPLIT_MIN_REM <= t->hi - (i + 1))
        {
          struct timespec now;
          clock_gettime (CLOCK_MONOTONIC, &now);
          intmax_t file_ns = ((now.tv_sec - prev.tv_sec) * 1000000000
                              + now.tv_nsec - prev.tv_nsec);
          intmax_t batch_ns = ((now.tv_sec - start.tv_sec) * 1000000000
                               + now.tv_nsec - start.tv_nsec);
          prev = now;
          if (WALK_SPLIT_FILE_NS <= file_ns
              || WALK_SPLIT_NS <= batch_ns)
            {
              walk_split (t, i + 1 + (t->hi - i) / 2);
              start = now;
            }
        }
    }
}

/* Run task T.  */
static bool
walk_run_task (struct walk_task *t)
{
  bool any_commit = false;
  while (t)
    {
      struct walk_dir *d = t->dir;
      struct walk_task *next = nullptr;
      if (t->read)
        {
          next = walk_read_dir (d, t->lo);
          atomic_store_explicit (&d->ents[t->lo].done, true,
                                 memory_order_release);
        }
      else
        {
          walk_search_files (t);
        }
      bool should_commit = (d == atomic_load_explicit (&walk.cur, memory_order_relaxed));
      if (should_commit)
        {
          walk_commit ();
          any_commit = true;
        }
      if (d->parent)
        walk_fd_release (d);
      walk_unref (d);
      free (t);
      t = next;
    }
  return any_commit;
}

/* Search the output file, entry E of D, now that everything before it
   has been committed.  */
static void
walk_search_deferred (struct walk_dir *d, struct walk_ent *e)
{
  bool saved_buffering = buffering_enabled;
  buffering_enabled = true;
  e->defer = false;
  int dfd = (walk.logical || !d->parent) ? AT_FDCWD : walk_dir_fd (d);
  if (dfd < 0 && !walk.logical && d->parent)
    walk_ent_error (d, e, errno);
  else
    walk_search (d, dfd, e, false);
  buffering_enabled = saved_buffering;
  if (d->parent)
    walk_fd_release (d);
  walk_unref (d);
}

/* Commit the results of entry E of directory D.  */
static void
walk_commit_ent (struct walk_dir *d, struct walk_ent *e)
{
  bool sep = need_group_separator (e->used, e->sep);
  if (e->used)
    global_used = true;
  if (e->post_grep)
    {
      filename = nullptr;
      walk_cur_dir = d;
      walk_cur_ent = e;
      if (count_matches)
        print_file_count (e->count);
      print_listed_file (e->status);
      filename = nullptr;
      walk_cur_dir = nullptr;
      walk_cur_ent = nullptr;
    }
  struct walk_out *o = e->out;
  if (o)
    {
      char const *out = (char const *) (o->big + 2 * o->nbig);
      commit_output (out, o->outlen, out + o->outlen, o->errlen, o->big,
                     o->nbig, sep, e->had_prline);
      walk_free_out (o);
      e->out = nullptr;
      if (line_buffered)
        fflush_errno ();
    }
  else if (sep)
    print_group_separator ();
  if (stdout_errno && e->had_prline)
    die_write_error ();
  overall_status &= e->status;
  if (e->errseen)
    errseen = true;
  if (exit_on_match && !e->status)
    {
      GREP_TEST_DELAY ("quiet_exit");
      atomic_store (&skip_clean_up_stdout, true);
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
  size_t before_bytes = atomic_load (&walk.buffered_bytes);
  struct walk_dir *d = atomic_load_explicit (&walk.cur, memory_order_acquire);
  while (d)
    {
      if (d->cpos == d->nents)
        {
          if (d->post)
            {
              struct walk_out *o = d->post;
              commit_output (nullptr, 0, (char const *) o->big, o->errlen,
                             nullptr, 0, false, false);
            }
          struct walk_dir *p = d->parent;
          if (p)
            p->cpos++;
          atomic_store_explicit (&walk.cur, p, memory_order_release);
          walk_unref (d);
          d = p;
          continue;
        }
      struct walk_ent *e = &d->ents[d->cpos];
      if (!atomic_load_explicit (&e->done, memory_order_acquire))
        break;
      if (e->defer)
        walk_search_deferred (d, e);
      walk_commit_ent (d, e);
      if (e->sub)
        {
          d = e->sub;
          atomic_store_explicit (&walk.cur, d, memory_order_release);
          e->sub = nullptr;
          continue;
        }
      d->cpos++;
    }
  buffering_enabled = saved_buffering;
  if (WALK_BUF_CAP <= before_bytes
      && atomic_load (&walk.buffered_bytes) < WALK_BUF_CAP)
    {
      pthread_mutex_lock (&walk.mu);
      if (walk.idle)
        pthread_cond_broadcast (&walk.cv);
      pthread_mutex_unlock (&walk.mu);
    }
  return !d;
}

/* Commit whatever can be committed, unless another thread is doing it;
   that thread then commits on behalf of this one too.  */
static void
walk_commit (void)
{
  GREP_TEST_DELAY ("walk_commit");
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
  if (WALK_BUF_CAP <= atomic_load (&walk.buffered_bytes) && 0 < walk.running)
    return nullptr;
  struct walk_task *r = walk.reads.n ? walk.reads.v[0] : nullptr;
  struct walk_task *f = walk.files.n ? walk.files.v[0] : nullptr;
  if (r && (!f || walk_task_before (r, f)
            || (walk.files.n < 4 * pool_target
                && atomic_load (&walk.buffered_bytes) < WALK_BUF_CAP / 2))
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

static void
walk_try_publish_bin_job (struct walk_bin_job *job, bool *published)
{
  pthread_mutex_lock (&walk.mu);
  if (walk.bin_job == nullptr)
    {
      walk.bin_job = job;
      *published = true;
      if (walk.idle)
        pthread_cond_broadcast (&walk.cv);
    }
  pthread_mutex_unlock (&walk.mu);
}

static void
walk_help_bin_job (struct walk_bin_job *job, bool *published)
{
  size_t slice = job->slice_size;
  size_t overlap = MAX (65536, (size_t) raw_keycc * 2 + 1);
  while (!atomic_load_explicit (&job->found, memory_order_relaxed))
    {
      if (published && !*published)
        walk_try_publish_bin_job (job, published);
      size_t off = atomic_fetch_add (&job->next_offset, slice);
      if (off >= job->size)
        break;
      size_t len = MIN (slice + overlap, job->size - off);
      idx_t msize = 0;
      ptrdiff_t res = execute (compiled_pattern, job->data + off, len, &msize, nullptr);
      if (res >= 0)
        {
          atomic_store_explicit (&job->found, true, memory_order_release);
          break;
        }
    }
}

static bool
walk_bin_scan (char const *map, size_t size, size_t scan_offset)
{
  if (walk_is_main && !walk_recruited)
    walk_recruit (walk.gen);
  struct walk_bin_job job;
  job.data = map;
  job.size = size;
  atomic_init (&job.next_offset, scan_offset);
  atomic_init (&job.found, false);
  atomic_init (&job.active_workers, 1);
  job.slice_size = WALK_BIN_SLICE;

  bool published = false;
  walk_help_bin_job (&job, &published);

  if (published)
    {
      GREP_TEST_DELAY ("walk_bin_job");
      pthread_mutex_lock (&walk.mu);
      walk.bin_job = nullptr;
      while (atomic_load (&job.active_workers) > 1)
        pthread_cond_wait (&walk.cv, &walk.mu);
      pthread_mutex_unlock (&walk.mu);
    }

  return atomic_load (&job.found);
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
          if (walk.bin_job)
            {
              struct walk_bin_job *job = walk.bin_job;
              atomic_fetch_add (&job->active_workers, 1);
              pthread_mutex_unlock (&walk.mu);
              walk_help_bin_job (job, nullptr);
              pthread_mutex_lock (&walk.mu);
              if (atomic_fetch_sub (&job->active_workers, 1) == 2)
                pthread_cond_broadcast (&walk.cv);
              continue;
            }
          if (walk.done)
            break;
          walk.idle++;
          pthread_cond_wait (&walk.cv, &walk.mu);
          walk.idle--;
          continue;
        }
      walk.running++;
      pthread_mutex_unlock (&walk.mu);
      GREP_TEST_DELAY ("walk_task");
      bool should_commit = walk_run_task (t);
      if (should_commit)
        walk_commit ();
      walk_maybe_recruit ();
      pthread_mutex_lock (&walk.mu);
      if (!should_commit && walk.running == 1)
        {
          pthread_mutex_unlock (&walk.mu);
          walk_commit ();
          pthread_mutex_lock (&walk.mu);
        }
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

/* Initialize walk state for a new generation rooted at HOLDER and
   return the new generation number.  WALK.mu must be held.  */
static unsigned int
walk_begin_generation_locked (struct walk_dir *holder)
{
  walk.logical = (fts_options & FTS_LOGICAL) != 0;
  walk.fd_cap = walk_fd_cap ();
  atomic_store_explicit (&walk.cur, holder, memory_order_release);
  walk.done = false;
  atomic_store (&walk.buffered_bytes, 0);
  return ++walk.gen;
}

/* Run the walk generation GEN on the calling thread (recruiting pool
   workers if needed) and wait for all participating workers to leave.  */
static void
walk_execute_generation (unsigned int gen)
{
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

/* Search contiguous directory operands recursively.  */
static void
walk_run_dirs (char *const *dirs, size_t ndirs)
{
  /* Commit all output that precedes the traversal.  */
  producer_flush_batch ();
  rob_wait_all_committed ();

  struct walk_dir *holder = walk_dir_new (nullptr, 0, "", 0);
  holder->ents = xzalloc (ndirs * sizeof *holder->ents);
  holder->nents = ndirs;

  idx_t total_names_len = 0;
  for (size_t i = 0; i < ndirs; i++)
    total_names_len += strlen (dirs[i]) + 1;
  holder->names = ximalloc (total_names_len);
  char *p = holder->names;
  for (size_t i = 0; i < ndirs; i++)
    {
      size_t len = strlen (dirs[i]);
      memcpy (p, dirs[i], len + 1);
      holder->ents[i].name = p;
      holder->ents[i].namelen = (unsigned short) MIN (len, 65535);
      holder->ents[i].kind = WALK_DIR;
      holder->ents[i].status = true;
      p += len + 1;
    }

  atomic_init (&holder->refs, ndirs + 1);
  atomic_init (&holder->fd_users, ndirs);

  pthread_mutex_lock (&walk.mu);
  unsigned int gen = walk_begin_generation_locked (holder);
  uint64_t span = (holder->key_hi - holder->key_lo) / (holder->nents + 1);
  for (size_t i = 0; i < ndirs; i++)
    {
      struct walk_task *t = xmalloc (sizeof *t);
      *t = (struct walk_task) { .dir = holder, .lo = i, .hi = i + 1,
                                .key = holder->key_lo + i * span,
                                .read = true };
      walk_heap_push (&walk.reads, t);
    }
  pthread_mutex_unlock (&walk.mu);

  walk_execute_generation (gen);
}

static void
walk_run_cmd_files (char *const *files, size_t nfiles)
{
  /* Commit all output that precedes the traversal.  */
  producer_flush_batch ();
  rob_wait_all_committed ();

  struct walk_dir *holder = walk_dir_new (nullptr, 0, "", 0);
  holder->ents = xzalloc (nfiles * sizeof *holder->ents);
  holder->nents = nfiles;

  for (size_t i = 0; i < nfiles; i++)
    {
      holder->ents[i].name = files[i];
      holder->ents[i].namelen = (unsigned short) MIN (strlen (files[i]), 65535);
      holder->ents[i].kind = WALK_FILE;
      holder->ents[i].status = true;
    }

  size_t ntasks = (nfiles + WALK_BATCH - 1) / WALK_BATCH;
  atomic_init (&holder->refs, ntasks + 1);
  atomic_init (&holder->fd_users, 0);

  pthread_mutex_lock (&walk.mu);
  unsigned int gen = walk_begin_generation_locked (holder);
  uint64_t span = (holder->key_hi - holder->key_lo) / (holder->nents + 1);
  for (size_t b = 0; b < nfiles; b += WALK_BATCH)
    {
      struct walk_task *t = xmalloc (sizeof *t);
      *t = (struct walk_task) {
        .dir = holder,
        .lo = b,
        .hi = MIN (b + WALK_BATCH, nfiles),
        .key = holder->key_lo + b * span,
        .read = false
      };
      walk_heap_push (&walk.files, t);
    }
  pthread_mutex_unlock (&walk.mu);

  walk_execute_generation (gen);
}
