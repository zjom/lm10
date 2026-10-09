/* ztd_map.h - single-header hash map: typed, insertion-ordered (C11)
 *
 * USAGE
 *   In exactly one .c file:
 *       #define ZTD_MAP_IMPLEMENTATION
 *       #include "ztd_map.h"
 *   Everywhere else just #include "ztd_map.h".
 *   Define ZTD_MAP_STATIC before including to make every function static.
 *   The implementation must be compiled as C; the declarations and macros
 *   also work from C++.
 *
 * PIECES
 *   ZTD_MAP(K, V) A hash map from K to V:
 *                     struct { struct { K key; V val; } *ptr; size_t len;
 *                              size_t cap; struct ZtdArena *arena;
 *                              bool failed; ...index fields... }
 *                 The entries live in one dense array, in insertion order
 *                 (until a removal moves the last entry into the hole), so
 *                 a map can be iterated as m.ptr[0 .. m.len). A separate
 *                 open-addressing index finds them. Operations are macros
 *                 taking a POINTER to the map. Declare a type once with a
 *                 typedef; two spellings of ZTD_MAP(int, int) are distinct
 *                 structs:
 *                     typedef ZTD_MAP(int, float) IntFloatMap;
 *
 *   ZTD_MAP_OF(E) The same, with an entry type you name, so you can declare
 *                 entry variables (ZTD_MAP_REMOVE_ENTRY) without typeof. E
 *                 must start with a member `key` and have a member `val`;
 *                 further members are zero-filled on insert:
 *                     typedef struct { const char *key; int val; } Pair;
 *                     typedef ZTD_MAP_OF(Pair) PairMap;
 *
 *   Keys          Hashed and compared by the map's hash/eq functions. The
 *                 default (ZTD_MAP_INIT) is bytewise over sizeof(K): right
 *                 for integers, pointers, enums and structs WITHOUT padding.
 *                 For anything else pass functions with ZTD_MAP_INIT_FN(h, eq):
 *                     uint64_t h(const void *key, size_t key_size);
 *                     bool eq(const void *a, const void *b, size_t ksize);
 *                 ZTD_MAP_INIT_CSTR compares `const char *` keys by contents.
 *                 The map stores keys by value: for string keys it holds the
 *                 pointer, and the characters must outlive the entry.
 *                 Floating-point keys need custom functions too: bytewise,
 *                 -0.0 and 0.0 are different keys.
 *
 *   ZtdArena      With ztd_arena.h (included before or after this header, in
 *                 any order) you get arena-backed maps:
 *                 ZTD_MAP_INIT_ARENA(&a), ZTD_MAP_INIT_FN_ARENA(&a, h, eq),
 *                 ZTD_MAP_INIT_CSTR_ARENA(&a).
 *                 ZTD_MAP_FREE then leaves the memory to the arena. The
 *                 implementation file must include ztd_arena.h too; otherwise
 *                 these fail to link.
 *
 *   ZtdStr        With ztd_str.h (in any order) you get ZtdStr keys:
 *                 ZTD_MAP_INIT_STR / ZTD_MAP_INIT_STR_ARENA(&a). Keys are
 *                 views; copy them somewhere that outlives the map (e.g.
 *                 ztd_str_arena_dup) if the text is transient.
 *
 *   ztd_arr.h     Entries are a .ptr/.len array, so ZTD_ARR_* helpers work on
 *                 a map. The key is the entry's first member, so a qsort
 *                 comparator for K sorts entries by key; call
 *                 ZTD_MAP_REINDEX after reordering:
 *                     ARR_SORT(&m, cmp_int);  MAP_REINDEX(&m);
 *                 Only use ZTD_ARR_* on a map, never the growing or
 *                 removing ZTD_LIST_* operations: they don't know about the
 *                 index or the scratch entry.
 *
 * SHORT NAMES
 *   By default the header also defines unprefixed aliases:
 *       MAP(), MAP_INIT, MAP_INIT_FN(), MAP_INIT_CSTR, MAP_* for every
 *       ZTD_MAP_* macro, MapHashFn, MapEqFn, map_* for every ztd_map_*
 *       function.
 *   Functions are aliased with object-like macros, so they still work as
 *   function pointers. To get only the ztd_ names, define before including:
 *       ZTD_NO_SHORT_NAMES        for every ztd_* header, or
 *       ZTD_MAP_NO_SHORT_NAMES    for this header only.
 *   Configuration macros (ZTD_MAP_IMPLEMENTATION etc.) are always prefixed.
 *
 * QUICK TOUR (short names)
 *   typedef MAP(int, double) IntDoubleMap;
 *   IntDoubleMap m = MAP_INIT;
 *   MAP_PUT(&m, 7, 0.5);                      // insert or overwrite
 *   double *p = MAP_GET(&m, 7);               // NULL if absent
 *   double  d = MAP_GET_OR(&m, 8, -1.0);      // value or default
 *   if (MAP_HAS(&m, 7)) MAP_REMOVE(&m, 7);
 *   if (MAP_REMOVE_INTO(&m, 9, &d)) ...       // removed; its value is in d
 *   for (size_t i = 0; i < m.len; i++)
 *       printf("%d %g\n", m.ptr[i].key, m.ptr[i].val);
 *   if (m.failed) out_of_memory();
 *   MAP_FREE(&m);
 *
 *   typedef MAP(Str, int) WordCount;          // with ztd_str.h
 *   WordCount wc = MAP_INIT_STR;
 *   StrSplit it = str_split(text, STR(" "));
 *   for (Str w; str_split_next(&it, &w);) {
 *       int *n = MAP_AT(&wc, w);              // inserts 0 if absent
 *       if (n) ++*n;
 *   }
 *   MAP_FOREACH(e, &wc) printf(STR_FMT ": %d\n", STR_ARG(e->key), e->val);
 *
 * CONVENTIONS
 *   - Macros taking a map `m` may evaluate `m` more than once; pass a plain
 *     pointer expression. Keys and values are evaluated at most once. Lookups
 *     on an empty map (len == 0) do not evaluate the key at all.
 *   - The value argument may contain unparenthesised commas, as in
 *     MAP_PUT(&m, k, (Point){1, 2}); a compound-literal KEY must be wrapped
 *     in parentheses. Values may read the map (they are evaluated after the
 *     entry exists) but must not modify it.
 *   - Lookups share one scratch key and index per map, so a key expression
 *     must not look up the same map, and two lookups on the same map must
 *     not appear in one unsequenced expression: f(MAP_GET(&m, 1),
 *     MAP_GET(&m, 2)) is undefined. Separate them with ; , && || or ?:.
 *     For the same reason, concurrent lookups need external locking.
 *   - Lookups write the key into a scratch entry, so they need a non-const
 *     map (the observable contents do not change).
 *   - Allocation errors are sticky, as in ZTD_LIST: after a failure every
 *     further inserting operation returns false (or NULL) and leaves the map
 *     as it was, so you can do a run of puts and check m.failed once.
 *     Lookups and removals still work. ZTD_MAP_CLEAR() and ZTD_MAP_FREE()
 *     reset the flag.
 *   - Inserting can move the entries: it invalidates pointers into the map
 *     (including those from ZTD_MAP_GET / ZTD_MAP_AT) and entry indices are
 *     stable only until the next removal.
 *   - ZTD_MAP_REMOVE (and _INTO, _ENTRY) move the last entry into the hole;
 *     ZTD_MAP_REMOVE_ORDERED keeps the order but costs O(len). To remove while
 *     iterating, iterate backwards.
 *   - Do not change a key in place; to reorder entries, rewrite them and
 *     call ZTD_MAP_REINDEX. Keys must stay unique.
 *   - At most about 3 * 2^30 entries (the index stores 32-bit positions).
 *   - Hash values are not stable across platforms or versions; don't
 *     persist them.
 *   - Arena-backed maps leave their old storage in the arena when they grow,
 *     and must not be used after the arena is rewound past their latest
 *     growth, or reset.
 *
 * CONFIGURATION
 *   ZTD_MAP_ASSERT(x)    default ZTD_ASSERT(x) if defined, else assert(x)
 *   ZTD_MAP_MALLOC(sz) / ZTD_MAP_REALLOC(p, sz) / ZTD_MAP_DEALLOC(p)
 *                        backing allocator; define all three or none, before
 *                        including the implementation. Defaults to
 *                        ZTD_MALLOC / ZTD_REALLOC / ZTD_FREE if those are
 *                        defined, else the C library. (The free hook is not
 *                        called ZTD_MAP_FREE, which is the map macro.)
 *
 * Requires C11. LICENSE: public domain / MIT-0. Do what you like.
 */
