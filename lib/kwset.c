/* kwset.c - search for any of a set of keywords.
   Copyright (C) 1989, 1998, 2000, 2005, 2007, 2009-2026 Free Software
   Foundation, Inc.
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

/* Written August 1989 by Mike Haertel.  */

/* For more on the Aho-Corasick and Boyer-Moore algorithms,
   as well as other algorithms that might help improve performance,
   see the grep manual's "Performance" chapter.  */

#include <config.h>

#include "kwset.h"

#include <ctype.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdint.h>
#include <sys/types.h>

#if defined __AVX2__
# include <immintrin.h>
#endif

#include "memchr2.h"
#include "minmax.h"
#include "obstack.h"
#include "xalloc.h"
#include "verify.h"

#define obstack_chunk_alloc xmalloc
#define obstack_chunk_free free

/* When we deliberately use duplicate branches, use this macro to
   disable gcc's -Wduplicated-branches in the containing expression.  */
#if 7 <= __GNUC__
# define IGNORE_DUPLICATE_BRANCH_WARNING(exp)                           \
  ({                                                                    \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wduplicated-branches\"")        \
    exp;                                                                \
    _Pragma ("GCC diagnostic pop")                                      \
  })
#else
# define IGNORE_DUPLICATE_BRANCH_WARNING(exp) exp
#endif

enum { NCHAR = UCHAR_MAX + 1 };

/* Convert a possibly-signed character to an unsigned character.  This is
   a bit safer than casting to unsigned char, since it catches some type
   errors that the cast doesn't.  */
static inline unsigned char
to_uchar (char ch)
{
  return ch;
}

static unsigned char
U (char ch)
{
  return to_uchar (ch);
}

/* Balanced tree of edges and labels leaving a given trie node.  */
struct tree
{
  struct tree *llink;           /* Left link; MUST be first field.  */
  struct tree *rlink;           /* Right link (to larger labels).  */
  struct trie *trie;            /* Trie node pointed to by this edge.  */
  unsigned char label;          /* Label on this edge.  */
  char balance;                 /* Difference in depths of subtrees.  */
};

/* Node of a trie representing a set of keywords.  */
struct trie
{
  /* If an accepting node, this is either 2*W + 1 where W is the word
     index, or is -1 if Aho-Corasick is in use and FAIL
     specifies where to look for more info.  If not an accepting node,
     this is zero.  */
  ptrdiff_t accepting;

  struct tree *links;           /* Tree of edges leaving this node.  */
  struct trie *parent;          /* Parent of this node.  */
  struct trie *next;            /* List of all trie nodes in level order.  */
  struct trie *fail;            /* Aho-Corasick failure function.  */
  idx_t depth;                  /* Depth of this node from the root.  */
  idx_t shift;                  /* Shift function for search failures.  */
  idx_t maxshift;               /* Max shift of self and descendants.  */
  uint32_t dfa_state;           /* State number of this node in the dense
                                   automaton, if any.  */
  unsigned char label;          /* Label on the edge from the parent.  */
};

/* Structure returned opaquely to the caller, containing everything.  */
struct kwset
{
  struct obstack obstack;       /* Obstack for node allocation.  */
  idx_t words;                  /* Number of words in the trie.  */
  idx_t nodes;                  /* Number of nodes in the trie.  */
  struct trie *trie;            /* The trie itself.  */
  idx_t mind;                   /* Minimum depth of an accepting node.  */
  unsigned char delta[NCHAR];   /* Delta table for rapid search.  */
  struct trie *next[NCHAR];     /* Table of children of the root.  */
  char *target;                 /* Target string if there's only one.  */
  idx_t *shift;                 /* Used in Boyer-Moore search for one
                                   string.  */
  char const *trans;            /* Character translation table.  */

  /* This helps to match a terminal byte, which is the first byte
     for Aho-Corasick, and the last byte for Boyer-More.  If all the
     patterns have the same terminal byte (after translation via TRANS
     if TRANS is nonnull), then this is that byte as an unsigned char.
     Otherwise this is -1 if there is disagreement among the strings
     about terminal bytes, and -2 if there are no terminal bytes and
     no disagreement because all the patterns are empty.  */
  int gc1;

  /* This helps to match a terminal byte.  If 0 <= GC1HELP, B is
     terminal when B == GC1 || B == GC1HELP (note that GC1 == GCHELP
     is common here).  This is typically faster than evaluating
     to_uchar (TRANS[B]) == GC1.  */
  int gc1help;

  /* If the string has two or more bytes, this is the penultimate byte,
     after translation via TRANS if TRANS is nonnull.  This variable
     is used only by Boyer-Moore.  */
  char gc2;

#if defined __AVX2__
  /* Single-pattern AVX2 packed-pair scanner. */
  bool simd_pair_ready;
  uint8_t simd_first_val;
  uint8_t simd_first_mask;
  uint8_t simd_last_val;
  uint8_t simd_last_mask;
  idx_t simd_last_offset;

  /* Multi-pattern AVX2 Teddy nibble filter. */
  bool teddy_active;
  uint8_t teddy_t0_lo[32];
  uint8_t teddy_t0_hi[32];
  uint8_t teddy_t1_lo[32];
  uint8_t teddy_t1_hi[32];
#endif

  /* Dense Aho-Corasick automaton for two or more keywords, or null.
     DFA_TRANS[S + DFA_CLASS[B]] is the state that follows state S on
     the text byte B.  A state is represented by the offset of its row
     in DFA_TRANS, i.e., by its number times DFA_NCLASSES.  The root is
     state 0, and the states at or above DFA_ACCEPT are exactly those
     whose trie nodes are accepting.  DFA_NODE[S / DFA_NCLASSES] is the
     trie node of state S.  */
  uint32_t *dfa_trans;
  struct trie const **dfa_node;
  uint32_t dfa_nclasses;
  uint32_t dfa_accept;
  unsigned char dfa_class[NCHAR];

  /* Sampling filter that runs ahead of the dense automaton, or null.
     Every keyword is at least QF_LEAD + QF_Q bytes long, and QF_BITS
     has the bit for the hash of every QF_Q-byte substring of the first
     QF_LEAD + QF_Q bytes of every keyword.  So an occurrence that
     starts in [P, P + QF_LEAD + 1) contains the QF_Q bytes at
     P + QF_LEAD, and if their bit is clear, no occurrence starts there.
     If QF_FOLD, bytes are folded by qf_fold_byte before hashing, so
     that bytes that TRANS maps to the same byte hash alike.  */
  uint64_t *qf_bits;
  uint64_t qf_qmask;            /* Selects the first QF_Q bytes of a load.  */
  idx_t qf_lead;
  int qf_q;
  int qf_shift;                 /* Hash shift: 64 - log2 (bits in QF_BITS).  */
  bool qf_fold;

  /* kwsexec implementation.  */
  ptrdiff_t (*kwsexec) (kwset_t, char const *, idx_t, struct kwsmatch *, bool);
};

/* Use TRANS to transliterate C.  A null TRANS does no transliteration.  */
static inline char
tr (char const *trans, char c)
{
  return trans ? trans[U(c)] : c;
}

