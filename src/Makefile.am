## Process this file with automake to create Makefile.in
# Copyright 1997-1998, 2005-2026 Free Software Foundation, Inc.
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

LN = ln

AM_CFLAGS = $(WARN_CFLAGS) $(WERROR_CFLAGS) $(PCRE_CFLAGS) -pthread

# Tell the linker to omit references to unused shared libraries.
AM_LDFLAGS = $(IGNORE_UNUSED_LIBRARIES_CFLAGS) -pthread

bin_PROGRAMS = grep
bin_SCRIPTS = egrep fgrep
grep_SOURCES =					\
  dfasearch.c					\
  die.h						\
  grep.c					\
  kwsearch.c					\
  searchutils.c
if USE_PCRE
grep_SOURCES += pcresearch.c
endif

noinst_HEADERS = grep.h search.h system.h

# Test-only builds of grep, never installed.  grep-hooks honors
# GREP_DELAY_AT (see GREP_TEST_DELAY in grep.c).  grep-shrunk also uses
# tiny queues, buffers and batches, so that every path that waits for
# space or splits work runs often, and recruits and splits eagerly.
check_PROGRAMS = grep-hooks grep-shrunk
grep_hooks_SOURCES = $(grep_SOURCES)
grep_hooks_CPPFLAGS = $(AM_CPPFLAGS) -DGREP_TEST_HOOKS
grep_hooks_LDADD = $(grep_LDADD)
grep_shrunk_SOURCES = $(grep_SOURCES)
grep_shrunk_CPPFLAGS = $(AM_CPPFLAGS) -DGREP_TEST_HOOKS \
  -DGREP_ROB_CAP=8 -DGREP_WORK_QUEUE_CAP=4 \
  -DGREP_SLICE_BUF_SIZE=4096 -DGREP_SLICE_POOL_CAP=2 \
  -DGREP_PRODUCER_BATCH_SIZE=2 \
  -DGREP_CHUNK_FILE_MIN=131072 -DGREP_CHUNK_MIN_SIZE=16384 \
  -DGREP_CHUNK_PIECE=4096 \
  -DGREP_WALK_BATCH=2 -DGREP_WALK_SPLIT_SIZE=65536 \
  -DGREP_WALK_SPLIT_MIN_REM=2 -DGREP_WALK_READDIR_BATCH=8 \
  -DGREP_WALK_INODE_SORT_MIN=16 -DGREP_WALK_BUF_CAP=65536 \
  -DGREP_WALK_BIN_SCAN_MIN=262144 -DGREP_WALK_BIN_SLICE=65536 \
  -DWALK_RECRUIT_NS=200000 -DWALK_SPLIT_NS=200000 \
  -DWALK_SPLIT_FILE_NS=20000 -DWALK_FD_CAP_MAX=4
grep_shrunk_LDADD = $(grep_LDADD)

# Sometimes, the expansion of $(LIBINTL) includes -lc which may
# include modules defining variables like 'optind', so libgreputils.a
# must precede $(LIBINTL) in order to ensure we use GNU getopt.
# But libgreputils.a must also follow $(LIBINTL), since libintl uses
# replacement functions defined in libgreputils.a.
LDADD = \
  ../lib/libgreputils.a $(LIBINTL) ../lib/libgreputils.a \
  $(HARD_LOCALE_LIB) $(LIBC32CONV) \
  $(LIBSIGSEGV) $(LIBUNISTRING) $(MBRTOWC_LIB) $(SETLOCALE_NULL_LIB) \
  $(LIBTHREAD)

grep_LDADD = $(LDADD) $(PCRE_LIBS) $(LIBCSTACK)
localedir = $(datadir)/locale
AM_CPPFLAGS = -I$(top_builddir)/lib -I$(top_srcdir)/lib

EXTRA_DIST = egrep.sh

egrep fgrep: egrep.sh Makefile
	$(AM_V_GEN)grep=`echo grep | sed -e '$(transform)'` &&		\
	case $@ in egrep) option=-E;; fgrep) option=-F;; esac &&	\
	shell_does_substrings='set x/y && d=$${1##*/} && test "$$d" = y' && \
	if $(SHELL) -c "$$shell_does_substrings" 2>/dev/null; then	\
	  edit_substring='s,X,X,';					\
	else								\
	  edit_substring='s,\$${0##\*/},`expr "X$$0" : '\''X\\(.*\\)/'\''`,g'; \
	fi &&								\
	sed -e 's|[@]SHELL@|$(SHELL)|g'					\
	    -e "$$edit_substring"					\
	    -e "s|[@]grep@|$$grep|g"					\
	    -e "s|[@]option@|$$option|g" <$(srcdir)/egrep.sh >$@-t
	$(AM_V_at)chmod +x $@-t
	$(AM_V_at)mv $@-t $@

CLEANFILES = egrep fgrep *-t