#ifndef ZTD_MAP_H
#define ZTD_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ZTD_MAP_STATIC
#if defined(__GNUC__)
#define ZTD_MAP_DEF static __attribute__((unused))
#else
#define ZTD_MAP_DEF static
#endif
#else
#define ZTD_MAP_DEF extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types -------------------------------------------------------------- */

struct ZtdArena; /* defined by ztd_arena.h; only ever used by pointer here */

typedef uint64_t (*ZtdMapHashFn)(const void* key, size_t key_size);
typedef bool (*ZtdMapEqFn)(const void* a, const void* b, size_t key_size);

/* One index slot: idx is entry index + 1 (0 = empty). */
typedef struct ZtdMapSlot {
  uint32_t hash;
  uint32_t idx;
} ZtdMapSlot;

/* ptr[cap] is a scratch entry (the allocation holds cap + 1 entries). */
#define ZTD_MAP(K, V) \
  ZTD_MAP_OF(struct { \
    K key;            \
    V val;            \
  })
#define ZTD_MAP_OF(E)                                                   \
  struct {                                                              \
    E* ptr;                                                             \
    size_t len;                                                         \
    size_t cap;                                                         \
    struct ZtdArena* arena;                                             \
    bool failed;                                                        \
    ZtdMapSlot* slots; /* index table, nslots is 0 or a power of two */ \
    size_t nslots;                                                      \
    ZtdMapHashFn hash; /* NULL: bytewise */                             \
    ZtdMapEqFn eq;     /* NULL: bytewise */                             \
    size_t tmp_;       /* scratch index for the lookup macros */        \
    size_t ins_;       /* scratch index for the inserting macros */     \
  }

#define ZTD_MAP_NPOS ((size_t)-1)

#ifdef __cplusplus
#define ZTD_MAP_INIT \
  {                  \
  }
#define ZTD_MAP_INIT_FN(h, e) \
  {nullptr, 0, 0, nullptr, false, nullptr, 0, (h), (e), 0, 0}
#else
#define ZTD_MAP_INIT {0}
#define ZTD_MAP_INIT_FN(h, e) {.hash = (h), .eq = (e)}
#endif

/* `const char *` keys, compared by contents. */
#define ZTD_MAP_INIT_CSTR ZTD_MAP_INIT_FN(ztd_map_hash_cstr, ztd_map_eq_cstr)