static ptrdiff_t acexec (kwset_t, char const *, idx_t,
                         struct kwsmatch *, bool);
static ptrdiff_t acexec_dfa (kwset_t, char const *, idx_t,
                             struct kwsmatch *, bool);
static ptrdiff_t bmexec (kwset_t, char const *, idx_t,
                         struct kwsmatch *, bool);

kwset_t
kwsalloc (char const *trans)
{
  struct kwset *kwset = xmalloc (sizeof *kwset);

  obstack_init (&kwset->obstack);
  kwset->words = 0;
  kwset->nodes = 1;
  kwset->trie = obstack_alloc (&kwset->obstack, sizeof *kwset->trie);
  kwset->trie->accepting = 0;
  kwset->trie->links = nullptr;
  kwset->trie->parent = nullptr;
  kwset->trie->next = nullptr;
  kwset->trie->fail = nullptr;
  kwset->trie->depth = 0;
  kwset->trie->shift = 0;
  kwset->mind = IDX_MAX;
  kwset->target = nullptr;
  kwset->trans = trans;
  kwset->kwsexec = acexec;

#if defined __AVX2__
  kwset->simd_pair_ready = false;
  kwset->teddy_active = false;
#endif

  kwset->dfa_trans = nullptr;
  kwset->dfa_node = nullptr;
  kwset->qf_bits = nullptr;
  kwset->qf_fold = false;

  return kwset;
}

/* This upper bound is valid for CHAR_BIT >= 4 and
   exact for CHAR_BIT in { 4..11, 13, 15, 17, 19 }.  */
enum { DEPTH_SIZE = CHAR_BIT + CHAR_BIT / 2 };

void
kwsincr (kwset_t kwset, char const *text, idx_t len)
{
  assume (0 <= len);
  struct trie *trie = kwset->trie;
  char const *trans = kwset->trans;
  bool reverse = kwset->kwsexec == bmexec;

  if (reverse)
    text += len;

  /* Descend the trie (built of keywords) character-by-character,
     installing new nodes when necessary.  */
  while (len--)
    {
      unsigned char uc = reverse ? *--text : *text++;
      unsigned char label = trans ? trans[uc] : uc;

      /* Descend the tree of outgoing links for this trie node,
         looking for the current character and keeping track
         of the path followed.  */
      struct tree *cur = trie->links;
      struct tree *links[DEPTH_SIZE];
      enum { L, R } dirs[DEPTH_SIZE];
      links[0] = (struct tree *) &trie->links;
      dirs[0] = L;
      idx_t depth = 1;

      while (cur && label != cur->label)
        {
          links[depth] = cur;
          if (label < cur->label)
            dirs[depth++] = L, cur = cur->llink;
          else
            dirs[depth++] = R, cur = cur->rlink;
        }

      /* The current character doesn't have an outgoing link at
         this trie node, so build a new trie node and install
         a link in the current trie node's tree.  */
      if (!cur)
        {
          cur = obstack_alloc (&kwset->obstack, sizeof *cur);
          cur->llink = nullptr;
          cur->rlink = nullptr;
          cur->trie = obstack_alloc (&kwset->obstack, sizeof *cur->trie);
          cur->trie->accepting = 0;
          cur->trie->links = nullptr;
          cur->trie->parent = trie;
          cur->trie->next = nullptr;
          cur->trie->fail = nullptr;
          cur->trie->depth = trie->depth + 1;
          cur->trie->shift = 0;
          cur->trie->label = label;
          cur->label = label;
          cur->balance = 0;
          kwset->nodes++;

          /* Install the new tree node in its parent.  */
          if (dirs[--depth] == L)
            links[depth]->llink = cur;
          else
            links[depth]->rlink = cur;

          /* Back up the tree fixing the balance flags.  */
          while (depth && !links[depth]->balance)
            {
              if (dirs[depth] == L)
                --links[depth]->balance;
              else
                ++links[depth]->balance;
              --depth;
            }

          /* Rebalance the tree by pointer rotations if necessary.  */
          if (depth && ((dirs[depth] == L && --links[depth]->balance)
                        || (dirs[depth] == R && ++links[depth]->balance)))
            {
              struct tree *t, *r, *l, *rl, *lr;

              switch (links[depth]->balance)
                {
                case (char) -2:
                  switch (dirs[depth + 1])
                    {
                    case L:
                      r = links[depth], t = r->llink, rl = t->rlink;
                      t->rlink = r, r->llink = rl;
                      t->balance = r->balance = 0;
                      break;
                    case R:
                      r = links[depth], l = r->llink, t = l->rlink;
                      rl = t->rlink, lr = t->llink;
                      t->llink = l, l->rlink = lr, t->rlink = r, r->llink = rl;
                      l->balance = t->balance != 1 ? 0 : -1;
                      r->balance = t->balance != (char) -1 ? 0 : 1;
                      t->balance = 0;
                      break;
                    default:
                      abort ();
                    }
                  break;
                case 2:
                  switch (dirs[depth + 1])
                    {
                    case R:
                      l = links[depth], t = l->rlink, lr = t->llink;
                      t->llink = l, l->rlink = lr;
                      t->balance = l->balance = 0;
                      break;
                    case L:
                      l = links[depth], r = l->rlink, t = r->llink;
                      lr = t->llink, rl = t->rlink;
                      t->llink = l, l->rlink = lr, t->rlink = r, r->llink = rl;
                      l->balance = t->balance != 1 ? 0 : -1;
                      r->balance = t->balance != (char) -1 ? 0 : 1;
                      t->balance = 0;
                      break;
                    default:
                      abort ();
                    }
                  break;
                default:
                  abort ();
                }

              if (dirs[depth - 1] == L)
                links[depth - 1]->llink = t;
              else
                links[depth - 1]->rlink = t;
            }
        }

      trie = cur->trie;
    }

  /* Mark the node finally reached as accepting, encoding the
     index number of this word in the keyword set so far.  */
  if (!trie->accepting)
    trie->accepting = 2 * kwset->words + 1;
  ++kwset->words;

  /* Keep track of the longest and shortest string of the keyword set.  */
  if (trie->depth < kwset->mind)
    kwset->mind = trie->depth;
}

idx_t
kwswords (kwset_t kwset)
{
  return kwset->words;
}

/* Enqueue the trie nodes referenced from the given tree in the
   given queue.  */
static void
enqueue (struct tree *tree, struct trie **last)
{
  if (!tree)
    return;
  enqueue (tree->llink, last);
  enqueue (tree->rlink, last);
  (*last) = (*last)->next = tree->trie;
}

/* Compute the Aho-Corasick failure function for the trie nodes referenced
   from the given tree, given the failure function for their parent as
   well as a last resort failure node.  */
static void
treefails (struct tree const *tree, struct trie const *fail,
           struct trie *recourse, bool reverse)
{
  if (!tree)
    return;

  treefails (tree->llink, fail, recourse, reverse);
  treefails (tree->rlink, fail, recourse, reverse);

