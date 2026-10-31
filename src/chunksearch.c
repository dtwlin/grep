/* Chunked file search and standard-input slicer of grep.
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

struct retained_mmap_entry
{
  void *addr;
  size_t size;
};

enum { MAX_RETAINED_MMAPS = 8 };
static struct retained_mmap_entry retained_mmaps[MAX_RETAINED_MMAPS];
static size_t retained_mmaps_count = 0;
static uint64_t retained_mmap_bytes = 0;

static void
cleanup_chunk_file_state (void *arg)
{
  struct chunk_file_state *state = arg;
  if (!state)
    return;

  void *map_addr = (state->base_addr && state->base_addr != MAP_FAILED)
                   ? state->base_addr : state->mmap_data;
  size_t map_size = (state->base_addr && state->base_addr != MAP_FAILED)
                    ? state->base_size : state->mmap_size;

  if (map_addr && map_addr != MAP_FAILED)
    {
      if (state->mmap_size >= (16 << 20) && map_size <= (2ULL << 30))
        {
          while (retained_mmaps_count > 0
                 && (retained_mmaps_count >= MAX_RETAINED_MMAPS
                     || retained_mmap_bytes + map_size > (2ULL << 30)))
            {
              retained_mmaps_count--;
              munmap (retained_mmaps[retained_mmaps_count].addr,
                      retained_mmaps[retained_mmaps_count].size);
              retained_mmap_bytes -= retained_mmaps[retained_mmaps_count].size;
            }
          retained_mmaps[retained_mmaps_count].addr = map_addr;
          retained_mmaps[retained_mmaps_count].size = map_size;
          retained_mmaps_count++;
          retained_mmap_bytes += map_size;
        }
      else
        munmap (map_addr, map_size);
    }

  if (state->close_fd && state->file_desc >= 0)
    close (state->file_desc);
  else if (!state->close_fd && state->file_desc >= 0)
    {
      if (!exit_on_match)
        lseek (state->file_desc, state->file_size, SEEK_SET);
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

/* Search the file of STATE sequentially into SLOT, as for
   rob_search_deferred.  */
static void
chunk_search_to_slot (struct chunk_file_state *state, struct rob_slot *slot)
{
  struct search_tls_state saved;
  save_search_tls (&saved);
  tls_out = tls_err = tls_big = (struct membuf) { .data = nullptr };
  buffering_enabled = true;
  task_reset ();
  slot->status = chunk_search_sequentially (state, state->fname);
  rob_capture_tls_output (slot);
  restore_search_tls (&saved);
}

/* Queue the results of a sequential search of the file of STATE, named
   FNAME.  */