/* 64-bit hash of n bytes (the default key hash). */
ZTD_MAP_DEF uint64_t ztd_map_hash_bytes(const void* p, size_t n);
/* Key functions for `const char *` keys (NUL-terminated, non-NULL). */
ZTD_MAP_DEF uint64_t ztd_map_hash_cstr(const void* key, size_t key_size);
ZTD_MAP_DEF bool ztd_map_eq_cstr(const void* a, const void* b, size_t key_size);

/* ---- Internal helpers used by the macros -------------------------------- */

#if defined(__GNUC__)
#define ZTD_MAP__ALIGNOF_EXPR(x) __alignof__(x)
#elif defined(__cplusplus)
#define ZTD_MAP__ALIGNOF_EXPR(x) alignof(max_align_t)
#else
#define ZTD_MAP__ALIGNOF_EXPR(x) _Alignof(max_align_t)
#endif

#if defined(__cplusplus)
#define ZTD_MAP__TYPED(m, p) static_cast<decltype((m)->ptr)>(p)
#elif defined(__GNUC__)
#define ZTD_MAP__TYPED(m, p) ((__typeof__((m)->ptr))(p))
#else
#define ZTD_MAP__TYPED(m, p) (p)
#endif

/* Arguments shared by the lookup/insert/remove primitives. */
#define ZTD_MAP__CORE(m)                                                      \
  (m)->ptr, &(m)->len, (m)->cap, (m)->slots, (m)->nslots, (m)->hash, (m)->eq, \
      sizeof *(m)->ptr, sizeof((m)->ptr->key)

/* Store k in the scratch entry. */
#define ZTD_MAP__SCRATCH(m, k) ((m)->ptr[(m)->cap].key = (k))

/* Grow so that n more entries fit. Returns the (possibly moved) entries; on
 * failure sets *failed and returns ptr unchanged. */
ZTD_MAP_DEF void* ztd_map__reserve(void* ptr,
                                   size_t len,
                                   size_t* cap,
                                   size_t extra,
                                   ZtdMapSlot** slots,
                                   size_t* nslots,
                                   size_t elem_size,
                                   size_t align,
                                   struct ZtdArena* arena,
                                   bool* failed);
/* The primitives below look up the key stored in the scratch entry ptr[cap]. */
ZTD_MAP_DEF size_t ztd_map__find(const void* ptr,
                                 size_t* len,
                                 size_t cap,
                                 const ZtdMapSlot* slots,
                                 size_t nslots,
                                 ZtdMapHashFn hash,
                                 ZtdMapEqFn eq,
                                 size_t elem_size,
                                 size_t key_size);
/* Index of the key's entry, inserting a zero-valued one if absent (needs
 * *len < cap). With only_new, returns ZTD_MAP_NPOS if the key was present. */
ZTD_MAP_DEF size_t ztd_map__put(void* ptr,
                                size_t* len,
                                size_t cap,
                                ZtdMapSlot* slots,
                                size_t nslots,
                                ZtdMapHashFn hash,
                                ZtdMapEqFn eq,
                                size_t elem_size,
                                size_t key_size,
                                bool only_new);
/* Flags for ztd_map__remove. */
#define ZTD_MAP__KEEP 1    /* copy the removed entry to the scratch entry */
#define ZTD_MAP__ORDERED 2 /* shift later entries down instead of swapping */
ZTD_MAP_DEF bool ztd_map__remove(void* ptr,
                                 size_t* len,
                                 size_t cap,
                                 ZtdMapSlot* slots,
                                 size_t nslots,
                                 ZtdMapHashFn hash,
                                 ZtdMapEqFn eq,
                                 size_t elem_size,
                                 size_t key_size,
                                 int flags);
ZTD_MAP_DEF void ztd_map__reindex(void* ptr,
                                  size_t* len,
                                  size_t cap,
                                  ZtdMapSlot* slots,
                                  size_t nslots,
                                  ZtdMapHashFn hash,
                                  ZtdMapEqFn eq,
                                  size_t elem_size,
                                  size_t key_size);
ZTD_MAP_DEF void ztd_map__clear(ZtdMapSlot* slots, size_t nslots);
ZTD_MAP_DEF void ztd_map__free(void* ptr,
                               ZtdMapSlot* slots,
                               struct ZtdArena* arena);

#ifdef __cplusplus
}
#endif

/* ---- Maps (m is a pointer to a ZTD_MAP) --------------------------------- */

/* Make room for n more entries. -> bool */
#define ZTD_MAP_RESERVE(m, n)                                               \
  ((m)->ptr = ZTD_MAP__TYPED(                                               \
       m, ztd_map__reserve((m)->ptr, (m)->len, &(m)->cap, (n), &(m)->slots, \
                           &(m)->nslots, sizeof *(m)->ptr,                  \
                           ZTD_MAP__ALIGNOF_EXPR(*(m)->ptr), (m)->arena,    \
                           &(m)->failed)),                                  \
   !(m)->failed)

/* Room for one more entry, without a call in the common case. -> bool */
#define ZTD_MAP__ROOM(m) \
  (((m)->len < (m)->cap && !(m)->failed) || ZTD_MAP_RESERVE(m, 1))

/* Set k to v, inserting or overwriting. -> bool
 * The entry's index goes in ins_ (sequenced before the value is evaluated),
 * so a value that looks up this map, clobbering the scratch key and tmp_,
 * can't redirect the store. */
#define ZTD_MAP_PUT(m, k, ...)                                            \
  (ZTD_MAP__ROOM(m) ? (ZTD_MAP__SCRATCH(m, k),                            \
                       (m)->ins_ = ztd_map__put(ZTD_MAP__CORE(m), false), \
                       (m)->ptr[(m)->ins_].val = (__VA_ARGS__), true)     \
                    : false)

