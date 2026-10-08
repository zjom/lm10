/*
 * dlist.h — single-header, generic, owning doubly linked list (C11)
 *
 * The list OWNS its items: every insert copies the item's bytes into a node
 * allocated by the list, and every removal frees that node. If your items own
 * resources themselves (heap strings, file handles, ...), pass a destructor to
 * dlist_init() and the list will call it whenever it discards an item.
 *
 * Usage
 * -----
 *   In exactly ONE .c file:
 *       #define DLIST_IMPLEMENTATION
 *       #include "dlist.h"
 *   Everywhere else just #include "dlist.h".
 *
 *   DList l;
 *   dlist_init(&l, sizeof(int), NULL);
 *   int x = 42;
 *   dlist_push_back(&l, &x);
 *   DLIST_FOREACH(&l, n) printf("%d\n", *DLIST_AT(n, int));
 *   dlist_clear(&l);
 *
 * Ownership rules
 * ---------------
 *   - push/insert copy the item in (shallow memcpy). After that the list owns
 *     whatever the bytes refer to; do not free it yourself.
 *   - pop/remove with a non-NULL `out` MOVE the item out: bytes are copied to
 *     `out`, the destructor is NOT called, and ownership passes to the caller.
 *   - pop/remove with `out == NULL`, and dlist_clear(), DESTROY the item
 *     (destructor is called, then the node is freed).
 *   - Pointers returned by dlist_data()/push/insert stay valid until that
 *     node is removed; the list never moves items.
 *
 * Not thread-safe. Allocation failures are reported (NULL / false), never
 * abort. Override allocation with DLIST_MALLOC / DLIST_FREE before including.
 */
#ifndef DLIST_H
#define DLIST_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*dlist_dtor_fn)(void* item);
typedef int (*dlist_cmp_fn)(const void* item, const void* key);

typedef struct DListNode {
  struct DListNode* prev;
  struct DListNode* next;
  max_align_t data[]; /* item storage, suitably aligned for any type */
} DListNode;

typedef struct DList {
  DListNode* head;
  DListNode* tail;
  size_t size;
  size_t elem_size;
  dlist_dtor_fn dtor; /* may be NULL */
} DList;

/* ---- lifecycle ---------------------------------------------------------- */
void dlist_init(DList* l, size_t elem_size, dlist_dtor_fn dtor);
void dlist_clear(DList* l); /* destroys all items; list stays usable */

/* ---- queries ------------------------------------------------------------ */
static inline size_t dlist_size(const DList* l) {
  return l->size;
}
static inline bool dlist_empty(const DList* l) {
  return l->size == 0;
}
static inline DListNode* dlist_head(const DList* l) {
  return l->head;
}
static inline DListNode* dlist_tail(const DList* l) {
  return l->tail;
}
static inline DListNode* dlist_next(const DListNode* n) {
  return n->next;
}
static inline DListNode* dlist_prev(const DListNode* n) {
  return n->prev;
}
static inline void* dlist_data(DListNode* n) {
  return (void*)n->data;
}

/* Front/back item pointers, or NULL if empty. */
void* dlist_front(const DList* l);
void* dlist_back(const DList* l);

/* ---- insertion (item is copied; NULL item => zero-initialised) ---------- */
/* Return a pointer to the stored copy, or NULL on allocation failure.       */
void* dlist_push_front(DList* l, const void* item);
void* dlist_push_back(DList* l, const void* item);
void* dlist_insert_before(DList* l, DListNode* pos, const void* item);
void* dlist_insert_after(DList* l, DListNode* pos, const void* item);

/* ---- removal ------------------------------------------------------------ */
/* out != NULL: move item to out (no dtor). out == NULL: destroy item.        */
/* pop_* return false if the list was empty.                                 */
bool dlist_pop_front(DList* l, void* out);
bool dlist_pop_back(DList* l, void* out);
/* Removes n; returns the node that followed it (handy while iterating).     */
DListNode* dlist_remove(DList* l, DListNode* n, void* out);