static void
chunk_sequential_to_rob (char const *fname, struct chunk_file_state *state)
{
  pthread_mutex_lock (&pool_mutex);
  uint64_t seq = producer_alloc_slot_locked ();
  struct rob_slot *slot = &rob[seq % ROB_CAP];
  slot->cleanup_fn = cleanup_chunk_file_state;
  slot->cleanup_arg = state;
  slot->filename = xstrdup (fname);
  pthread_mutex_unlock (&pool_mutex);

  chunk_search_to_slot (state, slot);

  pthread_mutex_lock (&pool_mutex);
  slot->state = ROB_SLOT_DONE;
  rob_try_commit_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

/* Note that the slot of one more chunk of the file of STATE is filled,
   in a mode that searches each chunk in a single pass.  When all are,
   decide whether the file must be searched sequentially; if so, do it
   and let the last slot hold the results instead of the chunks'.
   pool_mutex must be held; it is released while searching.  */
static void
chunk_slot_done_locked (struct chunk_file_state *state)
{
  if (++state->chunks_done < state->num_chunks)
    return;

  if (atomic_load_explicit (&state->has_nul, memory_order_relaxed))
    {
      GREP_TEST_ROUTE (CHUNK_NUL_RESEARCH, state->fname);
      struct rob_slot *result = xzalloc (sizeof *result);
      pthread_mutex_unlock (&pool_mutex);
      chunk_search_to_slot (state, result);
      pthread_mutex_lock (&pool_mutex);

      for (size_t k = 0; k < state->num_chunks; k++)
        {
          struct rob_slot *s = &rob[(state->first_seq + k) % ROB_CAP];
          rob_slot_discard_output (s);
          s->status = true;
          s->chunk_count = 0;
          s->is_chunk = s->is_last_chunk = false;
        }
      struct rob_slot *last
        = &rob[(state->first_seq + state->num_chunks - 1) % ROB_CAP];
      rob_slot_move_output (last, result);
      free (result);
    }

  state->decided = true;
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

  /* Sort and deduplicate matches by line_beg.  */
  qsort (state->matches, state->match_count, sizeof (struct match_record),
         cmp_matches);

  size_t unique_count = 0;
  for (size_t i = 0; i < state->match_count; ++i)
    if (unique_count == 0
        || state->matches[i].line_beg != state->matches[unique_count - 1].line_beg)
      state->matches[unique_count++] = state->matches[i];
  state->match_count = unique_count;

  struct search_tls_state saved;
  save_search_tls (&saved);
  membuf_init (&tls_out, 64 * 1024);
  tls_err = tls_big = (struct membuf) { .data = nullptr };
  buffering_enabled = true;
  task_reset ();
  filename = fname;
  bufbeg = state->mmap_data;
  buflim = state->mmap_data + state->mmap_size;
  lastnl = bufbeg;
  totalnl = 0;
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
  if ((0 <= out_before || 0 <= out_after) && encoding_error_output)
    {
      /* After omitting a line with an encoding error, single-threaded
         grep prints context lines that depend on where its read buffers
         happen to end.  Search the file as it does.  */
      GREP_TEST_ROUTE (CHUNK_ENC_SEQUENTIAL, fname);
      task_reset ();
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

  slot->status = status;
  rob_capture_tls_output (slot);
  restore_search_tls (&saved);
  slot->state = ROB_SLOT_DONE;

  rob_try_commit_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

/* Enqueue an empty terminal chunk slot for FNAME (or standard input
   if null) so that file-level summary output (-c, -L) is committed in
   sequence order.  */
static void
rob_enqueue_empty_last_chunk (char const *fname)
{
  pthread_mutex_lock (&pool_mutex);
  uint64_t seq = producer_alloc_slot_locked ();
  struct rob_slot *slot = &rob[seq % ROB_CAP];
  slot->status = true;
  slot->errseen = false;
  slot->had_prtext = false;
  slot->is_chunk = true;
  slot->is_last_chunk = true;
  slot->filename = fname ? xstrdup (fname) : nullptr;
  slot->state = ROB_SLOT_DONE;
  rob_try_commit_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

static void
chunked_fallback_enqueue (int fd, bool close_fd, char const *fname)
{
  GREP_TEST_ROUTE (CHUNK_FALLBACK, fname);
  if (close_fd)
    {
      close (fd);
      producer_enqueue_file (fname, true, true);
    }
  else
    producer_enqueue_stdin ();
}

/* Enqueue all chunks of STATE in a single-pass mode (CHUNK_MODE_COUNT
   or CHUNK_MODE_STATELESS).  */
static inline void
enqueue_single_pass_chunks (struct chunk_file_state *state,
                            char const *fname, enum chunk_mode mode)
{
  size_t num_chunks = state->num_chunks;
  size_t const *bounds = state->bounds;
  char *data = state->mmap_data;

  for (size_t k = 0; k < num_chunks; ++k)
    {
      size_t c_beg = bounds[k];
      size_t c_end = bounds[k + 1];
      bool is_last = (k == num_chunks - 1);

      GREP_TEST_DELAY ("chunk_enqueue");
      pthread_mutex_lock (&pool_mutex);
      uint64_t seq = producer_alloc_slot_locked ();
      struct rob_slot *slot = &rob[seq % ROB_CAP];
      slot->is_chunk = true;
      slot->is_last_chunk = is_last;
      slot->gate = state;
      if (k == 0)
        state->first_seq = seq;
      if (is_last)
        {
          slot->cleanup_fn = cleanup_chunk_file_state;
          slot->cleanup_arg = state;
          slot->filename = xstrdup (fname);
        }

      if (!work_queue_wait_space_locked (1))
        {
          slot->status = true;
          slot->is_last_chunk = true;
          slot->cleanup_fn = cleanup_chunk_file_state;
          slot->cleanup_arg = state;
          if (!slot->filename)
            slot->filename = xstrdup (fname);
          state->num_chunks = k + 1;
          slot->state = ROB_SLOT_DONE;
          chunk_slot_done_locked (state);
          rob_try_commit_locked ();
          pthread_mutex_unlock (&pool_mutex);
          return;
        }

      struct work_task t = {
        .type = TASK_TYPE_CHUNK,
        .seq = seq,
        .filename = (char *) state->fname,
        .mode = mode,
        .chunk_data = data + c_beg,
        .chunk_len = c_end - c_beg,
        .chunk_totalcc = c_beg,
        .chunk_lineno_offset = 0,
        .chunk_free_data = false,
        .is_last_chunk = is_last,
        .shared_state = state
      };
      work_queue_push_locked (&t);

      pthread_cond_signal (&q_not_empty_cv);
      pthread_mutex_unlock (&pool_mutex);
    }
}

static void stream_slicer_stdin (void);

/* Before starting the worker pool, try a quick 64 KiB pread on FD for
   uninverted -q so an immediate match at the start of a large file can
   exit in < 1 ms without spinning up worker threads.  */
static void
chunk_try_early_quiet_match (int fd, char const *fname, struct stat *st)
{
  char const *saved_filename = filename;
  filename = fname;
  char prebuf[GOOD_READSIZE_MIN + 2 + sizeof (uword)];
  prebuf[0] = eolbyte;
  ssize_t n;
  do
    n = pread (fd, prebuf + 1, GOOD_READSIZE_MIN, 0);
  while (n < 0 && errno == EINTR);
  if (n > 0
      && (!eolbyte || binary_files == TEXT_BINARY_FILES
          || (!patterns_have_nul
              && memchr (prebuf + 1, '\0', n) == nullptr
              && (binary_files != WITHOUT_MATCH_BINARY_FILES
                  || !file_must_have_nulls (n, fd, st)))))
    {
      char *lim = memrchr (prebuf + 1, eolbyte, n);
      if (lim)
        {
          idx_t scan_len = (lim + 1) - (prebuf + 1);
          memset (lim + 1, 0, uword_size);
          idx_t msize = 0;
          ptrdiff_t off = execute (compiled_pattern, prebuf + 1, scan_len,
                                   &msize, nullptr);
          if (off >= 0 && off < scan_len)
            {
              atomic_store (&skip_clean_up_stdout, true);
              exit (EXIT_SUCCESS);
            }
        }
    }
  filename = saved_filename;
}

/* Map FD into memory with one anonymous guard page before and after the
   file mapping so data[-1] and data[effective_size] can hold eolbyte
   sentinels.  Return false (and fall back to sequential queueing) if
   mmap fails or if the file contains NUL bytes.  */
static bool
chunk_mmap_file_with_sentinels (int fd, bool close_fd, char const *fname,
                                struct stat *st, char **base_out,
                                size_t *total_alloc_out, char **data_out,
                                size_t *effective_size_out)
{
  size_t ps = (size_t) pagesize;
  size_t align = MAX (ps, CHUNK_MAP_ALIGN);
  if ((uintmax_t) st->st_size > SIZE_MAX - align - 2 * ps)
    {
      chunked_fallback_enqueue (fd, close_fd, fname);
      return false;
    }
  size_t file_size = (size_t) st->st_size;
  size_t aligned_file_size = (file_size % ps == 0
                              ? file_size
                              : file_size + (ps - file_size % ps));

  /* Reserve room to start the file mapping on an ALIGN boundary after a
     guard page.  File offsets and addresses then agree modulo the huge
     page size, so the kernel can map each 2 MiB page cache folio with
     one PMD entry, and folios never straddle a PMD boundary.  */
  size_t reserve = aligned_file_size + align + ps;
  char *base = mmap (NULL, reserve, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED)
    {
      chunked_fallback_enqueue (fd, close_fd, fname);
      return false;
    }
#ifdef MADV_NOHUGEPAGE
  /* A guard page holds just one sentinel byte; writing it must not
     clear a whole huge page.  */
  madvise (base, reserve, MADV_NOHUGEPAGE);
#endif

  char *data = ALIGN_TO (base + ps, align);
  if (mmap (data, file_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_FIXED, fd, 0)
      == MAP_FAILED)
    {
      munmap (base, reserve);
      chunked_fallback_enqueue (fd, close_fd, fname);
      return false;
    }
#ifdef MADV_HUGEPAGE
  /* Also make page faults on uncached parts of the file read 2 MiB
     folios, which later runs can map with PMD entries too.  */
  madvise (data, aligned_file_size, MADV_HUGEPAGE);
#endif

  /* Give back the reservation outside the two guard pages.  */
  char *map_end = base + reserve;
  char *guard_beg = data - ps;
  char *guard_end = data + aligned_file_size + ps;
  if (base < guard_beg && munmap (base, guard_beg - base) == 0)
    base = guard_beg;
  if (guard_end < map_end && munmap (guard_end, map_end - guard_end) == 0)
    map_end = guard_end;
  size_t total_alloc = map_end - base;

  data[-1] = eolbyte;
  if (data[file_size] != eolbyte)
    data[file_size] = eolbyte;

  char const *saved_filename = filename;
  filename = fname;
  bool has_nulls = (eolbyte && binary_files != TEXT_BINARY_FILES
                    && (memchr (data, '\0', MIN (file_size, GOOD_READSIZE_MIN)) != nullptr
                        || file_must_have_nulls (0, fd, st)));
  filename = saved_filename;
  if (has_nulls)
    {
      munmap (base, total_alloc);
      chunked_fallback_enqueue (fd, close_fd, fname);
      return false;
    }

  size_t effective_size = file_size
    + (file_size > 0 && data[file_size - 1] != eolbyte);
  if (data[effective_size] != eolbyte)
    data[effective_size] = eolbyte;

  *base_out = base;
  *total_alloc_out = total_alloc;
  *data_out = data;
  *effective_size_out = effective_size;
  return true;
}

/* Partition DATA[0..EFFECTIVE_SIZE) into line-aligned chunks and allocate
   its shared chunk_file_state descriptor.  */
static struct chunk_file_state *
chunk_create_state (int fd, bool close_fd, char const *fname,
                    struct stat const *st, char *base, size_t total_alloc,
                    char *data, size_t effective_size)
{
  size_t eff_threads = MAX (1, pool_nthreads);
  size_t num_chunks = MIN (eff_threads * 2, MAX_FILE_CHUNKS);
  size_t min_chunk_size = CHUNK_MIN_SIZE;
  if (effective_size / num_chunks < min_chunk_size)
    num_chunks = effective_size / min_chunk_size;
  if (num_chunks < 2)
    num_chunks = 2;

  size_t *bounds = xnmalloc (num_chunks + 1, sizeof *bounds);
  bounds[0] = 0;
  for (size_t k = 1; k < num_chunks; ++k)
    {
      size_t target = k * (effective_size / num_chunks);
      if (target <= bounds[k - 1])
        {
          bounds[k] = bounds[k - 1];
          continue;
        }
      if (target < effective_size)
        {
          char *nl = memchr (data + target, eolbyte, effective_size - target);
          bounds[k] = nl ? (size_t) (nl - data) + 1 : effective_size;
        }
      else
        bounds[k] = effective_size;
    }
  bounds[num_chunks] = effective_size;

  struct chunk_file_state *state = xzalloc (sizeof *state);
  state->base_addr = base;
  state->base_size = total_alloc;
  state->mmap_data = data;
  state->mmap_size = effective_size;
  state->file_size = (size_t) st->st_size;
  state->file_desc = fd;
  state->close_fd = close_fd;
  state->fname = xstrdup (fname);
  state->fname_allocated = true;
  state->num_chunks = num_chunks;
  state->bounds = bounds;
  state->offset_width = compute_offset_width (st);
  atomic_init (&state->chunks_completed, 0);
  atomic_init (&state->total_count, 0);
  atomic_init (&state->has_match, false);
  atomic_init (&state->early_stop, false);
  pthread_mutex_init (&state->done_mutex, NULL);
  pthread_cond_init (&state->done_cv, NULL);
  pthread_mutex_init (&state->match_mutex, NULL);
  return state;
}

/* Dispatch Pass-1 TASK_TYPE_MMAP_SCAN tasks for all chunks in STATE, wait
   for completion, and compute the per-chunk newline prefix sums.  */
static void
chunk_run_mmap_scan_pass1 (struct chunk_file_state *state)
{
  size_t num_chunks = state->num_chunks;
  size_t const *bounds = state->bounds;
  char *data = state->mmap_data;

  state->chunk_nls = xnmalloc (num_chunks, sizeof *state->chunk_nls);
  state->prefix_nls = xnmalloc (num_chunks + 1, sizeof *state->prefix_nls);
  state->chunk_has_matches = xnmalloc (num_chunks,
                                       sizeof *state->chunk_has_matches);
  state->match_cap = (0 <= out_before || 0 <= out_after) ? 65536 : 4096;
  state->matches = xnmalloc (state->match_cap, sizeof *state->matches);
  state->match_count = 0;
  atomic_init (&state->match_overflow, false);

  pthread_mutex_lock (&pool_mutex);
  if (work_queue_wait_space_locked (num_chunks))
    {
      for (size_t k = 0; k < num_chunks; ++k)
        {
          struct work_task t = {
            .type = TASK_TYPE_MMAP_SCAN,
            .seq = 0,
            .filename = (char *) state->fname,
            .chunk_data = data + bounds[k],
            .chunk_len = bounds[k + 1] - bounds[k],
            .chunk_idx = k,
            .shared_state = state
          };
          work_queue_push_locked (&t);
        }
      drain_queue_if_no_workers_locked ();
      pthread_cond_broadcast (&q_not_empty_cv);
    }
  pthread_mutex_unlock (&pool_mutex);

  pthread_mutex_lock (&state->done_mutex);
  while (atomic_load (&state->chunks_completed) < num_chunks)
    pthread_cond_wait (&state->done_cv, &state->done_mutex);
  pthread_mutex_unlock (&state->done_mutex);

  state->prefix_nls[0] = 0;
  for (size_t k = 0; k < num_chunks; ++k)
    state->prefix_nls[k + 1] = state->prefix_nls[k] + state->chunk_nls[k];
}

/* Dispatch Pass-2 CHUNK_MODE_LINE_NUMBER tasks for chunks that matched
   during Pass 1, marking non-matching chunks immediately done in the ROB.  */
static void
chunk_enqueue_line_number_pass2 (struct chunk_file_state *state,
                                 char const *fname)
{
  size_t num_chunks = state->num_chunks;
  size_t const *bounds = state->bounds;
  char *data = state->mmap_data;

  pthread_mutex_lock (&pool_mutex);
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
          rob_try_commit_locked ();
          continue;
        }

      if (!work_queue_wait_space_locked (1))
        {
          slot->status = true;
          slot->is_last_chunk = true;
          slot->cleanup_fn = cleanup_chunk_file_state;
          slot->cleanup_arg = state;
          if (!slot->filename)
            slot->filename = xstrdup (fname);
          slot->state = ROB_SLOT_DONE;
          break;
        }

      struct work_task t = {
        .type = TASK_TYPE_CHUNK,
        .seq = seq,
        .filename = (char *) state->fname,
        .mode = CHUNK_MODE_LINE_NUMBER,
        .chunk_data = data + c_beg,
        .chunk_len = c_end - c_beg,
        .chunk_totalcc = c_beg,
        .chunk_lineno_offset = state->prefix_nls[k],
        .chunk_free_data = false,
        .is_last_chunk = is_last,
        .shared_state = state
      };
      work_queue_push_locked (&t);
      pthread_cond_signal (&q_not_empty_cv);
    }
  rob_try_commit_locked ();
  pthread_mutex_unlock (&pool_mutex);
}

static void
chunked_grep_fd (int fd, bool close_fd, char const *fname,
                 struct stat const *st_in)
{
  producer_flush_batch ();
  struct stat st = *st_in;

  if (exit_on_match && !out_invert && !pool_started)
    {
      GREP_TEST_ROUTE (CHUNK_QUIET_PROBE, fname);
      chunk_try_early_quiet_match (fd, fname, &st);
    }
  ensure_pool ();

  if (!out_quiet && list_files == LISTFILES_NONE && 1 < max_count
      && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))
    {
      if (close_fd)
        close (fd);
      producer_enqueue_msg (0, _("%s: input file is also the output"), fname);
      producer_enqueue_errseen ();
      return;
    }

  char *base = nullptr;
  char *data = nullptr;
  size_t total_alloc = 0;
  size_t effective_size = 0;
  if (!chunk_mmap_file_with_sentinels (fd, close_fd, fname, &st,
                                       &base, &total_alloc,
                                       &data, &effective_size))
    return;

  struct chunk_file_state *state
    = chunk_create_state (fd, close_fd, fname, &st, base, total_alloc,
                          data, effective_size);

  if (count_matches || exit_on_match || list_files != LISTFILES_NONE)
    {
      GREP_TEST_ROUTE (CHUNK_COUNT, fname);
      enqueue_single_pass_chunks (state, fname, CHUNK_MODE_COUNT);
      return;
    }

  if (!out_line && out_before < 0 && out_after < 0 && max_count == INTMAX_MAX)
    {
      GREP_TEST_ROUTE (CHUNK_STATELESS, fname);
      enqueue_single_pass_chunks (state, fname, CHUNK_MODE_STATELESS);
      return;
    }

  chunk_run_mmap_scan_pass1 (state);
  if (atomic_load (&state->has_nul))
    {
      GREP_TEST_ROUTE (CHUNK_NUL_SEQUENTIAL, fname);
      chunk_sequential_to_rob (fname, state);
      return;
    }

  if ((0 <= out_before || 0 <= out_after)
      || (!out_invert && !count_matches && list_files == LISTFILES_NONE
          && !atomic_load (&state->match_overflow)
          && state->match_count < state->match_cap))
    {
      GREP_TEST_ROUTE (CHUNK_CONTEXT, fname);
      format_context_matches_to_rob (fname, state);
      return;
    }

  GREP_TEST_ROUTE (CHUNK_LINENO, fname);
  chunk_enqueue_line_number_pass2 (state, fname);
}

static void
chunked_grep_file (char const *fname, struct stat const *st_in)
{
  producer_flush_batch ();
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

/* Allocate a slice buffer, prepend RESIDUE_BUF[0..RESIDUE_LEN), and read
   from STDIN_FILENO until at least one complete line is buffered or EOF/error
   is reached.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE char *
stream_read_next_slice (char const *residue_buf, size_t residue_len,
                        size_t *total_in_buf_out, bool *is_eof_out,
                        int *read_err_out)
{
  pthread_mutex_lock (&pool_mutex);
  char *chunk_alloc = alloc_slice_buf_locked ();
  pthread_mutex_unlock (&pool_mutex);

  chunk_alloc[0] = eolbyte;
  char *chunk_data = chunk_alloc + 1;

  if (residue_len > 0)
    memcpy (chunk_data, residue_buf, residue_len);

  size_t total_in_buf = residue_len;
  size_t slice_cap = SLICE_BUF_SIZE;
  bool is_eof = false;
  int read_err = 0;

  while (total_in_buf < slice_cap)
    {
      ssize_t nread = safe_read (STDIN_FILENO, chunk_data + total_in_buf,
                                 slice_cap - total_in_buf);
      if (nread <= 0)
        {
          if (nread < 0)
            read_err = errno;
          is_eof = true;
          break;
        }
      total_in_buf += nread;
    }

  while (!is_eof && memrchr (chunk_data, eolbyte, total_in_buf) == nullptr)
    {
      slice_cap *= 2;
      chunk_alloc = grow_slice_buf (chunk_alloc, total_in_buf, slice_cap);
      chunk_data = chunk_alloc + 1;
      while (total_in_buf < slice_cap)
        {
          ssize_t nread = safe_read (STDIN_FILENO, chunk_data + total_in_buf,
                                     slice_cap - total_in_buf);
          if (nread <= 0)
            {
              if (nread < 0)
                read_err = errno;
              is_eof = true;
              break;
            }
          size_t old_pos = total_in_buf;
          total_in_buf += nread;
          if (memchr (chunk_data + old_pos, eolbyte, (size_t) nread) != nullptr)
            break;
        }
    }

  *total_in_buf_out = total_in_buf;
  *is_eof_out = is_eof;
  *read_err_out = read_err;
  return chunk_alloc;
}

/* Enqueue a line-aligned stdin slice [CHUNK_DATA, CHUNK_DATA + CHUNK_LEN)
   into the ROB and worker queue.  Return false if the pool terminated
   while waiting for queue space.  */
static inline _GL_ATTRIBUTE_ALWAYS_INLINE bool
stream_enqueue_slice_task (char *chunk_alloc, char *chunk_data,
                           size_t chunk_len, intmax_t stream_totalcc,
                           bool chunk_is_last)
{
  chunk_data[chunk_len] = eolbyte;
  memset (chunk_data + chunk_len + 1, 0, uword_size);

  GREP_TEST_DELAY ("slice_enqueue");
  pthread_mutex_lock (&pool_mutex);

  while (next_seq - commit_seq >= STREAM_MAX_INFLIGHT && !pool_terminate)
    {
      drain_queue_if_no_workers_locked ();
      rob_try_commit_locked ();
      if (next_seq - commit_seq < STREAM_MAX_INFLIGHT)
        break;
      rob_space_waiters++;
      pthread_cond_wait (&rob_space_cv, &pool_mutex);
      rob_space_waiters--;
    }

  uint64_t seq = producer_alloc_slot_locked ();
  struct rob_slot *slot = &rob[seq % ROB_CAP];
  slot->is_chunk = true;
  slot->is_last_chunk = chunk_is_last;
  if (chunk_is_last)
    slot->filename = label ? xstrdup (label) : nullptr;

  if (!work_queue_wait_space_locked (1))
    {
      free_slice_buf_locked (chunk_alloc);
      slot->status = true;
      slot->state = ROB_SLOT_DONE;
      rob_try_commit_locked ();
      pthread_mutex_unlock (&pool_mutex);
      return false;
    }

  struct work_task t = {
    .type = TASK_TYPE_STREAM_CHUNK,
    .seq = seq,
    .filename = nullptr,
    .chunk_data = chunk_data,
    .chunk_len = chunk_len,
    .chunk_totalcc = stream_totalcc,
    .chunk_lineno_offset = 0,
    .chunk_free_data = true,
    .is_last_chunk = chunk_is_last,
    .shared_state = nullptr
  };
  work_queue_push_locked (&t);

  pthread_cond_signal (&q_not_empty_cv);
  pthread_mutex_unlock (&pool_mutex);
  return true;
}

static void
stream_slicer_stdin (void)
{
  producer_flush_batch ();
  if (pool_started)
    rob_wait_all_committed ();
  stream_readsize = ALIGN_TO (GOOD_READSIZE_MIN, pagesize);
#ifdef F_GETPIPE_SZ
  int pipe_size = fcntl (STDIN_FILENO, F_GETPIPE_SZ);
  if (0 < pipe_size)
    stream_readsize = MIN (stream_readsize, pipe_size);
#endif
#ifdef F_SETPIPE_SZ
  /* Fewer, larger reads keep the slices coming.  This changes how far
     the writer can get ahead of grep, which matters little here, as
     the slicer reads ahead anyway.  */
  fcntl (STDIN_FILENO, F_SETPIPE_SZ, 1024 * 1024);
#endif
  ensure_pool ();

  size_t residue_cap = 64 * 1024;
  char *residue_buf = xmalloc (residue_cap);
  size_t residue_len = 0;
  intmax_t stream_totalcc = 0;

  while (true)
    {
      size_t total_in_buf = 0;
      bool is_eof = false;
      int read_err = 0;
      char *chunk_alloc = stream_read_next_slice (residue_buf, residue_len,
                                                  &total_in_buf, &is_eof,
                                                  &read_err);
      char *chunk_data = chunk_alloc + 1;
      size_t chunk_len = 0;
      size_t next_residue = 0;

      if (read_err != 0)
        {
          char const *last_eol = (total_in_buf > 0
                                  ? memrchr (chunk_data, eolbyte, total_in_buf)
                                  : nullptr);
          chunk_len = last_eol ? (size_t) (last_eol + 1 - chunk_data) : 0;
          next_residue = 0;
        }
      else if (total_in_buf == 0)
        {
          pthread_mutex_lock (&pool_mutex);
          free_slice_buf_locked (chunk_alloc);
          pthread_mutex_unlock (&pool_mutex);
          rob_enqueue_empty_last_chunk (label);
          break;
        }
      else if (is_eof)
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
          chunk_len = last_eol + 1 - chunk_data;
          next_residue = total_in_buf - chunk_len;
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

      if (chunk_len > 0)
        {
          bool chunk_is_last = (is_eof && read_err == 0);
          if (!stream_enqueue_slice_task (chunk_alloc, chunk_data, chunk_len,
                                          stream_totalcc, chunk_is_last))
            break;
          stream_totalcc += chunk_len;
        }
      else
        {
          pthread_mutex_lock (&pool_mutex);
          free_slice_buf_locked (chunk_alloc);
          pthread_mutex_unlock (&pool_mutex);
        }

      if (read_err != 0)
        {
          if (!suppress_errors)
            producer_enqueue_error (read_err,
                                    label ? label : _("(standard input)"));
          else
            producer_enqueue_errseen ();
          rob_enqueue_empty_last_chunk (label);
        }

      if (is_eof)
        break;
    }

  free (residue_buf);
  if (pool_started)
    rob_wait_all_committed ();
}