/* Insert k -> v only if k is absent; v is evaluated only then. -> bool
 * (false if present or on failure; check m->failed to tell them apart) */
#define ZTD_MAP_PUT_NEW(m, k, ...)                                            \
  (ZTD_MAP__ROOM(m) && (ZTD_MAP__SCRATCH(m, k),                               \
                        ((m)->ins_ = ztd_map__put(ZTD_MAP__CORE(m), true)) != \
                            ZTD_MAP_NPOS)                                     \
       ? ((m)->ptr[(m)->ins_].val = (__VA_ARGS__), true)                      \
       : false)

/* Pointer to k's value, inserting a zero-filled one if absent; NULL on
 * failure. Like C++'s map[k]:  ++*MAP_AT(&counts, word)  (check for NULL). */
#define ZTD_MAP_AT(m, k)                                                     \
  (ZTD_MAP__ROOM(m) ? (ZTD_MAP__SCRATCH(m, k),                               \
                       &(m)->ptr[ztd_map__put(ZTD_MAP__CORE(m), false)].val) \
                    : NULL)

/* Index of k's entry in m->ptr, or ZTD_MAP_NPOS. */
#define ZTD_MAP_INDEX(m, k)                                             \
  ((m)->len ? (ZTD_MAP__SCRATCH(m, k), ztd_map__find(ZTD_MAP__CORE(m))) \
            : ZTD_MAP_NPOS)

/* Pointer to k's value, or NULL. */
#define ZTD_MAP_GET(m, k)                            \
  (((m)->tmp_ = ZTD_MAP_INDEX(m, k)) != ZTD_MAP_NPOS \
       ? &(m)->ptr[(m)->tmp_].val                    \
       : NULL)

/* k's value, or dflt (evaluated only when k is absent). */
#define ZTD_MAP_GET_OR(m, k, ...)                                              \
  (((m)->tmp_ = ZTD_MAP_INDEX(m, k)) != ZTD_MAP_NPOS ? (m)->ptr[(m)->tmp_].val \
                                                     : (__VA_ARGS__))

#define ZTD_MAP_HAS(m, k) (ZTD_MAP_INDEX(m, k) != ZTD_MAP_NPOS)

/* Remove k, moving the last entry into its place. -> bool (false if absent) */
#define ZTD_MAP_REMOVE(m, k) ZTD_MAP__REMOVE(m, k, 0)

/* Remove k, keeping the order of the other entries; O(len). -> bool */
#define ZTD_MAP_REMOVE_ORDERED(m, k) ZTD_MAP__REMOVE(m, k, ZTD_MAP__ORDERED)

/* Remove k and store its value in *out / its whole entry in *out (e.g. to
 * free an owned key). *out is untouched if k is absent. -> bool */
#define ZTD_MAP_REMOVE_INTO(m, k, out)           \
  (ZTD_MAP__REMOVE(m, k, ZTD_MAP__KEEP)          \
       ? (*(out) = (m)->ptr[(m)->cap].val, true) \
       : false)
#define ZTD_MAP_REMOVE_ENTRY(m, k, out)                                       \
  ((void)sizeof((m)->ptr == (out)), ZTD_MAP__REMOVE(m, k, ZTD_MAP__KEEP)      \
                                        ? (*(out) = (m)->ptr[(m)->cap], true) \
                                        : false)

#define ZTD_MAP__REMOVE(m, k, flags)                                          \
  ((m)->len                                                                   \
       ? (ZTD_MAP__SCRATCH(m, k), ztd_map__remove(ZTD_MAP__CORE(m), (flags))) \
       : false)

/* Rebuild the index after reordering or rewriting m->ptr[0 .. len). */
#define ZTD_MAP_REINDEX(m) ztd_map__reindex(ZTD_MAP__CORE(m))

/* Drop all entries but keep the storage; resets the failed flag. */
#define ZTD_MAP_CLEAR(m) \
  (ztd_map__clear((m)->slots, (m)->nslots), (m)->len = 0, (m)->failed = false)

/* Release the storage (unless arena-backed) and empty the map. The map keeps
 * its arena and key functions, so it can be reused. */
#define ZTD_MAP_FREE(m)                                              \
  (ztd_map__free((m)->ptr, (m)->slots, (m)->arena), (m)->ptr = NULL, \
   (m)->slots = NULL, (m)->len = (m)->cap = (m)->nslots = 0,         \
   (m)->failed = false)

/* for (entry *it ...) over every entry, in order: it->key, it->val. Needs
 * typeof (GCC, Clang, C23) or C++; otherwise loop over m->ptr[0 .. len). */
#if defined(__cplusplus)
#define ZTD_MAP_FOREACH(it, m) \
  for (decltype((m)->ptr) it = (m)->ptr; it && it < (m)->ptr + (m)->len; it++)
#elif defined(__GNUC__)
#define ZTD_MAP_FOREACH(it, m) \
  for (__typeof__((m)->ptr) it = (m)->ptr; it && it < (m)->ptr + (m)->len; it++)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define ZTD_MAP_FOREACH(it, m) \
  for (typeof((m)->ptr) it = (m)->ptr; it && it < (m)->ptr + (m)->len; it++)
#endif

/* ---- Short names (see SHORT NAMES above) -------------------------------- */

#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_MAP_NO_SHORT_NAMES)
typedef ZtdMapHashFn MapHashFn;
typedef ZtdMapEqFn MapEqFn;