  /* Find, in the chain of fails going back to the root, the first
     node that has a descendant on the current label.  */
  while (fail)
    {
      struct tree *cur = fail->links;
      while (cur && tree->label != cur->label)
        if (tree->label < cur->label)
          cur = cur->llink;
        else
          cur = cur->rlink;
      if (cur)
        {
          tree->trie->fail = cur->trie;
          if (!reverse && cur->trie->accepting && !tree->trie->accepting)
            tree->trie->accepting = -1;
          return;
        }
      fail = fail->fail;
    }

  tree->trie->fail = recourse;
}

/* Set delta entries for the links of the given tree such that
   the preexisting delta value is larger than the current depth.  */
static void
treedelta (struct tree const *tree, idx_t depth, unsigned char delta[])
{
  if (!tree)
    return;
  treedelta (tree->llink, depth, delta);
  treedelta (tree->rlink, depth, delta);
  if (depth < delta[tree->label])
    delta[tree->label] = depth;
}

/* Return true if A has every label in B.  */
static bool _GL_ATTRIBUTE_PURE
hasevery (struct tree const *a, struct tree const *b)
{
  if (!b)
    return true;
  if (!hasevery (a, b->llink))
    return false;
  if (!hasevery (a, b->rlink))
    return false;
  while (a && b->label != a->label)
    if (b->label < a->label)
      a = a->llink;
    else
      a = a->rlink;
  return !!a;
}

/* Compute a vector, indexed by character code, of the trie nodes
   referenced from the given tree.  */
static void
treenext (struct tree const *tree, struct trie *next[])
{
  if (!tree)
    return;
  treenext (tree->llink, next);
  treenext (tree->rlink, next);
  next[tree->label] = tree->trie;
}

#if defined __AVX2__
struct prefix_collector {
  int count;
  unsigned char b0[32];
  unsigned char b1[32];
};

static void
collect_prefixes_l1 (struct tree const *node, unsigned char c0, struct prefix_collector *coll)
{
  if (!node || coll->count >= 32)
    return;
  collect_prefixes_l1 (node->llink, c0, coll);
  if (coll->count < 32)
    {
      coll->b0[coll->count] = c0;
      coll->b1[coll->count] = node->label;
      coll->count++;
    }
  collect_prefixes_l1 (node->rlink, c0, coll);
}

static void
collect_prefixes_l0 (struct tree const *node, struct prefix_collector *coll)
{
  if (!node || coll->count >= 32)
    return;
  collect_prefixes_l0 (node->llink, coll);
  if (node->trie && node->trie->links)
    collect_prefixes_l1 (node->trie->links, node->label, coll);
  collect_prefixes_l0 (node->rlink, coll);
}

static bool
compute_simd_byte_mask (char const *trans, uint8_t target_byte,
                        uint8_t *out_mask, uint8_t *out_val)
{
  if (!trans)
    {
      *out_mask = 0xFF;
      *out_val = target_byte;
      return true;
    }
  int count = 0;
  uint8_t m0 = 0, m1 = 0;
  for (int b = 0; b < NCHAR; b++)
    if ((uint8_t) trans[b] == target_byte)
      {
        if (count == 0) m0 = (uint8_t) b;
        else if (count == 1) m1 = (uint8_t) b;
        count++;
      }
  if (count == 1)
    {
      *out_mask = 0xFF;
      *out_val = m0;
      return true;
    }
  if (count == 2)
    {
      uint8_t diff = m0 ^ m1;
      if ((diff & (diff - 1)) == 0)
        {
          *out_mask = (uint8_t) ~diff;
          *out_val = (uint8_t) (m0 & ~diff);
          return true;
        }
    }
  return false;
}
#endif

#if defined __GNUC__ || defined __clang__
# define kws_unlikely(x) __builtin_expect (!!(x), 0)
#else
# define kws_unlikely(x) (x)
#endif

/* The dense automaton is not built if it would need more than this
   many transitions.  */
enum { DFA_MAX_ENTRIES = 1 << 27 };

/* The sampling filter loads this many bytes at a time, so its q-grams
   are at most this long.  */
enum { QF_LOAD = 8 };

/* The sampling filter is used only if every keyword is at least this
   long.  Shorter q-grams are not selective enough to pay off.  */
enum { QF_MIN_WINDOW = 4 };

/* The filter looks at no more than this many leading bytes of each
   keyword.  */
enum { QF_MAX_WINDOW = 64 };

/* Multiplier of the filter's hash function.  */
#define QF_MULT UINT64_C (0x9E3779B97F4A7C15)

/* Fold the byte B for the filter: drop bit 5 (0x20) of an ASCII byte,
   which maps an ASCII letter to its upper case, and map every other
   byte to 0x80.  */
static unsigned char
qf_fold_byte (unsigned char b)
{
  return b & 0x80 ? 0x80 : b & 0xDF;
}

/* Fold each of the bytes in X, as qf_fold_byte does.  */
static inline uint64_t
qf_fold (uint64_t x)
{
  uint64_t hi = x & UINT64_C (0x8080808080808080);
  uint64_t himask = hi | (hi - (hi >> 7));
  return (x & ~himask & UINT64_C (0xDFDFDFDFDFDFDFDF)) | hi;
}

/* Return the filter bit number for the QF_LOAD bytes in X, which have
   already been folded if necessary.  */
static inline uint64_t
qf_hash (uint64_t x, uint64_t qmask, int shift)
{
  return ((x & qmask) * QF_MULT) >> shift;
}

/* Set up the sampling filter of KWSET, if it is likely to help, by
   choosing its parameters and allocating its empty bitmap.  Return the
   length of the leading part of each keyword that the filter covers,
   or 0 if there is no filter.  */
static idx_t
qf_init (kwset_t kwset)
{
  char const *trans = kwset->trans;
  idx_t window = MIN (kwset->mind, QF_MAX_WINDOW);
  if (window < QF_MIN_WINDOW)
    return 0;

  /* With TRANS, fold bytes before hashing.  This works only if folding
     maps every byte to the same byte that its translation maps to.  */
  bool fold = !!trans;
  if (fold)
    for (int i = 0; i < NCHAR; i++)
      if (qf_fold_byte (i) != qf_fold_byte (U(trans[i])))
        return 0;

  int q = MIN (window, QF_LOAD);
  idx_t lead = window - q;

  /* Size the bitmap for about one bit set in 256.  Each keyword has at
     most LEAD + 1 q-grams in its window.  */
  idx_t nqgrams = MIN (kwset->words, kwset->nodes) * (lead + 1);
  int log2bits = 12;
  while (log2bits < 28 && ((idx_t) 1 << log2bits) < nqgrams * 256)
    log2bits++;

  unsigned char qmask_bytes[QF_LOAD] = { 0 };
  memset (qmask_bytes, 0xFF, q);
  memcpy (&kwset->qf_qmask, qmask_bytes, sizeof kwset->qf_qmask);
  kwset->qf_lead = lead;
  kwset->qf_q = q;
  kwset->qf_shift = 64 - log2bits;
  kwset->qf_fold = fold;
  kwset->qf_bits = xicalloc ((idx_t) 1 << (log2bits - 6),
                             sizeof *kwset->qf_bits);
  return window;
}

