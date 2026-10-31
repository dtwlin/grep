/* Worker pool and reorder buffer of grep.
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

/* Without GREP_THREADS, use as many threads as there are usable
   processors, but at most this many.  With 32 threads instead of 16,
   traversals of a Linux source tree took 5-21% less time and searches
   of a 1.4 GB file 14-19% less on a machine with 64 cores and 128
   hardware threads; on 32 of those cores (64 hardware threads) the
   savings were 18-24% and 9-14%.  More than 32 threads slowed
   traversals down on both.  */
enum { DEFAULT_MAX_THREADS = 32 };

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
        return MIN (count, DEFAULT_MAX_THREADS);
    }
#endif

#if defined _SC_NPROCESSORS_ONLN
  long n = sysconf (_SC_NPROCESSORS_ONLN);
  if (n > 0)
    return (int) MIN (n, DEFAULT_MAX_THREADS);
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
  bool had_prline;              /* see TASK_HAD_PRLINE */
  bool sep;

  char *stdout_buf;
  size_t stdout_len;
  size_t stdout_cap;

  char *stderr_buf;
  size_t stderr_len;
  size_t stderr_cap;

  size_t *big;
  size_t nbig;

  bool is_chunk;
  bool is_last_chunk;
  intmax_t chunk_count;

  /* True if the chunk's output omitted a line because of an encoding
     error, so that a "binary file matches" message must follow the
     output of the file's last chunk.  */
  bool enc_error;

  /* If nonnull, the chunked file whose results this slot holds; the
     slot must not be committed until GATE->decided.  */
  struct chunk_file_state *gate;
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

  /* True if the slice has a null byte; the committer then searches it.  */
  bool stream_has_nul;

  char inline_out[1024];
};

enum work_task_type
{
  TASK_TYPE_FILE,
  TASK_TYPE_CHUNK,
  TASK_TYPE_STREAM_CHUNK,
  TASK_TYPE_MMAP_SCAN,
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
  size_t file_size;
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

  /* Whether some chunk contains a null byte.  Single-threaded grep
     then treats the rest of the file as binary from a point that
     depends on where its read buffers end, so the file is searched
     sequentially instead.  In the modes that search each chunk in a
     single pass, the chunks' slots must not be committed before that
     is decided: CHUNKS_DONE counts the filled slots, which start at
     FIRST_SEQ, and DECIDED tells whether they are final.  These three
     are protected by pool_mutex.  */
  _Atomic bool has_nul;
  size_t chunks_done;
  bool decided;
  uint64_t first_seq;

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

/* Files searched in chunks are mapped at an address that is a multiple of
   CHUNK_MAP_ALIGN, the size of a PMD-level huge page on x86-64 and on
   arm64 with 4 KiB pages, so that the kernel can map each 2 MiB page
   cache folio of the file with a single page table entry.  */
enum { CHUNK_MAP_ALIGN = 2 * 1024 * 1024 };

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

static char *slice_freelist[SLICE_POOL_CAP];
static int slice_freelist_cnt = 0;
static pthread_cond_t slice_avail_cv = PTHREAD_COND_INITIALIZER;

static char *alloc_slice_buf_locked (void);
static void free_slice_buf_locked (char *ptr);
static char *grow_slice_buf (char *chunk_alloc, size_t used_len,
                             size_t new_cap);
static void cleanup_slice_freelist (void);

static int pool_nthreads;
static pthread_t *pool_threads;

/* The number of worker threads to use, and whether they have been
   started.  Workers start only when there is work for them; a
   directory traversal first runs on the main thread alone and recruits
   them only if it lasts long enough, so that small trees and early exits
   do not pay for starting and stopping threads.  */
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

/* Whether the committed part of standard input read in slices contains
   a null byte, and whether a line matched after the point where
   single-threaded grep would have started treating it as binary.  */
static bool stream_binary;
static bool stream_binary_match;

/* How much a read of single-threaded grep would return from standard
   input, if the writer keeps the pipe full: its read size, or less if
   the pipe holds less.  */
static idx_t stream_readsize;

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
      if (from < to && !stdout_errno)
        fwrite_errno (out + from, 1, to - from);
      return;
    }
  while (from < to && !stdout_errno)
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
               size_t errlen, size_t const *big, size_t nbig, bool sep,
               bool had_prline)
{
  size_t pos = 0, bigi = 0;
  char const *lim = err + errlen;
  for (char const *p = err; p < lim; )
    {
      struct errrec h;
      memcpy (&h, p, sizeof h);
      char const *text = p + sizeof h;
      bool wrote_stdout = (sep && h.after_sep) || (h.outpos > pos);
      if (sep && h.after_sep)
        {
          print_group_separator ();
          sep = false;
        }
      commit_stdout (out, pos, h.outpos, big, nbig, &bigi);
      pos = h.outpos;
      if (wrote_stdout && stdout_errno && had_prline)
        die_write_error ();
      fflush (stdout);
      fwrite (text, 1, h.len, stderr);
      p = text + h.len;
    }
  bool tail_stdout = sep || (outlen > pos);
  if (sep)
    print_group_separator ();
  commit_stdout (out, pos, outlen, big, nbig, &bigi);
  if (tail_stdout && stdout_errno && had_prline)
    die_write_error ();
}