#define MAP(K, V) ZTD_MAP(K, V)
#define MAP_NPOS ZTD_MAP_NPOS
#define MAP_INIT ZTD_MAP_INIT
#define MAP_INIT_FN(h, e) ZTD_MAP_INIT_FN(h, e)
#define MAP_INIT_CSTR ZTD_MAP_INIT_CSTR
#define MAP_RESERVE(m, n) ZTD_MAP_RESERVE(m, n)
#define MAP_PUT(m, k, ...) ZTD_MAP_PUT(m, k, __VA_ARGS__)
#define MAP_PUT_NEW(m, k, ...) ZTD_MAP_PUT_NEW(m, k, __VA_ARGS__)
#define MAP_AT(m, k) ZTD_MAP_AT(m, k)
#define MAP_INDEX(m, k) ZTD_MAP_INDEX(m, k)
#define MAP_GET(m, k) ZTD_MAP_GET(m, k)
#define MAP_GET_OR(m, k, ...) ZTD_MAP_GET_OR(m, k, __VA_ARGS__)
#define MAP_HAS(m, k) ZTD_MAP_HAS(m, k)
#define MAP_OF(E) ZTD_MAP_OF(E)
#define MAP_REMOVE(m, k) ZTD_MAP_REMOVE(m, k)
#define MAP_REMOVE_ORDERED(m, k) ZTD_MAP_REMOVE_ORDERED(m, k)
#define MAP_REMOVE_INTO(m, k, out) ZTD_MAP_REMOVE_INTO(m, k, out)
#define MAP_REMOVE_ENTRY(m, k, out) ZTD_MAP_REMOVE_ENTRY(m, k, out)
#define MAP_REINDEX(m) ZTD_MAP_REINDEX(m)
#define MAP_CLEAR(m) ZTD_MAP_CLEAR(m)
#define MAP_FREE(m) ZTD_MAP_FREE(m)
#ifdef ZTD_MAP_FOREACH
#define MAP_FOREACH(it, m) ZTD_MAP_FOREACH(it, m)
#endif

#define map_hash_bytes ztd_map_hash_bytes
#define map_hash_cstr ztd_map_hash_cstr
#define map_eq_cstr ztd_map_eq_cstr
#endif

#endif /* ZTD_MAP_H */

/* ---- Arena integration (declarations) -----------------------------------
 * The integration sections are re-evaluated on every include, so they appear
 * as soon as both headers have been seen, whatever their order. */

#if defined(ZTD_ARENA_H) && !defined(ZTD_MAP__ARENA_DECL)
#define ZTD_MAP__ARENA_DECL

#ifdef __cplusplus
extern "C" {
#endif
/* Returns a; defined only by an implementation that saw ztd_arena.h, so using
 * ZTD_MAP_INIT_ARENA without arena support fails to link. */
ZTD_MAP_DEF ZtdArena* ztd_map__arena_linked(ZtdArena* a);
#ifdef __cplusplus
}
#endif

/* A map that grows inside arena *a (automatic-storage initializers). */
#ifdef __cplusplus
#define ZTD_MAP_INIT_FN_ARENA(a, h, e) \
  {nullptr, 0, 0, ztd_map__arena_linked(a), false, nullptr, 0, (h), (e), 0, 0}
#else
#define ZTD_MAP_INIT_FN_ARENA(a, h, e) \
  {.arena = ztd_map__arena_linked(a), .hash = (h), .eq = (e)}
#endif
#define ZTD_MAP_INIT_ARENA(a) ZTD_MAP_INIT_FN_ARENA(a, NULL, NULL)
#define ZTD_MAP_INIT_CSTR_ARENA(a) \
  ZTD_MAP_INIT_FN_ARENA(a, ztd_map_hash_cstr, ztd_map_eq_cstr)

#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_MAP_NO_SHORT_NAMES)
#define MAP_INIT_ARENA(a) ZTD_MAP_INIT_ARENA(a)
#define MAP_INIT_FN_ARENA(a, h, e) ZTD_MAP_INIT_FN_ARENA(a, h, e)
#define MAP_INIT_CSTR_ARENA(a) ZTD_MAP_INIT_CSTR_ARENA(a)
#endif
#endif /* arena declarations */

/* ---- String integration (declarations) ---------------------------------- */

#if defined(ZTD_STR_H) && !defined(ZTD_MAP__STR_DECL)
#define ZTD_MAP__STR_DECL

#ifdef __cplusplus
extern "C" {
#endif
/* Key functions for ZtdStr keys (byte contents, as ztd_str_eq). */
ZTD_MAP_DEF uint64_t ztd_map_hash_str(const void* key, size_t key_size);
ZTD_MAP_DEF bool ztd_map_eq_str(const void* a, const void* b, size_t key_size);
#ifdef __cplusplus
}
#endif

#define ZTD_MAP_INIT_STR ZTD_MAP_INIT_FN(ztd_map_hash_str, ztd_map_eq_str)

#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_MAP_NO_SHORT_NAMES)
#define MAP_INIT_STR ZTD_MAP_INIT_STR
#define map_hash_str ztd_map_hash_str
#define map_eq_str ztd_map_eq_str
#endif
#endif /* string declarations */

/* ---- Arena + string (declarations) -------------------------------------- */

#if defined(ZTD_MAP__ARENA_DECL) && defined(ZTD_MAP__STR_DECL) && \
    !defined(ZTD_MAP__ARENA_STR_DECL)
#define ZTD_MAP__ARENA_STR_DECL
#define ZTD_MAP_INIT_STR_ARENA(a) \
  ZTD_MAP_INIT_FN_ARENA(a, ztd_map_hash_str, ztd_map_eq_str)