/* Set the filter bit of KWSET for the q-gram that ends at a trie node,
   given TAIL, the last QF_LOAD labels of the path to that node, with
   the last label in the most significant byte.  */
static void
qf_add (kwset_t kwset, uint64_t tail)
{
  unsigned char buf[QF_LOAD] = { 0 };
  for (int i = 0; i < kwset->qf_q; i++)
    buf[i] = tail >> (8 * (QF_LOAD - kwset->qf_q + i));
  uint64_t x;
  memcpy (&x, buf, sizeof x);
  uint64_t h = qf_hash (x, kwset->qf_qmask, kwset->qf_shift);
  kwset->qf_bits[h >> 6] |= (uint64_t) 1 << (h & 63);
}

/* Store into ROW the transitions along the edges in TREE, given the
   class of each label.  */
static void
dfa_set_children (struct tree const *tree, uint32_t *row,
                  unsigned char const label_class[], uint32_t nclasses)
{
  if (!tree)
    return;
  row[label_class[tree->label]] = tree->trie->dfa_state * nclasses;
  dfa_set_children (tree->llink, row, label_class, nclasses);
  dfa_set_children (tree->rlink, row, label_class, nclasses);
}

/* Build the dense automaton of KWSET, whose trie and failure function
   have been computed for Aho-Corasick and whose root is not accepting,
   and set up the sampling filter if it is likely to help.  Return true
   if successful, false if the automaton would be too large or if it
   might not track the trie search exactly.  */
static bool
kwsprep_dfa (kwset_t kwset)
{
  char const *trans = kwset->trans;

  /* The trie search translates a byte that starts a keyword twice, so
     the automaton is equivalent only if TRANS is idempotent.  */
  if (trans)
    for (int i = 0; i < NCHAR; i++)
      if (trans[U(trans[i])] != trans[i])
        return false;

  idx_t nstates = kwset->nodes;
  if (DFA_MAX_ENTRIES < nstates)
    return false;
  idx_t window = qf_init (kwset);
  uint64_t *tail = window ? xinmalloc (nstates, sizeof *tail) : nullptr;

  /* Number the states in level order, accepting states last.  Along
     the way, find the labels in use and set the filter bits of the
     q-grams of the leading WINDOW bytes of the keywords: a q-gram ends
     at each node at least QF_Q deep and at most WINDOW deep.  */
  struct trie const **node = xinmalloc (nstates, sizeof *node);
  bool used[NCHAR] = { false };
  idx_t nonaccepting = 0, accepting = nstates;
  for (struct trie *t = kwset->trie; t; t = t->next)
    {
      idx_t n = t->accepting ? --accepting : nonaccepting++;
      t->dfa_state = n;
      node[n] = t;
      if (t->parent)
        {
          used[t->label] = true;
          if (t->depth <= window)
            {
              unsigned char b = t->label;
              if (kwset->qf_fold)
                b = qf_fold_byte (b);
              tail[n] = ((tail[t->parent->dfa_state] >> 8)
                         | ((uint64_t) b << (8 * (QF_LOAD - 1))));
              if (kwset->qf_q <= t->depth)
                qf_add (kwset, tail[n]);
            }
        }
      else if (tail)
        tail[n] = 0;
    }
  free (tail);

  /* Bytes that label no edge share one class, which leads back to the
     root from every state; each label has a class of its own.  */
  bool some_unused = false;
  for (int b = 0; b < NCHAR; b++)
    some_unused |= !used[b];
  unsigned char label_class[NCHAR];
  uint32_t nclasses = some_unused;
  for (int b = 0; b < NCHAR; b++)
    label_class[b] = used[b] ? nclasses++ : 0;
  if (DFA_MAX_ENTRIES / nclasses < nstates)
    {
      free (node);
      free (kwset->qf_bits);
      kwset->qf_bits = nullptr;
      return false;
    }

  /* A state's row is that of its failure state, except along its own
     edges.  Level order computes each failure state's row first.  */
  uint32_t *dtrans = xinmalloc (nstates, nclasses * sizeof *dtrans);
  for (struct trie const *t = kwset->trie; t; t = t->next)
    {
      uint32_t *row = dtrans + t->dfa_state * nclasses;
      if (t->fail)
        memcpy (row, dtrans + t->fail->dfa_state * nclasses,
                nclasses * sizeof *row);
      else
        memset (row, 0, nclasses * sizeof *row);
      dfa_set_children (t->links, row, label_class, nclasses);
    }

  for (int b = 0; b < NCHAR; b++)
    kwset->dfa_class[b] = label_class[trans ? U(trans[b]) : b];
  kwset->dfa_trans = dtrans;
  kwset->dfa_node = node;
  kwset->dfa_nclasses = nclasses;
  kwset->dfa_accept = nonaccepting * nclasses;
  return true;
}