/* Reset the per-file state of the current thread before searching a
   file into its buffers.  */
static void
task_reset (void)
{
#define GREP_TLS_RESET(type, name, reset) reset;
  GREP_TASK_TLS_SAVED (GREP_TLS_RESET)
  GREP_TASK_TLS_UNSAVED (GREP_TLS_RESET)
#undef GREP_TLS_RESET
}

/* Saved thread-local search and buffering state when a thread
   temporarily switches to a nested search or stream-slice commit.  */
struct search_tls_state
{
#define GREP_TLS_FIELD(type, name, reset) type name;
  GREP_TASK_TLS_SAVED (GREP_TLS_FIELD)
#undef GREP_TLS_FIELD
};

static inline void
save_search_tls (struct search_tls_state *s)
{
#define GREP_TLS_SAVE(type, name, reset) s->name = name;
  GREP_TASK_TLS_SAVED (GREP_TLS_SAVE)
#undef GREP_TLS_SAVE
}

static inline void
restore_search_tls (struct search_tls_state const *s)
{
#define GREP_TLS_RESTORE(type, name, reset) name = s->name;
  GREP_TASK_TLS_SAVED (GREP_TLS_RESTORE)
#undef GREP_TLS_RESTORE
}

/* Free SLOT's standard output and the offsets of its big writes, and
   clear the flags that describe that output.  */
static void
rob_slot_discard_stdout (struct rob_slot *slot)
{
  free (slot->stdout_buf);
  slot->stdout_buf = nullptr;
  slot->stdout_len = 0;
  free (slot->big);
  slot->big = nullptr;
  slot->nbig = 0;
  slot->enc_error = false;
  slot->had_prline = false;
  slot->had_prtext = false;
  slot->sep = false;
}

/* Free all of SLOT's output: its standard output as
   rob_slot_discard_stdout does, and its standard error.  */
static void
rob_slot_discard_output (struct rob_slot *slot)
{
  rob_slot_discard_stdout (slot);
  free (slot->stderr_buf);
  slot->stderr_buf = nullptr;
  slot->stderr_len = 0;
  slot->errseen = false;
}

/* Move the status and output of SRC, a slot outside the reorder
   buffer, into DST, whose output has been discarded.  */
static void
rob_slot_move_output (struct rob_slot *dst, struct rob_slot const *src)
{
  dst->status = src->status;
  dst->errseen = src->errseen;
  dst->had_prtext = src->had_prtext;
  dst->had_prline = src->had_prline;
  dst->sep = src->sep;
  dst->enc_error = src->enc_error;
  if (!src->stdout_buf && src->stdout_len > 0)
    memcpy (dst->inline_out, src->inline_out, src->stdout_len);
  dst->stdout_buf = src->stdout_buf;
  dst->stdout_len = src->stdout_len;
  dst->stdout_cap = src->stdout_cap;
  dst->stderr_buf = src->stderr_buf;
  dst->stderr_len = src->stderr_len;
  dst->stderr_cap = src->stderr_cap;
  dst->big = src->big;
  dst->nbig = src->nbig;
}

