/* Capacities and timings of the parallel engine of grep.
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

/* Only grep.c includes this file.  The installed grep uses the
   defaults below.  The test build grep-shrunk overrides every GREP_*
   and WALK_* macro here with the much smaller value noted next to it
   (see src/Makefile.am), so that the tests reach the paths that wait
   for space, split work or recruit threads with small inputs.
   grep-shrunk also defines WALK_FD_CAP_MAX, which walk_fd_cap in
   grep.c reads.  */

#ifndef GREP_TUNABLES_H
#define GREP_TUNABLES_H

/* Capacities of the work queue and of the reorder buffer (ROB).  */
#ifndef GREP_WORK_QUEUE_CAP
# define GREP_WORK_QUEUE_CAP 256          /* grep-shrunk: 4 */
#endif
#ifndef GREP_ROB_CAP
# define GREP_ROB_CAP 1024                /* grep-shrunk: 8 */
#endif
enum { WORK_QUEUE_CAP = GREP_WORK_QUEUE_CAP };
enum { ROB_CAP = GREP_ROB_CAP };

/* A file is searched in at most this many chunks.  All chunks of a file
   must fit in the work queue at once, and their ROB slots, which may
   not be committed before all chunks are searched, must leave room for
   the slots of other inputs.  */
enum { MAX_FILE_CHUNKS = MIN (64, MIN (WORK_QUEUE_CAP, ROB_CAP / 2)) };
static_assert (2 <= MAX_FILE_CHUNKS);

/* The stream slicer keeps at most this many slices in flight.  */
enum { STREAM_MAX_INFLIGHT = MIN (56, ROB_CAP / 2) };
static_assert (1 <= STREAM_MAX_INFLIGHT);

/* Regular files of at least CHUNK_FILE_MIN bytes are searched in
   chunks of at least CHUNK_MIN_SIZE bytes each.  */
#ifndef GREP_CHUNK_FILE_MIN
# define GREP_CHUNK_FILE_MIN (2 * 1024 * 1024) /* grep-shrunk: 128 KiB */
#endif
#ifndef GREP_CHUNK_MIN_SIZE
# define GREP_CHUNK_MIN_SIZE (2 * 1024 * 1024) /* grep-shrunk: 16 KiB */
#endif
enum { CHUNK_FILE_MIN = GREP_CHUNK_FILE_MIN };
enum { CHUNK_MIN_SIZE = GREP_CHUNK_MIN_SIZE };

/* Chunk tasks search in pieces of about this many bytes, so that the
   check for null bytes reads each piece while the search has left it in
   cache.  */
#ifndef GREP_CHUNK_PIECE
# define GREP_CHUNK_PIECE (128 * 1024)    /* grep-shrunk: 4 KiB */
#endif
enum { CHUNK_PIECE = GREP_CHUNK_PIECE };

/* The stream slicer reads standard input into buffers of
   SLICE_BUF_SIZE bytes, and keeps at most SLICE_POOL_CAP free buffers
   for reuse.  */
#ifndef GREP_SLICE_BUF_SIZE
# define GREP_SLICE_BUF_SIZE (1024 * 1024) /* grep-shrunk: 4 KiB */
#endif
#ifndef GREP_SLICE_POOL_CAP
# define GREP_SLICE_POOL_CAP 64           /* grep-shrunk: 2 */
#endif
enum { SLICE_BUF_SIZE = GREP_SLICE_BUF_SIZE };
enum { SLICE_POOL_CAP = GREP_SLICE_POOL_CAP };

/* The producer hands file operands to the work queue in batches of
   at most this many; see producer_flush_batch.  */
#ifndef GREP_PRODUCER_BATCH_SIZE
# define GREP_PRODUCER_BATCH_SIZE 32      /* grep-shrunk: 2 */
#endif
enum { PRODUCER_BATCH_SIZE = GREP_PRODUCER_BATCH_SIZE };

/* A task of the recursive walker searches at most this many files.  */
#ifndef GREP_WALK_BATCH
# define GREP_WALK_BATCH 32               /* grep-shrunk: 2 */
#endif
enum { WALK_BATCH = GREP_WALK_BATCH };

/* gnulib fts reads at most this many entries of a directory at a time,
   and sorts a batch by inode number if it has more than
   WALK_INODE_SORT_MIN entries.  */
#ifndef GREP_WALK_READDIR_BATCH
# define GREP_WALK_READDIR_BATCH 100000   /* grep-shrunk: 8 */
#endif
#ifndef GREP_WALK_INODE_SORT_MIN
# define GREP_WALK_INODE_SORT_MIN 10000   /* grep-shrunk: 16 */
#endif
enum { WALK_READDIR_BATCH = GREP_WALK_READDIR_BATCH };
enum { WALK_INODE_SORT_MIN = GREP_WALK_INODE_SORT_MIN };

/* A recursive search searches the files of a directory in batches.
   Before it searches a regular file of at least WALK_SPLIT_SIZE bytes,
   it lets other threads search the rest of the batch; see
   walk_split_rest.  */
#ifndef GREP_WALK_SPLIT_SIZE
# define GREP_WALK_SPLIT_SIZE (2 * 1024 * 1024) /* grep-shrunk: 64 KiB */
#endif
enum { WALK_SPLIT_SIZE = GREP_WALK_SPLIT_SIZE };
/* A recursive search scans a binary file of at least WALK_BIN_SCAN_MIN
   bytes with -q in slices of WALK_BIN_SLICE bytes that any thread may
   search; see walk_bin_scan.  */
#ifndef GREP_WALK_BIN_SCAN_MIN
# define GREP_WALK_BIN_SCAN_MIN (16 * 1024 * 1024) /* grep-shrunk: 256 KiB */
#endif
#ifndef GREP_WALK_BIN_SLICE
# define GREP_WALK_BIN_SLICE (4 * 1024 * 1024) /* grep-shrunk: 64 KiB */
#endif
enum { WALK_BIN_SCAN_MIN = GREP_WALK_BIN_SCAN_MIN };
enum { WALK_BIN_SLICE = GREP_WALK_BIN_SLICE };

/* The walker starts no more tasks while at least WALK_BUF_CAP bytes
   of finished output wait to be written in order.  */
#ifndef GREP_WALK_BUF_CAP
# define GREP_WALK_BUF_CAP (256 * 1024 * 1024) /* grep-shrunk: 64 KiB */
#endif
enum { WALK_BUF_CAP = GREP_WALK_BUF_CAP };

/* The main thread traverses alone for this many nanoseconds before it
   recruits the worker threads.  Searches that end sooner, such as -q
   with an early match, then never start them.  */
#ifndef WALK_RECRUIT_NS
# define WALK_RECRUIT_NS (1000 * 1000)    /* grep-shrunk: 200 us */
#endif

/* A batch of files that has been searched for this many nanoseconds
   gives the second half of the files it has left to other threads, so
   that a batch of files that are slow to search is spread over the
   threads instead of keeping one of them busy while the others wait.  */
#ifndef WALK_SPLIT_NS
# define WALK_SPLIT_NS (3 * 1000 * 1000)  /* grep-shrunk: 200 us */
#endif
#ifndef WALK_SPLIT_FILE_NS
# define WALK_SPLIT_FILE_NS (500 * 1000)  /* grep-shrunk: 20 us */
#endif
#ifndef GREP_WALK_SPLIT_MIN_REM
# define GREP_WALK_SPLIT_MIN_REM 8        /* grep-shrunk: 2 */
#endif
enum { WALK_SPLIT_MIN_REM = GREP_WALK_SPLIT_MIN_REM };

#endif /* GREP_TUNABLES_H */