void
kwsprep (kwset_t kwset)
{
  char const *trans = kwset->trans;
  unsigned char deltabuf[NCHAR];
  unsigned char *delta = trans ? deltabuf : kwset->delta;

  /* Use Boyer-Moore if just one pattern, Aho-Corasick otherwise.  */
  bool reverse = kwset->words == 1;

  if (reverse)
    {
      /* Enqueue the immediate descendants in the level order queue.  */
      {
        struct trie *last;
        for (struct trie *curr = last = kwset->trie; curr; curr = curr->next)
          enqueue (curr->links, &last);
      }

      /* Looking for just one string.  Extract it from the trie.  */
      kwset->target = obstack_alloc (&kwset->obstack, kwset->mind);
      struct trie *curr = kwset->trie;
      for (idx_t i = 0; i < kwset->mind; i++)
        {
          kwset->target[i] = curr->links->label;
          curr = curr->next;
        }

      kwset_t new_kwset = kwsalloc (kwset->trans);
      new_kwset->kwsexec = bmexec;
      kwsincr (new_kwset, kwset->target, kwset->mind);
      obstack_free (&kwset->obstack, nullptr);
      *kwset = *new_kwset;
      free (new_kwset);
    }

  /* Initial values for the delta table; will be changed later.  The
     delta entry for a given character is the smallest depth of any
     node at which an outgoing edge is labeled by that character.  */
  memset (delta, MIN (kwset->mind, UCHAR_MAX), sizeof deltabuf);

  /* Traverse the nodes of the trie in level order, simultaneously
     computing the delta table, failure function, and shift function.  */
  {
    struct trie *last;
    for (struct trie *curr = last = kwset->trie; curr; curr = curr->next)
      {
        /* Enqueue the immediate descendants in the level order queue.  */
        enqueue (curr->links, &last);

        /* Update the delta table for the descendants of this node.  */
        treedelta (curr->links, curr->depth, delta);

        /* Compute the failure function for the descendants of this node.  */
        treefails (curr->links, curr->fail, kwset->trie, reverse);

        if (reverse)
          {
            curr->shift = kwset->mind;
            curr->maxshift = kwset->mind;

            /* Update the shifts at each node in the current node's chain
               of fails back to the root.  */
            for (struct trie *fail = curr->fail; fail; fail = fail->fail)
              {
                /* If the current node has some outgoing edge that the fail
                   doesn't, then the shift at the fail should be no larger
                   than the difference of their depths.  */
                if (!hasevery (fail->links, curr->links))
                  if (curr->depth - fail->depth < fail->shift)
                    fail->shift = curr->depth - fail->depth;

                /* If the current node is accepting then the shift at the
                   fail and its descendants should be no larger than the
                   difference of their depths.  */
                if (curr->accepting && fail->maxshift > curr->depth - fail->depth)
                  fail->maxshift = curr->depth - fail->depth;
              }
          }
      }
  }

  if (reverse)
    {
      /* Traverse the trie in level order again, fixing up all nodes whose
         shift exceeds their inherited maxshift.  */
      for (struct trie *curr = kwset->trie->next; curr; curr = curr->next)
        {
          if (curr->maxshift > curr->parent->maxshift)
            curr->maxshift = curr->parent->maxshift;
          if (curr->shift > curr->maxshift)
            curr->shift = curr->maxshift;
        }
    }

  /* Create a vector, indexed by character code, of the outgoing links
     from the root node.  Accumulate GC1 and GC1HELP.  */
  {
    struct trie *nextbuf[NCHAR];
    struct trie **next = trans ? nextbuf : kwset->next;
    memset (next, 0, sizeof nextbuf);
    treenext (kwset->trie->links, next);
    int gc1 = -2;
    int gc1help = -1;
    for (int i = 0; i < NCHAR; i++)
      {
        int ti = i;
        if (trans)
          {
            ti = U(trans[i]);
            kwset->next[i] = next[ti];
          }
        if (kwset->next[i])
          {
            if (gc1 < -1)
              {
                gc1 = ti;
                gc1help = i;
              }
            else if (gc1 == ti)
              gc1help = gc1help == ti ? i : -1;
            else if (i == ti && gc1 == gc1help)
              gc1help = i;
            else
              gc1 = -1;
          }
      }
    kwset->gc1 = gc1;
    kwset->gc1help = gc1help;
  }

  if (reverse)
    {
      /* Looking for just one string.  Extract it from the trie.  */
      kwset->target = obstack_alloc (&kwset->obstack, kwset->mind);
      {
        struct trie *curr = kwset->trie;
        for (idx_t i = kwset->mind; 0 < i; i--)
          {
            kwset->target[i - 1] = curr->links->label;
            curr = curr->next;
          }
      }

      if (kwset->mind > 1)
        {
          /* Looking for the delta2 shift that might be made after a
             backwards match has failed.  Extract it from the trie.  */
          kwset->shift
            = obstack_alloc (&kwset->obstack,
                             sizeof *kwset->shift * (kwset->mind - 1));
          {
            struct trie *curr = kwset->trie->next;
            for (idx_t i = 0; i < kwset->mind - 1; i++)
              {
                kwset->shift[i] = curr->shift;
                curr = curr->next;
              }
          }

          /* The penultimate byte.  */
          kwset->gc2 = tr (trans, kwset->target[kwset->mind - 2]);
        }
    }

  /* Fix things up for any translation table.  */
  if (trans)
    for (int i = 0; i < NCHAR; ++i)
      kwset->delta[i] = delta[U(trans[i])];

#if defined __AVX2__
  if (reverse && kwset->mind >= 2)
    {
      idx_t len = kwset->mind;
      uint8_t c0 = (uint8_t) kwset->target[0];
      uint8_t c1 = (uint8_t) kwset->target[len - 1];

      if (compute_simd_byte_mask (trans, c0, &kwset->simd_first_mask, &kwset->simd_first_val)
          && compute_simd_byte_mask (trans, c1, &kwset->simd_last_mask, &kwset->simd_last_val))
        {
          kwset->simd_last_offset = len - 1;
          kwset->simd_pair_ready = true;
        }
    }
  else if (!reverse && kwset->words <= 32 && kwset->mind >= 2)
    {
      struct prefix_collector coll = {0};
      collect_prefixes_l0 (kwset->trie->links, &coll);
      if (coll.count > 0)
        {
          uint8_t t0_lo[16] = {0};
          uint8_t t0_hi[16] = {0};
          uint8_t t1_lo[16] = {0};
          uint8_t t1_hi[16] = {0};

          for (int p = 0; p < coll.count; p++)
            {
              int bit = 1 << (p % 8);
              unsigned char b0 = coll.b0[p];
              unsigned char b1 = coll.b1[p];

              for (int b = 0; b < NCHAR; b++)
                {
                  unsigned char tb = trans ? U(trans[b]) : (unsigned char) b;
                  if (tb == b0)
                    {
                      t0_lo[b & 0x0F] |= bit;
                      t0_hi[(b >> 4) & 0x0F] |= bit;
                    }
                  if (tb == b1)
                    {
                      t1_lo[b & 0x0F] |= bit;
                      t1_hi[(b >> 4) & 0x0F] |= bit;
                    }
                }
            }

          memcpy (kwset->teddy_t0_lo, t0_lo, 16);
          memcpy (kwset->teddy_t0_lo + 16, t0_lo, 16);
          memcpy (kwset->teddy_t0_hi, t0_hi, 16);
          memcpy (kwset->teddy_t0_hi + 16, t0_hi, 16);
          memcpy (kwset->teddy_t1_lo, t1_lo, 16);
          memcpy (kwset->teddy_t1_lo + 16, t1_lo, 16);
          memcpy (kwset->teddy_t1_hi, t1_hi, 16);
          memcpy (kwset->teddy_t1_hi + 16, t1_hi, 16);
          kwset->teddy_active = true;
        }
    }
#endif

  /* For two or more keywords, search with a dense automaton, behind a
     sampling filter when the keywords are long enough.  */
  if (!reverse && 0 < kwset->mind
#if defined __AVX2__
      && !kwset->teddy_active
#endif
      && kwsprep_dfa (kwset))
    kwset->kwsexec = acexec_dfa;
}

/* Delta2 portion of a Boyer-Moore search.  *TP is the string text
   pointer; it is updated in place.  EP is the end of the string text,
   and SP the end of the pattern.  LEN is the pattern length; it must
   be at least 2.  TRANS, if nonnull, is the input translation table.
   GC1 and GC2 are the last and second-from last bytes of the pattern,
   transliterated by TRANS; the caller precomputes them for
   efficiency.  If D1 is nonnull, it is a delta1 table for shifting *TP
   when failing.  KWSET->shift says how much to shift.  */