/* ---- algorithms --------------------------------------------------------- */
/* First node whose item compares equal (cmp == 0) to key, or NULL.          */
DListNode* dlist_find(const DList* l, const void* key, dlist_cmp_fn cmp);
void dlist_reverse(DList* l);
/* Stable merge sort, O(n log n), no allocation. cmp(a, b) like qsort.       */
void dlist_sort(DList* l, dlist_cmp_fn cmp);
/* Moves every node of src to the end of dst (same elem_size). src empties. */
bool dlist_splice_back(DList* dst, DList* src);

/* ---- convenience macros ------------------------------------------------- */
#define DLIST_AT(node, T) ((T*)dlist_data(node))

/* Iterate; do NOT remove `it` inside the body (use DLIST_FOREACH_SAFE). */
#define DLIST_FOREACH(l, it) for (DListNode* it = (l)->head; it; it = it->next)

#define DLIST_FOREACH_REV(l, it) \
  for (DListNode* it = (l)->tail; it; it = it->prev)

/* Safe to dlist_remove(l, it, ...) inside the body. */
#define DLIST_FOREACH_SAFE(l, it, tmp)                             \
  for (DListNode* it = (l)->head, *tmp = it ? it->next : NULL; it; \
       it = tmp, tmp = it ? it->next : NULL)

#ifdef __cplusplus
}
#endif
#endif /* DLIST_H */

/* ======================================================================== */
#ifdef DLIST_IMPLEMENTATION
#ifndef DLIST_IMPLEMENTATION_DONE
#define DLIST_IMPLEMENTATION_DONE

#include <stdint.h>
#include <string.h>

#ifndef DLIST_MALLOC
#include <stdlib.h>
#define DLIST_MALLOC(sz) malloc(sz)
#define DLIST_FREE(p) free(p)
#endif

static DListNode* dlist__new_node(const DList* l, const void* item) {
  if (l->elem_size > SIZE_MAX - sizeof(DListNode))
    return NULL;
  DListNode* n = (DListNode*)DLIST_MALLOC(sizeof(DListNode) + l->elem_size);
  if (!n)
    return NULL;
  n->prev = n->next = NULL;
  if (item)
    memcpy(n->data, item, l->elem_size);
  else
    memset(n->data, 0, l->elem_size);
  return n;
}

static void dlist__destroy_node(DList* l, DListNode* n) {
  if (l->dtor)
    l->dtor(n->data);
  DLIST_FREE(n);
}

static void dlist__unlink(DList* l, DListNode* n) {
  if (n->prev)
    n->prev->next = n->next;
  else
    l->head = n->next;
  if (n->next)
    n->next->prev = n->prev;
  else
    l->tail = n->prev;
  n->prev = n->next = NULL;
  l->size--;
}

/* Link n between a and b (either may be NULL). */
static void dlist__link(DList* l, DListNode* a, DListNode* n, DListNode* b) {
  n->prev = a;
  n->next = b;
  if (a)
    a->next = n;
  else
    l->head = n;
  if (b)
    b->prev = n;
  else
    l->tail = n;
  l->size++;
}

void dlist_init(DList* l, size_t elem_size, dlist_dtor_fn dtor) {
  l->head = l->tail = NULL;
  l->size = 0;
  l->elem_size = elem_size;
  l->dtor = dtor;
}

void dlist_clear(DList* l) {
  DListNode* n = l->head;
  while (n) {
    DListNode* next = n->next;
    dlist__destroy_node(l, n);
    n = next;
  }
  l->head = l->tail = NULL;
  l->size = 0;
}

void* dlist_front(const DList* l) {
  return l->head ? (void*)l->head->data : NULL;
}
void* dlist_back(const DList* l) {
  return l->tail ? (void*)l->tail->data : NULL;
}

void* dlist_push_front(DList* l, const void* item) {
  DListNode* n = dlist__new_node(l, item);
  if (!n)
    return NULL;
  dlist__link(l, NULL, n, l->head);
  return n->data;
}