#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_MAP_NO_SHORT_NAMES)
#define MAP_INIT_STR_ARENA(a) ZTD_MAP_INIT_STR_ARENA(a)
#endif
#endif

/* ======================================================================== */

#ifdef ZTD_MAP_IMPLEMENTATION
#ifndef ZTD_MAP_IMPLEMENTATION_DONE
#define ZTD_MAP_IMPLEMENTATION_DONE

#ifdef __cplusplus
#error "ztd_map.h: compile the implementation as C (C11)"
#endif

#include <string.h>

#if defined(ZTD_MAP_MALLOC) != defined(ZTD_MAP_DEALLOC) || \
    defined(ZTD_MAP_MALLOC) != defined(ZTD_MAP_REALLOC)
#error \
    "ztd_map.h: define all of ZTD_MAP_MALLOC, ZTD_MAP_REALLOC and ZTD_MAP_DEALLOC, or none"
#endif
#ifndef ZTD_MAP_MALLOC
#if defined(ZTD_MALLOC) && defined(ZTD_REALLOC) && defined(ZTD_FREE)
#define ZTD_MAP_MALLOC(sz) ZTD_MALLOC(sz)
#define ZTD_MAP_REALLOC(p, sz) ZTD_REALLOC(p, sz)
#define ZTD_MAP_DEALLOC(p) ZTD_FREE(p)
#else
#include <stdlib.h>
#define ZTD_MAP_MALLOC(sz) malloc(sz)
#define ZTD_MAP_REALLOC(p, sz) realloc((p), (sz))
#define ZTD_MAP_DEALLOC(p) free(p)
#endif
#endif

#ifndef ZTD_MAP_ASSERT
#ifdef ZTD_ASSERT
#define ZTD_MAP_ASSERT(x) ZTD_ASSERT(x)
#else
#include <assert.h>
#define ZTD_MAP_ASSERT(x) assert(x)
#endif
#endif

#define ZTD_MAP__MIN_SLOTS 8

/* Arena growth hook. A tentative definition: it stays NULL unless the arena
 * integration below is compiled into this file, which initialises it. */
static void* (*ztd_map__arena_realloc)(struct ZtdArena* a,
                                       void* ptr,
                                       size_t old_size,
                                       size_t new_size,
                                       size_t align);

/* ---- Hashing ------------------------------------------------------------ */

static uint64_t ztd_map__mix(uint64_t x) {
  x ^= x >> 32;
  x *= 0xd6e8feb86659fd93ull;
  x ^= x >> 32;
  x *= 0xd6e8feb86659fd93ull;
  x ^= x >> 32;
  return x;
}

static uint64_t ztd_map__round(uint64_t h, uint64_t w) {
  h ^= ztd_map__mix(w);
  return (h << 27 | h >> 37) * 0x9e3779b97f4a7c15ull;
}

ZTD_MAP_DEF uint64_t ztd_map_hash_bytes(const void* p, size_t n) {
  const unsigned char* b = (const unsigned char*)p;
  uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t)n;
  uint64_t w;
  while (n >= 8) {
    memcpy(&w, b, 8);
    h = ztd_map__round(h, w);
    b += 8;
    n -= 8;
  }
  if (n) {
    w = 0;
    memcpy(&w, b, n);
    h = ztd_map__round(h, w);
  }
  return ztd_map__mix(h);
}

ZTD_MAP_DEF uint64_t ztd_map_hash_cstr(const void* key, size_t key_size) {
  (void)key_size;
  const char* s = *(const char* const*)key;
  return ztd_map_hash_bytes(s, strlen(s));
}

ZTD_MAP_DEF bool ztd_map_eq_cstr(const void* a,
                                 const void* b,
                                 size_t key_size) {
  (void)key_size;
  return strcmp(*(const char* const*)a, *(const char* const*)b) == 0;
}

/* The 32-bit hash kept in the index. The multiply spreads weak user hashes
 * (e.g. the identity) over the high bits we keep. */
static uint32_t ztd_map__hash(ZtdMapHashFn hash,
                              const void* key,
                              size_t key_size) {
  uint64_t h = hash ? hash(key, key_size) : ztd_map_hash_bytes(key, key_size);
  return (uint32_t)((h * 0x9e3779b97f4a7c15ull) >> 32);
}

static bool ztd_map__eq(ZtdMapEqFn eq,
                        const void* a,
                        const void* b,
                        size_t key_size) {
  return eq ? eq(a, b, key_size) : memcmp(a, b, key_size) == 0;
}

/* ---- Index -------------------------------------------------------------- */

#define ZTD_MAP__ENTRY(ptr, i, elem_size) \
  ((unsigned char*)(ptr) + (i) * (elem_size))

/* Slot holding entry `idx` (whose hash is h); the entry must be indexed. */
static size_t ztd_map__slot_of(const ZtdMapSlot* slots,
                               size_t nslots,
                               uint32_t h,
                               size_t idx) {
  size_t mask = nslots - 1;
  for (size_t i = h & mask;; i = (i + 1) & mask) {
    if (slots[i].idx == idx + 1)
      return i;
    ZTD_MAP_ASSERT(slots[i].idx != 0 &&
                   "ztd_map: entry missing from the index");
  }
}

/* Slot holding the scratch key, or (with *empty) the empty slot where it
 * would go; NPOS if the table is full or missing. */