/* Move the current thread's newly allocated output buffers and flags
   (some of the variables of GREP_TASK_TLS_SAVED) into SLOT.  */
static inline void
rob_capture_tls_output (struct rob_slot *slot)
{
  slot->errseen = tls_errseen;
  slot->had_prtext = task_had_prtext;
  slot->had_prline = task_had_prline;
  slot->sep = task_sep;
  if (tls_out.data && tls_out.len <= sizeof slot->inline_out)
    {
      memcpy (slot->inline_out, tls_out.data, tls_out.len);
      slot->stdout_buf = nullptr;
      slot->stdout_len = tls_out.len;
      slot->stdout_cap = sizeof slot->inline_out;
      membuf_free (&tls_out);
    }
  else
    {
      slot->stdout_buf = tls_out.data;
      slot->stdout_len = tls_out.len;
      slot->stdout_cap = tls_out.cap;
    }
  slot->stderr_buf = tls_err.data;
  slot->stderr_len = tls_err.len;
  slot->stderr_cap = tls_err.cap;
  slot->big = CAST_ALIGNED (size_t *, tls_big.data);
  slot->nbig = tls_big.len / (2 * sizeof (size_t));
  tls_out = tls_err = tls_big = (struct membuf) { .data = nullptr };
}

static struct rob_slot *stream_commit_slice_locked (struct rob_slot *slot);

/* Release all dynamic buffers and callbacks owned by SLOT and mark it
   empty.  POOL_MUTEX must be held.  */
static void
rob_free_slot_resources_locked (struct rob_slot *slot)
{
  rob_slot_discard_output (slot);

  if (slot->cleanup_fn)
    {
      slot->cleanup_fn (slot->cleanup_arg);
      slot->cleanup_fn = nullptr;
      slot->cleanup_arg = nullptr;
    }

  free (slot->filename);
  slot->filename = nullptr;

  if (slot->stream_chunk_alloc)
    {
      free_slice_buf_locked (slot->stream_chunk_alloc);
      slot->stream_chunk_alloc = nullptr;
      slot->stream_chunk_data = nullptr;
    }

  slot->state = ROB_SLOT_EMPTY;
}

static bool rob_committing = false;

static void
rob_try_commit_locked (void)
{
  if (rob_committing)
    return;
  rob_committing = true;

  bool saved_buffering = buffering_enabled;
  char const *saved_outer_filename = filename;
  buffering_enabled = false;
  bool committed_any = false;

  while (commit_seq < next_seq)
    {
      struct rob_slot *slot = &rob[commit_seq % ROB_CAP];
      if (slot->state != ROB_SLOT_DONE)
        break;
      if (slot->gate && !slot->gate->decided)
        break;

      filename = slot->filename;

      /* Group separator logic across files */
      bool want_sep = need_group_separator (slot->had_prtext, slot->sep);
      if (slot->had_prtext)
        global_used = true;

      if (slot->is_stream_chunk)
        slot = stream_commit_slice_locked (slot);

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
                print_file_count (chunk_accumulated_count);
              else
                print_listed_file (!chunk_has_any_match);

              slot->status = !chunk_has_any_match;
              binary_message = (chunk_enc_error
                                || (stream_binary_match
                                    && binary_files == BINARY_BINARY_FILES
                                    && !initial_out_quiet));
              chunk_accumulated_count = 0;
              chunk_has_any_match = false;
              chunk_enc_error = false;
              stream_commit_totalnl = 0;
              stream_binary = stream_binary_match = false;
            }
          else
            {
              if (count_matches || list_files != LISTFILES_NONE)
                slot->status = true;
            }
        }

      commit_output (slot->stdout_buf ? slot->stdout_buf : slot->inline_out,
                     slot->stdout_len, slot->stderr_buf, slot->stderr_len,
                     slot->big, slot->nbig, want_sep, slot->had_prline);

      /* Like single-threaded grep, report omitted lines once, after all
         the output of the file.  */
      if (binary_message)
        error (0, 0, _("%s: binary file matches"),
               slot->filename ? slot->filename : _("(standard input)"));

      if (line_buffered)
        fflush_errno ();

      if (stdout_errno && slot->had_prline)
        die_write_error ();

      overall_status &= slot->status;
      if (slot->errseen)
        errseen = true;

      bool matched = !slot->status;
      rob_free_slot_resources_locked (slot);
      commit_seq++;
      committed_any = true;

      if (exit_on_match && matched)
        {
          if (line_buffered)
            fflush_errno ();
          if (stdout_errno)
            die_write_error ();
          GREP_TEST_DELAY ("quiet_exit");
          atomic_store (&skip_clean_up_stdout, true);
          exit (EXIT_SUCCESS);
        }
    }

  filename = saved_outer_filename;
  buffering_enabled = saved_buffering;
  rob_committing = false;
  if (committed_any)
    {
      if (rob_space_waiters > 0)
        pthread_cond_broadcast (&rob_space_cv);
      if (rob_done_waiters > 0)
        pthread_cond_broadcast (&rob_done_cv);
    }
}