static inline bool
bm_delta2_search (char const **tpp, char const *ep, char const *sp,
                  idx_t len,
                  char const *trans, char gc1, char gc2,
                  unsigned char const *d1, kwset_t kwset)
{
  char const *tp = *tpp;
  idx_t d = len, skip = 0;

  while (true)
    {
      idx_t i = 2;
      if (tr (trans, tp[-2]) == gc2)
        {
          while (++i <= d)
            if (tr (trans, tp[-i]) != tr (trans, sp[-i]))
              break;
          if (i > d)
            {
              for (i = d + skip + 1; i <= len; ++i)
                if (tr (trans, tp[-i]) != tr (trans, sp[-i]))
                  break;
              if (i > len)
                {
                  *tpp = tp - len;
                  return true;
                }
            }
        }

      tp += d = kwset->shift[i - 2];
      if (tp > ep)
        break;
      if (tr (trans, tp[-1]) != gc1)
        {
          if (d1)
            tp += d1[U(tp[-1])];
          break;
        }
      skip = i - 1;
    }

  *tpp = tp;
  return false;
}

/* Return the address of the first byte in the buffer S (of size N)
   that matches the terminal byte specified by KWSET, or null if there
   is no match.  KWSET->gc1 should be nonnegative.  */
static char const *
memchr_kwset (char const *s, idx_t n, kwset_t kwset)
{
  char const *slim = s + n;
  if (kwset->gc1help < 0)
    {
      for (; s < slim; s++)
        if (kwset->next[U(*s)])
          return s;
    }
  else
    {
      int small_heuristic = 2;
      idx_t small_bytes = small_heuristic * sizeof (unsigned long int);
      while (s < slim)
        {
          if (kwset->next[U(*s)])
            return s;
          s++;
          if ((uintptr_t) s % small_bytes == 0)
            return memchr2 (s, kwset->gc1, kwset->gc1help, slim - s);
        }
    }
  return nullptr;
}

/* Fast Boyer-Moore search (scalar version).  */
static inline ptrdiff_t _GL_ATTRIBUTE_PURE
bmexec_trans_scalar (kwset_t kwset, char const *text, idx_t size)
{
  assume (0 <= size);

  idx_t len = kwset->mind;
  if (len == 0)
    return 0;
  if (len > size)
    return -1;
  if (len == 1)
    {
      char const *tp = memchr_kwset (text, size, kwset);
      return tp ? tp - text : -1;
    }

  char const *trans = kwset->trans;

  unsigned char const *d1 = kwset->delta;
  char const *sp = kwset->target + len;
  char const *tp = text + len;
  char gc1 = kwset->gc1;
  char gc2 = kwset->gc2;

  /* Significance of 12: 1 (initial offset) + 10 (skip loop) + 1 (md2).  */
  idx_t len12;
  if (!ckd_mul (&len12, len, 12) && len12 < size)
    /* 11 is not a bug, the initial offset happens only once.  */
    for (char const *ep = text + size - 11 * len; tp <= ep; )
      {
        char const *tp0 = tp;
        int d;
        d = d1[U(tp[-1])], tp += d;
        d = d1[U(tp[-1])], tp += d;
        if (d != 0)
          {
            d = d1[U(tp[-1])], tp += d;
            d = d1[U(tp[-1])], tp += d;
            d = d1[U(tp[-1])], tp += d;
            if (d != 0)
              {
                d = d1[U(tp[-1])], tp += d;
                d = d1[U(tp[-1])], tp += d;
                d = d1[U(tp[-1])], tp += d;
                if (d != 0)
                  {
                    d = d1[U(tp[-1])], tp += d;
                    d = d1[U(tp[-1])], tp += d;

                    /* As a heuristic, prefer memchr to seeking by
                       delta1 when the latter doesn't advance much.  */
                    int advance_heuristic = 16 * sizeof (long);
                    if (advance_heuristic <= tp - tp0)
                      continue;
                    tp--;
                    tp = memchr_kwset (tp, text + size - tp, kwset);
                    if (! tp)
                      return -1;
                    tp++;
                    if (ep <= tp)
                      break;
                  }
              }
          }
        if (bm_delta2_search (&tp, ep, sp, len, trans, gc1, gc2, d1, kwset))
          return tp - text;
      }

  /* Now only a few characters are left to search.  Carefully avoid
     ever producing an out-of-bounds pointer.  */
  char const *ep = text + size;
  int d = d1[U(tp[-1])];
  while (d <= ep - tp)
    {
      d = d1[U((tp += d)[-1])];
      if (d != 0)
        continue;
      if (bm_delta2_search (&tp, ep, sp, len, trans, gc1, gc2, nullptr, kwset))
        return tp - text;
    }

  return -1;
}

/* Fast Boyer-Moore search (inlinable version with AVX2 fast path).  */
static inline ptrdiff_t _GL_ATTRIBUTE_PURE
bmexec_trans (kwset_t kwset, char const *text, idx_t size)
{
  assume (0 <= size);

  idx_t len = kwset->mind;
  if (len == 0)
    return 0;
  if (len > size)
    return -1;
  if (len == 1)
    {
      char const *tp = memchr_kwset (text, size, kwset);
      return tp ? tp - text : -1;
    }

#if defined __AVX2__
  if (kwset->simd_pair_ready && size >= 64 + len)
    {
      __m256i f_mask = _mm256_set1_epi8 (kwset->simd_first_mask);
      __m256i f_val  = _mm256_set1_epi8 (kwset->simd_first_val);
      __m256i l_mask = _mm256_set1_epi8 (kwset->simd_last_mask);
      __m256i l_val  = _mm256_set1_epi8 (kwset->simd_last_val);

      char const *p = text;
      char const *ep_simd = text + size - (64 + len);
      char const *target = kwset->target;
      char const *trans = kwset->trans;
      idx_t last_off = kwset->simd_last_offset;

      while (p <= ep_simd)
        {
          __m256i v_f0 = _mm256_loadu_si256 ((const __m256i *) (const void *) p);
          __m256i v_l0 = _mm256_loadu_si256 ((const __m256i *) (const void *) (p + last_off));
          __m256i eq_f0 = _mm256_cmpeq_epi8 (_mm256_and_si256 (v_f0, f_mask), f_val);
          __m256i eq_l0 = _mm256_cmpeq_epi8 (_mm256_and_si256 (v_l0, l_mask), l_val);
          uint32_t mask0 = (uint32_t) _mm256_movemask_epi8 (_mm256_and_si256 (eq_f0, eq_l0));

          __m256i v_f1 = _mm256_loadu_si256 ((const __m256i *) (const void *) (p + 32));
          __m256i v_l1 = _mm256_loadu_si256 ((const __m256i *) (const void *) (p + 32 + last_off));
          __m256i eq_f1 = _mm256_cmpeq_epi8 (_mm256_and_si256 (v_f1, f_mask), f_val);
          __m256i eq_l1 = _mm256_cmpeq_epi8 (_mm256_and_si256 (v_l1, l_mask), l_val);
          uint32_t mask1 = (uint32_t) _mm256_movemask_epi8 (_mm256_and_si256 (eq_f1, eq_l1));

          if (__builtin_expect ((mask0 | mask1) == 0, 1))
            {
              p += 64;
              continue;
            }

          while (mask0)
            {
              int idx = __builtin_ctz (mask0);
              char const *cand = p + idx;
              bool match = true;
              if (trans)
                {
                  for (idx_t j = 0; j < len; j++)
                    if (tr (trans, cand[j]) != target[j])
                      { match = false; break; }
                }
              else
                {
                  if (memcmp (cand, target, len) != 0)
                    match = false;
                }
              if (match)
                return cand - text;
              mask0 &= mask0 - 1;
            }

          while (mask1)
            {
              int idx = __builtin_ctz (mask1);
              char const *cand = p + 32 + idx;
              bool match = true;
              if (trans)
                {
                  for (idx_t j = 0; j < len; j++)
                    if (tr (trans, cand[j]) != target[j])
                      { match = false; break; }
                }
              else
                {
                  if (memcmp (cand, target, len) != 0)
                    match = false;
                }
              if (match)
                return cand - text;
              mask1 &= mask1 - 1;
            }

          p += 64;
        }

      idx_t off = p - text;
      ptrdiff_t ret = bmexec_trans_scalar (kwset, p, size - off);
      return ret >= 0 ? off + ret : -1;
    }
#endif

  return bmexec_trans_scalar (kwset, text, size);
}