static size_t ztd_map__lookup(const void* ptr,
                              size_t cap,
                              const ZtdMapSlot* slots,
                              size_t nslots,
                              uint32_t h,
                              ZtdMapEqFn eq,
                              size_t elem_size,
                              size_t key_size,
                              bool* empty) {
  *empty = false;
  if (nslots == 0)
    return ZTD_MAP_NPOS;
  const void* key = ZTD_MAP__ENTRY(ptr, cap, elem_size);
  size_t mask = nslots - 1;
  for (size_t i = h & mask, n = 0; n < nslots; i = (i + 1) & mask, n++) {
    if (slots[i].idx == 0) {
      *empty = true;
      return i;
    }
    if (slots[i].hash == h &&
        ztd_map__eq(eq, ZTD_MAP__ENTRY(ptr, slots[i].idx - 1, elem_size), key,
                    key_size))
      return i;
  }
  return ZTD_MAP_NPOS;
}

static void ztd_map__place(ZtdMapSlot* slots,
                           size_t nslots,
                           uint32_t h,
                           size_t idx) {
  size_t mask = nslots - 1, i = h & mask;
  while (slots[i].idx)
    i = (i + 1) & mask;
  slots[i].hash = h;
  slots[i].idx = (uint32_t)(idx + 1);
}

/* ---- Maps --------------------------------------------------------------- */

static void* ztd_map__alloc(struct ZtdArena* arena,
                            void* ptr,
                            size_t old_size,
                            size_t size,
                            size_t align) {
  if (arena) {
    ZTD_MAP_ASSERT(ztd_map__arena_realloc &&
                   "ztd_map: arena-backed map, but the implementation did not "
                   "include ztd_arena.h");
    if (!ztd_map__arena_realloc)
      return NULL;
    return ztd_map__arena_realloc(arena, ptr, old_size, size, align);
  }
  (void)old_size;
  (void)align; /* malloc's alignment is enough for any object type */
  return ptr ? ZTD_MAP_REALLOC(ptr, size) : ZTD_MAP_MALLOC(size);
}

ZTD_MAP_DEF void* ztd_map__reserve(void* ptr,
                                   size_t len,
                                   size_t* cap,
                                   size_t extra,
                                   ZtdMapSlot** slots,
                                   size_t* nslots,
                                   size_t elem_size,
                                   size_t align,
                                   struct ZtdArena* arena,
                                   bool* failed) {
  if (*failed)
    return ptr;
  if (extra <= *cap - len)
    return ptr;
  ZTD_MAP_ASSERT(elem_size > 0);
  if (extra > SIZE_MAX - len)
    goto fail;

  /* Entries fill at most 3/4 of the slots; cap is exactly that limit. */
  size_t need = len + extra;
  size_t n = *nslots ? *nslots : ZTD_MAP__MIN_SLOTS;
  while (n / 4 * 3 < need) {
    if (n > SIZE_MAX / 2)
      goto fail;
    n *= 2;
  }
  if (n - 1 > UINT32_MAX)
    goto fail; /* slot positions are 32-bit */
  size_t new_cap = n / 4 * 3;
  if (new_cap + 1 > SIZE_MAX / elem_size || n > SIZE_MAX / sizeof(ZtdMapSlot))
    goto fail;

  ZtdMapSlot* ns = (ZtdMapSlot*)ztd_map__alloc(
      arena, NULL, 0, n * sizeof(ZtdMapSlot), _Alignof(ZtdMapSlot));
  if (!ns)
    goto fail;
  void* p = ztd_map__alloc(arena, ptr, ptr ? (*cap + 1) * elem_size : 0,
                           (new_cap + 1) * elem_size, align);
  if (!p) {
    if (!arena)
      ZTD_MAP_DEALLOC(ns);
    goto fail;
  }

  memset(ns, 0, n * sizeof(ZtdMapSlot));
  for (size_t i = 0; i < *nslots; i++) {
    if ((*slots)[i].idx)
      ztd_map__place(ns, n, (*slots)[i].hash, (*slots)[i].idx - 1);
  }
  if (!arena)
    ZTD_MAP_DEALLOC(*slots);
  *slots = ns;
  *nslots = n;
  *cap = new_cap;
  return p;

fail:
  *failed = true;
  return ptr;
}

ZTD_MAP_DEF size_t ztd_map__find(const void* ptr,
                                 size_t* len,
                                 size_t cap,
                                 const ZtdMapSlot* slots,
                                 size_t nslots,
                                 ZtdMapHashFn hash,
                                 ZtdMapEqFn eq,
                                 size_t elem_size,
                                 size_t key_size) {
  (void)len;
  if (nslots == 0)
    return ZTD_MAP_NPOS;
  uint32_t h =
      ztd_map__hash(hash, ZTD_MAP__ENTRY(ptr, cap, elem_size), key_size);
  bool empty;
  size_t i = ztd_map__lookup(ptr, cap, slots, nslots, h, eq, elem_size,
                             key_size, &empty);
  return i == ZTD_MAP_NPOS || empty ? ZTD_MAP_NPOS : slots[i].idx - 1;
}

ZTD_MAP_DEF size_t ztd_map__put(void* ptr,
                                size_t* len,
                                size_t cap,
                                ZtdMapSlot* slots,
                                size_t nslots,
                                ZtdMapHashFn hash,
                                ZtdMapEqFn eq,
                                size_t elem_size,
                                size_t key_size,
                                bool only_new) {
  ZTD_MAP_ASSERT(*len < cap && "ztd_map: put without room");
  unsigned char* key = ZTD_MAP__ENTRY(ptr, cap, elem_size);
  uint32_t h = ztd_map__hash(hash, key, key_size);
  bool empty;
  size_t i = ztd_map__lookup(ptr, cap, slots, nslots, h, eq, elem_size,
                             key_size, &empty);
  ZTD_MAP_ASSERT(i !=
                 ZTD_MAP_NPOS); /* load <= 3/4 always leaves an empty slot */
  if (!empty)
    return only_new ? ZTD_MAP_NPOS : slots[i].idx - 1;

  unsigned char* e = ZTD_MAP__ENTRY(ptr, *len, elem_size);
  memset(e, 0, elem_size);
  memcpy(e, key, key_size);
  slots[i].hash = h;
  slots[i].idx = (uint32_t)(*len + 1);
  return (*len)++;
}