static void drain_queue_if_no_workers_locked (void);

static uint64_t
producer_alloc_slot_locked (void)
{
  while (next_seq - commit_seq >= ROB_CAP && !pool_terminate)
    {
      drain_queue_if_no_workers_locked ();
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

static void chunk_slot_done_locked (struct chunk_file_state *state);

/* Return true if chunk tasks of the file of STATE should still check
   for null bytes.  */
static bool
chunk_nul_check (struct chunk_file_state *state)
{
  return (eolbyte && binary_files != TEXT_BINARY_FILES
          && !atomic_load_explicit (&state->has_nul, memory_order_relaxed));
}

/* Note that the file of STATE contains a null byte.  */
static void
chunk_note_nul (struct chunk_file_state *state)
{
  atomic_store_explicit (&state->has_nul, true, memory_order_relaxed);
}

/* Return the number of lines in the chunk [BEG, LIM) of the file of
   STATE, and note whether the chunk contains a null byte.  */
static idx_t
chunk_count_newlines (char const *beg, char const *lim,
                      struct chunk_file_state *state)
{
  if (!chunk_nul_check (state))
    return count_newlines (beg, lim);

  idx_t newlines = 0;
  bool nul = false;
#if defined __AVX2__
  if (lim - beg >= 128)
    {
      /* Count line ends as count_newlines does, and track the least
         byte seen, which is zero if there is a null byte.  */
      __m256i match_eol = _mm256_set1_epi8 (eolbyte);
      __m256i least = _mm256_set1_epi8 (-1);
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
          least = _mm256_min_epu8 (least,
                                   _mm256_min_epu8 (_mm256_min_epu8 (v0, v1),
                                                    _mm256_min_epu8 (v2, v3)));
          beg += 128;
        }
      nul = (_mm256_movemask_epi8 (_mm256_cmpeq_epi8 (least,
                                                      _mm256_setzero_si256 ()))
             != 0);
    }
#endif
  newlines += count_newlines (beg, lim);
  if (nul || memchr (beg, '\0', lim - beg))
    chunk_note_nul (state);
  return newlines;
}

/* If SEARCH, search the chunk [BEG, LIM) of the file of STATE with
   grepbuf, a line-aligned piece at a time, stopping at the first match
   if DONE_ON_MATCH; and note whether the chunk contains a null byte.
   Return what grepbuf returned in total.  */
static intmax_t
chunk_grepbuf (char *beg, char *lim, struct chunk_file_state *state,
               bool search)
{
  bool check = chunk_nul_check (state);
  intmax_t n = 0;
  for (char *p = beg; p < lim && (search || check); )
    {
      char *q = lim;
      if (CHUNK_PIECE < lim - p)
        {
          char *nl = memchr (p + CHUNK_PIECE, eolbyte,
                             lim - (p + CHUNK_PIECE));
          if (nl)
            q = nl + 1;
        }
      bool stop = false;
      if (search)
        {
          n += grepbuf (p, q);
          stop = n && done_on_match;
        }

      /* Check the piece after searching it, while it is in cache.  */
      if (check && memchr (p, '\0', q - p))
        {
          chunk_note_nul (state);
          check = false;
        }

      if (stop)
        {
          /* Single-threaded grep stops reading here.  A null byte
             further on matters only if it would make the file not
             match.  */
          search = false;
          if (binary_files != WITHOUT_MATCH_BINARY_FILES
              && !patterns_have_nul && skip_empty_lines)
            check = false;
        }
      p = q;
    }
  return n;
}

/* Wait until at least NEEDED slots are free in WORK_QUEUE, or
   POOL_TERMINATE is set.  POOL_MUTEX must be held.  Return true if
   space is available, false if terminating.  */
static inline bool
work_queue_wait_space_locked (size_t needed)
{
  while (q_count + needed > WORK_QUEUE_CAP && !pool_terminate)
    {
      drain_queue_if_no_workers_locked ();
      rob_try_commit_locked ();
      if (q_count + needed <= WORK_QUEUE_CAP)
        break;
      pthread_cond_wait (&q_not_full_cv, &pool_mutex);
    }
  return !pool_terminate;
}

static inline void
work_queue_push_locked (struct work_task const *task)
{
  work_queue[q_tail] = *task;
  q_tail = (q_tail + 1) % WORK_QUEUE_CAP;
  q_count++;
}

/* Wait until all allocated ROB slots have been committed.
   POOL_MUTEX must be held.  */
static inline void
rob_wait_all_committed_locked (void)
{
  while (commit_seq < next_seq)
    {
      drain_queue_if_no_workers_locked ();
      rob_try_commit_locked ();
      if (commit_seq >= next_seq)
        break;
      rob_done_waiters++;
      pthread_cond_wait (&rob_done_cv, &pool_mutex);
      rob_done_waiters--;
    }
}

static inline void
rob_wait_all_committed (void)
{
  pthread_mutex_lock (&pool_mutex);
  rob_wait_all_committed_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

/* Execute a Pass-1 mmap scan task for a single file chunk.  */
static inline void
worker_run_mmap_scan (struct work_task const *task)
{
  struct chunk_file_state *state = task->shared_state;
  size_t idx = task->chunk_idx;
  char const *cbeg = task->chunk_data;
  char const *clim = task->chunk_data + task->chunk_len;
  filename = task->filename;

  if (cbeg == clim)
    {
      state->chunk_nls[idx] = 0;
      state->chunk_has_matches[idx] = false;
    }
  else
    {
      state->chunk_nls[idx] = chunk_count_newlines (cbeg, clim, state);

      idx_t match_size;
      ptrdiff_t off = execute (compiled_pattern, cbeg, clim - cbeg,
                               &match_size, nullptr);
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
              if (out_before < 0 && out_after < 0
                  && atomic_load (&state->match_overflow))
                break;

              ptrdiff_t mo;
              if (p == cbeg && off >= 0)
                mo = off;
              else
                mo = execute (compiled_pattern, p, clim - p, &match_size,
                              nullptr);

              if (mo < 0)
                break;
              char const *b = p + mo;
              char const *endp = b + match_size;
              if (b == clim)
                break;
              char const *mbeg = b;
              char const *lbeg = mbeg;
              while (lbeg > cbeg && lbeg[-1] != eolbyte)
                lbeg--;
              char const *lend = memchr (mbeg, eolbyte, clim - mbeg);
              lend = lend ? lend + 1 : clim;

              if (out_line)
                {
                  running_nl += count_newlines (nl_cursor, lbeg);
                  nl_cursor = lbeg;
                }

              pthread_mutex_lock (&state->match_mutex);
              if (state->match_count >= state->match_cap)
                {
                  if (0 <= out_before || 0 <= out_after)
                    {
                      state->match_cap *= 2;
                      state->matches
                        = xrealloc (state->matches,
                                    (state->match_cap
                                     * sizeof *state->matches));
                    }
                  else
                    atomic_store (&state->match_overflow, true);
                }
              if (state->match_count < state->match_cap)
                state->matches[state->match_count++] = (struct match_record) {
                  .chunk_idx = idx,
                  .line_beg = lbeg,
                  .line_end = lend,
                  .rel_nl = running_nl
                };
              pthread_mutex_unlock (&state->match_mutex);
              p = endp;
            }
        }
    }

  filename = nullptr;
  /* Count the chunk under DONE_MUTEX and do not touch STATE
     afterwards: once the last chunk is counted, the producer
     may go on to free STATE.  */
  pthread_mutex_lock (&state->done_mutex);
  if (atomic_fetch_add (&state->chunks_completed, 1) == state->num_chunks - 1)
    pthread_cond_signal (&state->done_cv);
  pthread_mutex_unlock (&state->done_mutex);
  GREP_TEST_DELAY ("mmap_scan_done");
}