void* dlist_push_back(DList* l, const void* item) {
  DListNode* n = dlist__new_node(l, item);
  if (!n)
    return NULL;
  dlist__link(l, l->tail, n, NULL);
  return n->data;
}

void* dlist_insert_before(DList* l, DListNode* pos, const void* item) {
  if (!pos)
    return dlist_push_back(l, item); /* "before end" */
  DListNode* n = dlist__new_node(l, item);
  if (!n)
    return NULL;
  dlist__link(l, pos->prev, n, pos);
  return n->data;
}

void* dlist_insert_after(DList* l, DListNode* pos, const void* item) {
  if (!pos)
    return dlist_push_front(l, item); /* "after before-begin" */
  DListNode* n = dlist__new_node(l, item);
  if (!n)
    return NULL;
  dlist__link(l, pos, n, pos->next);
  return n->data;
}

DListNode* dlist_remove(DList* l, DListNode* n, void* out) {
  DListNode* next = n->next;
  dlist__unlink(l, n);
  if (out) {
    memcpy(out, n->data, l->elem_size); /* move: caller now owns it */
    DLIST_FREE(n);
  } else {
    dlist__destroy_node(l, n);
  }
  return next;
}

bool dlist_pop_front(DList* l, void* out) {
  if (!l->head)
    return false;
  dlist_remove(l, l->head, out);
  return true;
}

bool dlist_pop_back(DList* l, void* out) {
  if (!l->tail)
    return false;
  dlist_remove(l, l->tail, out);
  return true;
}

DListNode* dlist_find(const DList* l, const void* key, dlist_cmp_fn cmp) {
  for (DListNode* n = l->head; n; n = n->next)
    if (cmp(n->data, key) == 0)
      return n;
  return NULL;
}

void dlist_reverse(DList* l) {
  DListNode* n = l->head;
  while (n) {
    DListNode* next = n->next;
    n->next = n->prev;
    n->prev = next;
    n = next;
  }
  DListNode* t = l->head;
  l->head = l->tail;
  l->tail = t;
}

/* Merge two null-terminated singly linked (via ->next) sorted runs. */
static DListNode* dlist__merge(DListNode* a, DListNode* b, dlist_cmp_fn cmp) {
  DListNode head, *t = &head;
  while (a && b) {
    if (cmp(b->data, a->data) < 0) {
      t->next = b;
      b = b->next;
    } else {
      t->next = a;
      a = a->next;
    } /* stable */
    t = t->next;
  }
  t->next = a ? a : b;
  return head.next;
}

void dlist_sort(DList* l, dlist_cmp_fn cmp) {
  if (l->size < 2)
    return;

  /* Bottom-up merge sort over ->next using a small array of runs. */
  DListNode* bins[64] = {0};
  DListNode* n = l->head;
  while (n) {
    DListNode* next = n->next;
    n->next = NULL;
    size_t i = 0;
    for (; i < 64 && bins[i]; i++) {
      n = dlist__merge(bins[i], n, cmp);
      bins[i] = NULL;
    }
    if (i == 64)
      i = 63;
    bins[i] = n;
    n = next;
  }
  DListNode* result = NULL;
  for (size_t i = 0; i < 64; i++)
    if (bins[i])
      result = result ? dlist__merge(bins[i], result, cmp) : bins[i];

  /* Rebuild prev links and tail. */
  l->head = result;
  DListNode* prev = NULL;
  for (n = result; n; n = n->next) {
    n->prev = prev;
    prev = n;
  }
  l->tail = prev;
}

bool dlist_splice_back(DList* dst, DList* src) {
  if (dst->elem_size != src->elem_size)
    return false;
  if (dst == src || !src->head)
    return true;
  if (dst->tail) {
    dst->tail->next = src->head;
    src->head->prev = dst->tail;
  } else {
    dst->head = src->head;
  }
  dst->tail = src->tail;
  dst->size += src->size;
  src->head = src->tail = NULL;
  src->size = 0;
  return true;
}

#endif /* DLIST_IMPLEMENTATION_DONE */
#endif /* DLIST_IMPLEMENTATION */