ZTD_MAP_DEF bool ztd_map__remove(void* ptr,
                                 size_t* len,
                                 size_t cap,
                                 ZtdMapSlot* slots,
                                 size_t nslots,
                                 ZtdMapHashFn hash,
                                 ZtdMapEqFn eq,
                                 size_t elem_size,
                                 size_t key_size,
                                 int flags) {
  if (nslots == 0)
    return false;
  uint32_t h =
      ztd_map__hash(hash, ZTD_MAP__ENTRY(ptr, cap, elem_size), key_size);
  bool empty;
  size_t i = ztd_map__lookup(ptr, cap, slots, nslots, h, eq, elem_size,
                             key_size, &empty);
  if (i == ZTD_MAP_NPOS || empty)
    return false;
  size_t idx = slots[i].idx - 1;

  /* Backward-shift deletion: pull later members of the probe run into the
   * hole unless that would move them before their home slot. */
  size_t mask = nslots - 1;
  for (size_t j = (i + 1) & mask; slots[j].idx; j = (j + 1) & mask) {
    size_t home = slots[j].hash & mask;
    bool stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);
    if (!stays) {
      slots[i] = slots[j];
      i = j;
    }
  }
  slots[i].idx = 0;
  slots[i].hash = 0;

  if (flags & ZTD_MAP__KEEP)
    memcpy(ZTD_MAP__ENTRY(ptr, cap, elem_size),
           ZTD_MAP__ENTRY(ptr, idx, elem_size), elem_size);

  size_t last = *len - 1;
  if (flags & ZTD_MAP__ORDERED) {
    /* Close the gap and renumber the index entries that moved down. */
    memmove(ZTD_MAP__ENTRY(ptr, idx, elem_size),
            ZTD_MAP__ENTRY(ptr, idx + 1, elem_size), (last - idx) * elem_size);
    for (size_t s = 0; s < nslots; s++) {
      if (slots[s].idx > idx + 1)
        slots[s].idx--;
    }
  } else if (idx != last) {
    /* Move the last entry into the gap and repoint its slot. */
    unsigned char *dst = ZTD_MAP__ENTRY(ptr, idx, elem_size),
                  *src = ZTD_MAP__ENTRY(ptr, last, elem_size);
    memcpy(dst, src, elem_size);
    size_t s = ztd_map__slot_of(slots, nslots,
                                ztd_map__hash(hash, dst, key_size), last);
    slots[s].idx = (uint32_t)(idx + 1);
  }
  *len = last;
  return true;
}

ZTD_MAP_DEF void ztd_map__reindex(void* ptr,
                                  size_t* len,
                                  size_t cap,
                                  ZtdMapSlot* slots,
                                  size_t nslots,
                                  ZtdMapHashFn hash,
                                  ZtdMapEqFn eq,
                                  size_t elem_size,
                                  size_t key_size) {
  (void)cap;
  (void)eq;
  ztd_map__clear(slots, nslots);
  for (size_t i = 0; i < *len; i++) {
    ztd_map__place(
        slots, nslots,
        ztd_map__hash(hash, ZTD_MAP__ENTRY(ptr, i, elem_size), key_size), i);
  }
}

ZTD_MAP_DEF void ztd_map__clear(ZtdMapSlot* slots, size_t nslots) {
  if (slots)
    memset(slots, 0, nslots * sizeof *slots);
}

ZTD_MAP_DEF void ztd_map__free(void* ptr,
                               ZtdMapSlot* slots,
                               struct ZtdArena* arena) {
  if (arena)
    return;
  ZTD_MAP_DEALLOC(ptr);
  ZTD_MAP_DEALLOC(slots);
}

#endif /* ZTD_MAP_IMPLEMENTATION_DONE */

/* ---- Arena integration (implementation) --------------------------------- */

#if defined(ZTD_ARENA_H) && !defined(ZTD_MAP__ARENA_IMPL)
#define ZTD_MAP__ARENA_IMPL

static void* (*ztd_map__arena_realloc)(struct ZtdArena* a,
                                       void* ptr,
                                       size_t old_size,
                                       size_t new_size,
                                       size_t align) = ztd_arena_realloc;

ZTD_MAP_DEF ZtdArena* ztd_map__arena_linked(ZtdArena* a) {
  return a;
}
#endif /* arena implementation */

/* ---- String integration (implementation) -------------------------------- */

#if defined(ZTD_STR_H) && !defined(ZTD_MAP__STR_IMPL)
#define ZTD_MAP__STR_IMPL

ZTD_MAP_DEF uint64_t ztd_map_hash_str(const void* key, size_t key_size) {
  (void)key_size;
  const ZtdStr* s = (const ZtdStr*)key;
  return ztd_map_hash_bytes(s->ptr, s->len);
}

ZTD_MAP_DEF bool ztd_map_eq_str(const void* a, const void* b, size_t key_size) {
  (void)key_size;
  const ZtdStr *x = (const ZtdStr*)a, *y = (const ZtdStr*)b;
  return x->len == y->len &&
         (x->len == 0 || memcmp(x->ptr, y->ptr, x->len) == 0);
}
#endif /* string implementation */
#endif /* ZTD_MAP_IMPLEMENTATION */