/* Execute a TASK_TYPE_STREAM_CHUNK task.  Return true if the ROB slot
   was directly completed under POOL_MUTEX (which remains held on
   return); return false if normal slot completion should proceed.  */
static inline bool
worker_run_stream_chunk (struct work_task const *task, bool *status_out,
                         intmax_t *chunk_matched_cnt_out, bool *enc_error_out)
{
  filename = label;
  bufbeg = (char *) task->chunk_data;
  buflim = (char *) task->chunk_data + task->chunk_len;
  totalcc = task->chunk_totalcc;
  totalnl = task->chunk_lineno_offset;
  lastnl = bufbeg;
  offset_width = compute_offset_width (nullptr);

  /* Whether and how a slice with a null byte is output depends on
     what precedes it, which only the committer knows; leave such a
     slice to it.  */
  if (eolbyte && binary_files != TEXT_BINARY_FILES
      && memchr (bufbeg, '\0', buflim - bufbeg))
    {
      pthread_mutex_lock (&pool_mutex);
      struct rob_slot *slot = &rob[task->seq % ROB_CAP];
      slot->is_chunk = true;
      slot->is_last_chunk = task->is_last_chunk;
      slot->is_stream_chunk = true;
      slot->stream_has_nul = true;
      slot->stream_chunk_alloc = (char *) (task->chunk_data - 1);
      slot->stream_chunk_data = bufbeg;
      slot->stream_chunk_len = task->chunk_len;
      slot->stream_totalcc = task->chunk_totalcc;
      slot->status = true;
      slot->state = ROB_SLOT_DONE;
      rob_try_commit_locked ();
      return true;
    }

  if (out_line && !out_quiet && !count_matches && list_files == LISTFILES_NONE)
    {
      intmax_t chunk_nls = count_newlines (bufbeg, buflim);
      char *lbeg = bufbeg;
      intmax_t first_rel_nl = 0;

      if (!out_invert)
        {
          idx_t match_size;
          ptrdiff_t mo = execute (compiled_pattern, bufbeg, buflim - bufbeg,
                                  &match_size, nullptr);
          if (mo < 0 || bufbeg + mo == buflim)
            {
              pthread_mutex_lock (&pool_mutex);
              free_slice_buf_locked ((char *) (task->chunk_data - 1));
              struct rob_slot *slot = &rob[task->seq % ROB_CAP];
              slot->is_chunk = true;
              slot->is_last_chunk = task->is_last_chunk;
              slot->is_stream_chunk = true;
              slot->chunk_nls = chunk_nls;
              slot->stream_chunk_alloc = nullptr;
              slot->stream_chunk_data = nullptr;
              slot->status = true;
              slot->state = ROB_SLOT_DONE;
              rob_try_commit_locked ();
              return true;
            }
          if (out_before < 0 && out_after < 0)
            {
              lbeg = bufbeg + mo;
              while (lbeg > bufbeg && lbeg[-1] != eolbyte)
                lbeg--;
              first_rel_nl = count_newlines (bufbeg, lbeg);
            }
        }

      pthread_mutex_lock (&pool_mutex);
      struct rob_slot *slot = &rob[task->seq % ROB_CAP];
      slot->is_chunk = true;
      slot->is_last_chunk = task->is_last_chunk;
      slot->is_stream_chunk = true;
      slot->chunk_nls = chunk_nls;
      slot->stream_chunk_alloc = (char *) (task->chunk_data - 1);
      slot->stream_chunk_data = bufbeg;
      slot->stream_chunk_len = task->chunk_len;
      slot->stream_first_lbeg = lbeg;
      slot->stream_first_rel_nl = first_rel_nl;
      slot->stream_totalcc = task->chunk_totalcc;
      slot->status = false;
      slot->state = ROB_SLOT_DONE;
      rob_try_commit_locked ();
      return true;
    }

  if (count_matches)
    {
      out_quiet = true;
      *chunk_matched_cnt_out = grepbuf (bufbeg, buflim);
      *status_out = (*chunk_matched_cnt_out == 0);
    }
  else
    {
      encoding_error_output = false;
      intmax_t lines_matched = grepbuf (bufbeg, buflim);
      *status_out = (lines_matched == 0);
      *enc_error_out = (binary_files == BINARY_BINARY_FILES && !out_quiet
                        && encoding_error_output);
    }
  return false;
}