/* Fast Boyer-Moore search.  */
static ptrdiff_t
bmexec (kwset_t kwset, char const *text, idx_t size,
        struct kwsmatch *kwsmatch, bool longest)
{
  /* Help the compiler inline in two ways, depending on whether
     kwset->trans is null.  */
  ptrdiff_t ret = (IGNORE_DUPLICATE_BRANCH_WARNING
                   (kwset->trans
                    ? bmexec_trans (kwset, text, size)
                    : bmexec_trans (kwset, text, size)));
  kwsmatch->index = 0;
  kwsmatch->offset = ret;
  kwsmatch->size = kwset->mind;
  return ret;
}

/* Report a match found by a multiple string search of TEXT, whose end
   is LIM.  TRIE is the accepting node that the Aho-Corasick search is
   in just before TP, and TRANS is KWSET->trans.  If LONGEST, look for
   the longest match that starts at the leftmost position.  Store
   details in *KWSMATCH and return the offset of the match.  */
static inline ptrdiff_t
ac_match (kwset_t kwset, char const *trans, char const *text,
          char const *lim, char const *tp, struct trie const *trie,
          bool longest, struct kwsmatch *kwsmatch)
{
  struct trie const *accept = trie;
  while (accept->accepting < 0)
    accept = accept->fail;
  char const *left = tp - accept->depth;

  /* Try left-most longest match.  */
  if (longest)
    {
      while (tp < lim)
        {
          unsigned char c = tr (trans, *tp++);

          struct tree const *tree;
          do
            {
              tree = trie->links;
              while (tree && c != tree->label)
                tree = c < tree->label ? tree->llink : tree->rlink;
            }
          while (!tree && (trie = trie->fail) && accept->depth <= trie->depth);

          if (!tree)
            break;
          trie = tree->trie;
          if (trie->accepting)
            {
              struct trie const *accept1 = trie;
              while (accept1->accepting < 0)
                accept1 = accept1->fail;
              char const *left1 = tp - accept1->depth;
              if (left1 <= left)
                {
                  left = left1;
                  accept = accept1;
                }
            }
        }
    }

  kwsmatch->index = accept->accepting >> 1;
  kwsmatch->offset = left - text;
  kwsmatch->size = accept->depth;

  return left - text;
}

/* Hairy multiple string search with the Aho-Corasick algorithm.
   (inlinable version)  */
static inline ptrdiff_t
acexec_trans (kwset_t kwset, char const *text, idx_t len,
              struct kwsmatch *kwsmatch, bool longest)
{
  /* Initialize register copies and look for easy ways out.  */
  if (len < kwset->mind)
    return -1;

  char const *trans = kwset->trans;
  struct trie const *trie = kwset->trie;
  char const *lim = text + len;
  char const *tp = text;

  if (!trie->accepting)
    {
      unsigned char c;
      int gc1 = kwset->gc1;

#if defined __AVX2__
      __m256i mask_0f, t0_lo, t0_hi, t1_lo, t1_hi;
      if (kwset->teddy_active)
        {
          mask_0f = _mm256_set1_epi8 (0x0F);
          t0_lo = _mm256_loadu_si256 ((const __m256i *) (const void *) kwset->teddy_t0_lo);
          t0_hi = _mm256_loadu_si256 ((const __m256i *) (const void *) kwset->teddy_t0_hi);
          t1_lo = _mm256_loadu_si256 ((const __m256i *) (const void *) kwset->teddy_t1_lo);
          t1_hi = _mm256_loadu_si256 ((const __m256i *) (const void *) kwset->teddy_t1_hi);
        }
#endif

      while (true)
        {
#if defined __AVX2__
          if (kwset->teddy_active && lim - tp >= 34)
            {
              while (lim - tp >= 34)
                {
                  __m256i v0 = _mm256_loadu_si256 ((const __m256i *) (const void *) tp);
                  __m256i v1 = _mm256_loadu_si256 ((const __m256i *) (const void *) (tp + 1));
                  __m256i v0_lo = _mm256_and_si256 (v0, mask_0f);
                  __m256i v0_hi = _mm256_and_si256 (_mm256_srli_epi16 (v0, 4), mask_0f);
                  __m256i v1_lo = _mm256_and_si256 (v1, mask_0f);
                  __m256i v1_hi = _mm256_and_si256 (_mm256_srli_epi16 (v1, 4), mask_0f);

                  __m256i m0 = _mm256_and_si256 (_mm256_shuffle_epi8 (t0_lo, v0_lo),
                                                 _mm256_shuffle_epi8 (t0_hi, v0_hi));
                  __m256i m1 = _mm256_and_si256 (_mm256_shuffle_epi8 (t1_lo, v1_lo),
                                                 _mm256_shuffle_epi8 (t1_hi, v1_hi));
                  __m256i match = _mm256_and_si256 (m0, m1);
                  if (_mm256_testz_si256 (match, match))
                    tp += 32;
                  else
                    {
                      uint32_t mask = ~(uint32_t) _mm256_movemask_epi8 (_mm256_cmpeq_epi8 (match, _mm256_setzero_si256 ()));
                      char const *base_tp = tp;
                      while (mask)
                        {
                          int bit_idx = __builtin_ctz (mask);
                          char const *cand = base_tp + bit_idx;
                          c = tr (trans, cand[0]);
                          trie = kwset->next[c];
                          if (trie)
                            {
                              if (trie->accepting)
                                {
                                  tp = cand + 1;
                                  goto match;
                                }
                              unsigned char c1 = tr (trans, cand[1]);
                              struct tree const *tree = trie->links;
                              while (tree && c1 != tree->label)
                                tree = c1 < tree->label ? tree->llink : tree->rlink;
                              if (tree)
                                {
                                  trie = tree->trie;
                                  tp = cand + 2;
                                  goto have_first_trie;
                                }
                            }
                          mask &= mask - 1;
                        }
                      tp = base_tp + 32;
                    }
                }
            }
#endif
          if (gc1 < 0)
            {
              while (! (trie = kwset->next[c = tr (trans, *tp++)]))
                if (tp >= lim)
                  return -1;
            }
          else
            {
              tp = memchr_kwset (tp, lim - tp, kwset);
              if (!tp)
                return -1;
              c = tr (trans, *tp++);
              trie = kwset->next[c];
            }

#if defined __AVX2__
        have_first_trie:
#endif
          while (true)
            {
              if (trie->accepting)
                goto match;
              if (tp >= lim)
                return -1;
              c = tr (trans, *tp++);

              struct tree const *tree;
              for (tree = trie->links; c != tree->label; )
                {
                  tree = c < tree->label ? tree->llink : tree->rlink;
                  if (! tree)
                    {
                      trie = trie->fail;
                      if (!trie)
                        {
                          trie = kwset->next[c];
                          if (trie)
                            goto have_trie;
                          if (tp >= lim)
                            return -1;
                          goto next_c;
                        }
                      if (trie->accepting)
                        {
                          --tp;
                          goto match;
                        }
                      tree = trie->links;
                    }
                }
              trie = tree->trie;
            have_trie:;
            }
        next_c:;
        }
    }