/* Execute a TASK_TYPE_CHUNK task on a regular file mmap slice.  */
static inline void
worker_run_file_chunk (struct work_task const *task, bool *status_out,
                       intmax_t *chunk_matched_cnt_out, bool *enc_error_out)
{
  filename = task->filename;
  bufbeg = (char *) task->chunk_data;
  buflim = (char *) task->chunk_data + task->chunk_len;
  totalcc = task->chunk_totalcc;
  totalnl = task->chunk_lineno_offset;
  lastnl = (char *) task->chunk_data;
  offset_width = (task->shared_state
                  ? ((struct chunk_file_state *) task->shared_state)->offset_width
                  : 0);
  encoding_error_output = false;

  if (task->mode == CHUNK_MODE_COUNT)
    {
      struct chunk_file_state *state = task->shared_state;
      if (atomic_load (&state->early_stop))
        {
          *status_out = true;

          /* With --binary-files=without-match, a null byte
             before the first match makes the file not match.  */
          if (binary_files == WITHOUT_MATCH_BINARY_FILES
              || patterns_have_nul || !skip_empty_lines)
            chunk_grepbuf (bufbeg, buflim, state, false);
        }
      else
        {
          out_quiet = true;
          intmax_t cnt = chunk_grepbuf (bufbeg, buflim, state, true);
          *chunk_matched_cnt_out = cnt;
          if (cnt > 0)
            {
              atomic_store (&state->has_match, true);
              if (exit_on_match || list_files == LISTFILES_MATCHING)
                atomic_store (&state->early_stop, true);
            }
          *status_out = (cnt == 0);
        }
    }
  else
    {
      intmax_t lines_matched
        = (task->mode == CHUNK_MODE_STATELESS
           ? chunk_grepbuf (bufbeg, buflim, task->shared_state, true)
           : grepbuf (bufbeg, buflim));
      *status_out = (lines_matched == 0);
      *enc_error_out = (binary_files == BINARY_BINARY_FILES && !out_quiet
                        && encoding_error_output);
    }
  filename = nullptr;
}

/* Populate ROB slot for TASK with the current worker's TLS output and
   status, then attempt to commit completed ROB slots in order.
   POOL_MUTEX must be held.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE void
rob_complete_worker_slot_locked (struct work_task const *task, bool status,
                                 intmax_t chunk_matched_cnt, bool enc_error,
                                 char *copy_out)
{
  if (task->chunk_free_data)
    free_slice_buf_locked ((char *) (task->chunk_data - 1));

  struct rob_slot *slot = &rob[task->seq % ROB_CAP];
  slot->status = status;
  slot->errseen = tls_errseen;
  slot->had_prtext = task_had_prtext;
  slot->had_prline = task_had_prline;
  slot->sep = task_sep;
  slot->chunk_count = chunk_matched_cnt;
  slot->enc_error = enc_error;
  if (tls_big.len)
    {
      slot->big = ximemdup (tls_big.data, tls_big.len);
      slot->nbig = tls_big.len / (2 * sizeof (size_t));
    }
  if (task->type == TASK_TYPE_CHUNK || task->type == TASK_TYPE_STREAM_CHUNK)
    {
      slot->is_chunk = true;
      slot->is_last_chunk = task->is_last_chunk;
      slot->is_stream_chunk = task->type == TASK_TYPE_STREAM_CHUNK;
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
  if (task->type == TASK_TYPE_CHUNK && task->mode != CHUNK_MODE_LINE_NUMBER)
    chunk_slot_done_locked (task->shared_state);
  rob_try_commit_locked ();
}

static void
worker_execute_task (struct work_task const *task)
{
  if (task->type == TASK_TYPE_WALK)
    {
      walk_participate (task->seq);
      pthread_mutex_lock (&pool_mutex);
      return;
    }

  if (task->type == TASK_TYPE_MMAP_SCAN)
    {
      worker_run_mmap_scan (task);
      pthread_mutex_lock (&pool_mutex);
      return;
    }

  task_reset ();

  bool status = true;
  intmax_t chunk_matched_cnt = 0;
  bool enc_error = false;

  if (task->type == TASK_TYPE_FILE)
    {
      filename = task->filename;
      status = grepfile (AT_FDCWD, task->filename, task->follow,
                         task->command_line);
      free (task->filename);
      filename = nullptr;
    }
  else if (task->type == TASK_TYPE_STREAM_CHUNK)
    {
      if (worker_run_stream_chunk (task, &status, &chunk_matched_cnt,
                                   &enc_error))
        return;
    }
  else if (task->type == TASK_TYPE_CHUNK)
    {
      worker_run_file_chunk (task, &status, &chunk_matched_cnt,
                             &enc_error);
    }

  char *copy_out = nullptr;
  if (tls_out.len > sizeof rob[0].inline_out && tls_out.len <= 32768)
    {
      copy_out = ximalloc (tls_out.len);
      memcpy (copy_out, tls_out.data, tls_out.len);
    }

  GREP_TEST_DELAY ("worker_complete");
  pthread_mutex_lock (&pool_mutex);
  rob_complete_worker_slot_locked (task, status, chunk_matched_cnt,
                                   enc_error, copy_out);
}

static void
drain_queue_if_no_workers_locked (void)
{
  while (pool_nthreads == 0 && q_count > 0 && !pool_terminate)
    {
      struct work_task task = work_queue[q_head];
      q_head = (q_head + 1) % WORK_QUEUE_CAP;
      q_count--;
      pthread_mutex_unlock (&pool_mutex);

      struct search_tls_state saved;
      save_search_tls (&saved);
      tls_out = tls_err = tls_big = (struct membuf) { .data = nullptr };
      buffering_enabled = true;
      worker_execute_task (&task);
      membuf_free (&tls_out);
      membuf_free (&tls_err);
      membuf_free (&tls_big);
      restore_search_tls (&saved);
    }
}

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
      GREP_TEST_DELAY ("worker_dequeued");

      worker_execute_task (&task);
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
      if (!work_queue_wait_space_locked (1))
        {
          struct rob_slot *slot = &rob[seq % ROB_CAP];
          slot->status = true;
          slot->state = ROB_SLOT_DONE;
          rob_try_commit_locked ();
          for (size_t j = i; j < producer_batch_count; ++j)
            free (producer_batch[j].filename);
          producer_batch_count = 0;
          pthread_mutex_unlock (&pool_mutex);
          return;
        }

      struct work_task t = {
        .type = TASK_TYPE_FILE,
        .seq = seq,
        .filename = producer_batch[i].filename,
        .follow = producer_batch[i].follow,
        .command_line = producer_batch[i].command_line
      };
      work_queue_push_locked (&t);
      pthread_cond_signal (&q_not_empty_cv);
    }

  producer_batch_count = 0;
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
  if (pool_started)
    rob_wait_all_committed ();

  if (!buffer)
    init_worker_buffer ();

  bool saved_buffering = buffering_enabled;
  buffering_enabled = false;
  out_quiet = initial_out_quiet;
  done_on_match = initial_done_on_match;
  file_used = false;

  bool status = grep_command_line_arg ("-");
  overall_status &= status;
  if (exit_on_match && !status)
    {
      GREP_TEST_DELAY ("quiet_exit");
      atomic_store (&skip_clean_up_stdout, true);
      exit (EXIT_SUCCESS);
    }

  out_quiet = initial_out_quiet;
  done_on_match = initial_done_on_match;
  buffering_enabled = saved_buffering;
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