 match:
  return ac_match (kwset, trans, text, lim, tp, trie, longest, kwsmatch);
}

/* Hairy multiple string search with Aho-Corasick algorithm.  */
static ptrdiff_t
acexec (kwset_t kwset, char const *text, idx_t size,
        struct kwsmatch *kwsmatch, bool longest)
{
  assume (0 <= size);
  /* Help the compiler inline in two ways, depending on whether
     kwset->trans is null.  */
  return (IGNORE_DUPLICATE_BRANCH_WARNING
          (kwset->trans
           ? acexec_trans (kwset, text, size, kwsmatch, longest)
           : acexec_trans (kwset, text, size, kwsmatch, longest)));
}

/* Return true if the filter bit of the QF_LOAD bytes at P is set.  */
static inline bool
qf_probe (char const *p, uint64_t const *bits, uint64_t qmask, int shift,
          bool fold)
{
  uint64_t x;
  memcpy (&x, p, sizeof x);
  if (fold)
    x = qf_fold (x);
  uint64_t h = qf_hash (x, qmask, shift);
  return (bits[h >> 6] >> (h & 63)) & 1;
}

/* Multiple string search with the dense automaton.  This finds the
   same match as acexec: the automaton is in the same state as the
   Aho-Corasick search after every byte, except that the sampling
   filter lets it skip text where no keyword can start, and it may
   restart at the root at a point P after which no keyword can start
   before P; this cannot change the first accepting position, nor the
   longest keyword ending there.  (inlinable version)  */
static inline ptrdiff_t
acexec_dfa_fold (kwset_t kwset, char const *text, idx_t len,
                 struct kwsmatch *kwsmatch, bool longest, bool fold)
{
  if (len < kwset->mind)
    return -1;

  uint32_t const *dtrans = kwset->dfa_trans;
  unsigned char const *dclass = kwset->dfa_class;
  uint32_t accept = kwset->dfa_accept;
  char const *lim = text + len;
  idx_t i = 0;
  uint32_t s = 0;

  if (kwset->qf_bits)
    {
      uint64_t const *bits = kwset->qf_bits;
      uint64_t qmask = kwset->qf_qmask;
      int shift = kwset->qf_shift;
      idx_t lead = kwset->qf_lead;
      idx_t stride = lead + 1;

      /* No keyword starts before TEXT + ANCHOR.  */
      idx_t anchor = 0;

      while (true)
        {
          /* The sample at J tells whether a keyword can start
             in [J - LEAD, J - LEAD + STRIDE).  */
          idx_t j = anchor + lead;
          while (j + 3 * stride + QF_LOAD <= len)
            {
              bool h0 = qf_probe (text + j, bits, qmask, shift, fold);
              bool h1 = qf_probe (text + j + stride, bits, qmask, shift, fold);
              bool h2 = qf_probe (text + j + 2 * stride, bits, qmask, shift,
                                  fold);
              bool h3 = qf_probe (text + j + 3 * stride, bits, qmask, shift,
                                  fold);
              if (kws_unlikely (h0 | h1 | h2 | h3))
                {
                  j += (h0 ? 0 : h1 ? stride : h2 ? 2 * stride : 3 * stride);
                  goto verify;
                }
              j += 4 * stride;
            }
          while (j + QF_LOAD <= len)
            {
              if (qf_probe (text + j, bits, qmask, shift, fold))
                goto verify;
              j += stride;
            }

          /* Too little text is left to sample; search the rest.  */
          i = j - lead;
          break;

        verify:
          /* Run the automaton from the root at the start of the block
             until it finds a match, or shows that no keyword starts in
             the block, i.e., until the bytes consumed since the block
             start exceed the depth of the state by at least STRIDE.  */
          {
            idx_t goal = j - lead + stride;
            idx_t stop = goal;
            i = j - lead;
            s = 0;
            while (true)
              {
                idx_t e = MIN (stop, len);
                while (i < e)
                  {
                    s = dtrans[s + dclass[U(text[i++])]];
                    if (kws_unlikely (accept <= s))
                      goto found;
                  }
                if (i == len)
                  return -1;
                idx_t depth = kwset->dfa_node[s / kwset->dfa_nclasses]->depth;
                if (goal <= i - depth)
                  {
                    anchor = i - depth;
                    break;
                  }
                stop = goal + depth;
              }
          }
        }
    }

  /* Search the rest of the text from the root.  As the trie search
     does, skip bytes that leave the automaton at the root.  */
  while (i < len)
    {
      if (kwset->gc1 < 0)
        {
          while (!kwset->next[U(text[i])])
            if (++i == len)
              return -1;
        }
      else
        {
          char const *tp = memchr_kwset (text + i, len - i, kwset);
          if (!tp)
            return -1;
          i = tp - text;
        }

      s = 0;
      do
        {
          s = dtrans[s + dclass[U(text[i++])]];
          if (kws_unlikely (accept <= s))
            goto found;
        }
      while (s != 0 && i < len);
    }
  return -1;

 found:
  return ac_match (kwset, kwset->trans, text, lim, text + i,
                   kwset->dfa_node[s / kwset->dfa_nclasses], longest,
                   kwsmatch);
}

/* Multiple string search with the dense automaton.  */
static ptrdiff_t
acexec_dfa (kwset_t kwset, char const *text, idx_t size,
            struct kwsmatch *kwsmatch, bool longest)
{
  assume (0 <= size);
  /* Help the compiler inline in two ways, depending on whether the
     filter folds bytes.  */
  return (kwset->qf_fold
          ? acexec_dfa_fold (kwset, text, size, kwsmatch, longest, true)
          : acexec_dfa_fold (kwset, text, size, kwsmatch, longest, false));
}

ptrdiff_t
kwsexec (kwset_t kwset, char const *text, idx_t size,
         struct kwsmatch *kwsmatch, bool longest)
{
  return kwset->kwsexec (kwset, text, size, kwsmatch, longest);
}

void
kwsfree (kwset_t kwset)
{
  free (kwset->dfa_trans);
  free (kwset->dfa_node);
  free (kwset->qf_bits);
  obstack_free (&kwset->obstack, nullptr);
  free (kwset);
}
