/* ztd_arr.h - single-header arrays: growable lists, slices, sort/search (C11)
 *
 * USAGE
 *   In exactly one .c file:
 *       #define ZTD_ARR_IMPLEMENTATION
 *       #include "ztd_arr.h"
 *   Everywhere else just #include "ztd_arr.h".
 *   Define ZTD_ARR_STATIC before including to make every function static.
 *   The implementation must be compiled as C; the declarations and macros
 *   also work from C++.
 *
 * PIECES
 *   ZTD_LIST(T)   An owning, growable array of T:
 *                     struct { T *ptr; size_t len; size_t cap;
 *                              struct ZtdArena *arena; bool failed; }
 *                 Same layout and error convention as ZtdStrBuf. Operations
 *                 are macros taking a POINTER to the list. Declare a type once
 *                 with a typedef; two spellings of ZTD_LIST(int) are distinct
 *                 (incompatible) anonymous structs:
 *                     typedef ZTD_LIST(int) IntList;
 *
 *   ZTD_SLICE(T)  A non-owning view: struct { T *ptr; size_t len; }.
 *
 *   ZTD_ARR_*     Helpers that work on a pointer to ANYTHING with .ptr/.len:
 *                 lists, slices, ZtdStr, ZtdStrBuf, your own structs.
 *                 Sorting/searching take qsort-style comparators. Mutating
 *                 helpers refuse (with a const warning) read-only arrays such
 *                 as ZtdStr.
 *
 *   ZtdArena      With ztd_arena.h (included before or after this header, in
 *                 any order) you get arena-backed lists: initialise with
 *                 ZTD_LIST_INIT_ARENA(&a) and the list grows inside the arena
 *                 (in place when it is the arena's latest allocation);
 *                 ZTD_LIST_FREE then leaves the memory to the arena. You also
 *                 get ZTD_ARR_ARENA_DUP (copy any array into an arena). The
 *                 implementation file must include ztd_arena.h too; otherwise
 *                 ZTD_LIST_INIT_ARENA fails to link.
 *
 *   ZtdStr        With ztd_str.h (in any order) you also get ZtdStrList (a
 *                 list of views) with split / lines / join. Sort one with
 *                 ZTD_ARR_SORT(&l, ztd_str_cmp_qsort).
 *
 * SHORT NAMES
 *   By default the header also defines unprefixed aliases:
 *       LIST(), SLICE(), LIST_INIT, LIST_* , ARR_*, SLICE_SET, COUNTOF,
 *       ArrCmp, arr_* for every ztd_arr_* function,
 *       StrList and strlist_* (with ztd_str.h).
 *   Functions are aliased with object-like macros, so they still work as
 *   function pointers. LIST_INIT and LIST_REMOVE clash with <sys/queue.h>.
 *   To get only the ztd_ names, define before including:
 *       ZTD_NO_SHORT_NAMES        for every ztd_* header, or
 *       ZTD_ARR_NO_SHORT_NAMES    for this header only.
 *   Configuration macros (ZTD_ARR_IMPLEMENTATION etc.) are always prefixed.
 *
 * QUICK TOUR (short names)
 *   typedef LIST(int) IntList;
 *   IntList xs = LIST_INIT;
 *   for (int i = 0; i < 10; i++) LIST_PUSH(&xs, 9 - i);
 *   LIST_INSERT(&xs, 0, 42);
 *   LIST_SWAP_REMOVE(&xs, 3);
 *   int gone;
 *   LIST_REMOVE_INTO(&xs, 0, &gone);                     // gone == 42
 *   ARR_SORT(&xs, cmp_int);
 *   size_t at = ARR_BSEARCH(&xs, &(int){7}, cmp_int);   // index or ARR_NPOS
 *   ARR_FOREACH(int, it, &xs) printf("%d\n", *it);
 *   if (xs.failed) out_of_memory();
 *   LIST_FREE(&xs);
 *
 *   typedef struct { int x, y; } Point;
 *   typedef LIST(Point) PointList;
 *   PointList ps = LIST_INIT;
 *   LIST_PUSH(&ps, (Point){1, 2});                       // commas are fine
 *   Point *p = LIST_ADD(&ps);                            // zeroed, or NULL
 *   if (p) p->x = 3;
 *
 *   int raw[] = {3, 1, 2};
 *   typedef SLICE(int) IntSlice;
 *   IntSlice s = ARR_VIEW(raw);                          // {raw, 3}
 *   SLICE_SET(&s, &s, 1, 99);                            // {raw + 1, 2}
 *
 *   StrList parts = LIST_INIT;                           // with ztd_str.h
 *   strlist_split(&parts, STR("b,c,a"), STR(","));
 *   ARR_SORT(&parts, str_cmp_qsort);
 *   strlist_join(&buf, &parts, STR(", "));               // "a, b, c"
 *
 * CONVENTIONS
 *   - Macros taking a list or array `l` may evaluate `l` more than once;
 *     pass a plain pointer expression. Element values, indices, counts and
 *     source pointers are evaluated at most once (ZTD_LIST_INSERT skips its
 *     index when the push fails). The exceptions are `other` in
 *     ZTD_LIST_EXTEND and `n` in ZTD_LIST_ADD_N, which are evaluated twice.
 *   - Allocation errors are sticky, as in ZtdStrBuf: after a failure every
 *     further growing operation (including ZTD_LIST_RESIZE) returns false and
 *     leaves the list as it was, so you can do a run of pushes and check
 *     l.failed once. ZTD_LIST_CLEAR() and ZTD_LIST_FREE() reset it.
 *   - Growing invalidates pointers into the list.
 *   - ZTD_LIST_APPEND / _INSERT_N / _EXTEND accept a source that points into
 *     the same list. The value passed to ZTD_LIST_PUSH / _INSERT may read the
 *     list (it is evaluated after growing) but must not modify it.
 *   - Out-of-range indices are caught by ZTD_ARR_ASSERT. With asserts off,
 *     removals clamp and ZTD_LIST_POP on an empty list reads element 0
 *     without changing the length; ZTD_ARR_AT / _LAST are unchecked. Use
 *     ZTD_LIST_TRY_POP when the list may be empty.
 *   - "Not found" is ZTD_ARR_NPOS (same value as ZTD_STR_NPOS).
 *   - Arena-backed lists leave their old storage in the arena when they move,
 *     and must not be used after the arena is rewound past their latest
 *     growth, or reset.
 *
 * CONFIGURATION
 *   ZTD_ARR_ASSERT(x)    default ZTD_ASSERT(x) if defined, else assert(x);
 *                        define before including the header (it is used by
 *                        the macros)
 *   ZTD_ARR_MALLOC(sz) / ZTD_ARR_REALLOC(p, sz) / ZTD_ARR_FREE(p)
 *                        backing allocator; define all three or none, before
 *                        including the implementation. Defaults to
 *                        ZTD_MALLOC / ZTD_REALLOC / ZTD_FREE if those are
 *                        defined, else the C library.
 *
 * Requires C11. LICENSE: public domain / MIT-0. Do what you like.
 */
#ifndef ZTD_ARR_H
#define ZTD_ARR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> /* qsort */

#ifdef ZTD_ARR_STATIC
#if defined(__GNUC__)
#define ZTD_ARR_DEF static __attribute__((unused))
#else
#define ZTD_ARR_DEF static
#endif
#else
#define ZTD_ARR_DEF extern
#endif

#ifndef ZTD_ARR_ASSERT
#ifdef ZTD_ASSERT
#define ZTD_ARR_ASSERT(x) ZTD_ASSERT(x)
#else
#include <assert.h>
#define ZTD_ARR_ASSERT(x) assert(x)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types -------------------------------------------------------------- */

struct ZtdArena; /* defined by ztd_arena.h; only ever used by pointer here */

#define ZTD_LIST(T)         \
  struct {                  \
    T* ptr;                 \
    size_t len;             \
    size_t cap;             \
    struct ZtdArena* arena; \
    bool failed;            \
  }
#define ZTD_SLICE(T) \
  struct {           \
    T* ptr;          \
    size_t len;      \
  }

#ifdef __cplusplus
#define ZTD_LIST_INIT \
  {                   \
  }
#else
#define ZTD_LIST_INIT {0}
#endif

#define ZTD_ARR_NPOS ((size_t)-1)

/* qsort-style comparator: <0, 0, >0. */
typedef int (*ZtdArrCmp)(const void* a, const void* b);

/* ---- Internal helpers used by the macros -------------------------------- */

#if defined(__GNUC__)
#define ZTD_ARR__ALIGNOF_EXPR(x) __alignof__(x)
#elif defined(__cplusplus)
#define ZTD_ARR__ALIGNOF_EXPR(x) alignof(max_align_t)
#else
#define ZTD_ARR__ALIGNOF_EXPR(x) _Alignof(max_align_t)
#endif

/* Convert a void * to the element pointer type of a (needed in C++, and gives
 * ZTD_ARR_ARENA_DUP a typed result where the compiler allows it). */
#if defined(__cplusplus)
#define ZTD_ARR__TYPED(a, p) static_cast<decltype((a)->ptr)>(p)
#elif defined(__GNUC__)
#define ZTD_ARR__TYPED(a, p) ((__typeof__((a)->ptr))(p))
#else
#define ZTD_ARR__TYPED(a, p) (p)
#endif

/* Compile-time check (a warning) that p points to the element type of l. */
#define ZTD_ARR__SAME(l, p) ((void)sizeof((l)->ptr == (p)))

/* elem_size, align, arena, failed */
#define ZTD_LIST__ARGS(l) \
  sizeof *(l)->ptr, ZTD_ARR__ALIGNOF_EXPR(*(l)->ptr), (l)->arena, &(l)->failed

static inline size_t ztd_arr__idx(size_t i, size_t n) {
  ZTD_ARR_ASSERT(i < n && "ztd_arr: index out of range");
  (void)n;
  return i;
}

static inline size_t ztd_arr__pop_idx(size_t* len) {
  ZTD_ARR_ASSERT(*len > 0 && "ztd_arr: pop from empty list");
  if (*len == 0)
    return 0;
  return --*len;
}

static inline void ztd_arr__truncate(size_t* len, size_t n) {
  if (n < *len)
    *len = n;
}

/* Clamp [start, end) to n elements; returns the start pointer (NULL if ptr
 * is) and stores the length. */
static inline void* ztd_arr__slice(const void* ptr,
                                   size_t elem_size,
                                   size_t n,
                                   size_t start,
                                   size_t end,
                                   size_t* out_len) {
  if (end > n)
    end = n;
  if (start > end)
    start = end;
  *out_len = end - start;
  return ptr ? (void*)((const unsigned char*)ptr + start * elem_size) : NULL;
}

/* The list primitives below take the list's fields separately and return the
 * (possibly moved) storage pointer, which the macros assign back. On failure
 * they set *failed and return the old pointer unchanged. */
ZTD_ARR_DEF void* ztd_list__reserve(void* ptr,
                                    size_t len,
                                    size_t* cap,
                                    size_t extra,
                                    size_t elem_size,
                                    size_t align,
                                    struct ZtdArena* arena,
                                    bool* failed);
/* Insert n elements at idx, copied from src (which may alias the list), or
 * zero-filled if src is NULL. */
ZTD_ARR_DEF void* ztd_list__insert(void* ptr,
                                   size_t* len,
                                   size_t* cap,
                                   size_t idx,
                                   const void* src,
                                   size_t n,
                                   size_t elem_size,
                                   size_t align,
                                   struct ZtdArena* arena,
                                   bool* failed);
ZTD_ARR_DEF void* ztd_list__resize(void* ptr,
                                   size_t* len,
                                   size_t* cap,
                                   size_t n,
                                   size_t elem_size,
                                   size_t align,
                                   struct ZtdArena* arena,
                                   bool* failed);
/* Take the element staged in the spare slot ptr[*len] into the list at idx,
 * shifting [idx, *len) up by one. */
ZTD_ARR_DEF void ztd_list__rotate_in(void* ptr,
                                     size_t* len,
                                     size_t cap,
                                     size_t idx,
                                     size_t elem_size);
ZTD_ARR_DEF void ztd_list__remove(void* ptr,
                                  size_t* len,
                                  size_t idx,
                                  size_t n,
                                  size_t elem_size);
ZTD_ARR_DEF void ztd_list__swap_remove(void* ptr,
                                       size_t* len,
                                       size_t idx,
                                       size_t elem_size);
/* Copy element idx to *out, then remove it (by swapping the last element in
 * if swap). Returns false, leaving *out alone, if idx is out of range. */
ZTD_ARR_DEF bool ztd_list__remove_into(void* ptr,
                                       size_t* len,
                                       size_t idx,
                                       void* out,
                                       bool swap,
                                       size_t elem_size);
ZTD_ARR_DEF void* ztd_list__shrink(void* ptr,
                                   size_t len,
                                   size_t* cap,
                                   size_t elem_size,
                                   struct ZtdArena* arena);
ZTD_ARR_DEF void ztd_list__free(void* ptr, struct ZtdArena* arena);

/* ---- Lists (l is a pointer to a ZTD_LIST) ------------------------------- */

/* Make room for n more elements. -> bool */
#define ZTD_LIST_RESERVE(l, n)                                                 \
  ((l)->ptr =                                                                  \
       ZTD_ARR__TYPED(l, ztd_list__reserve((l)->ptr, (l)->len, &(l)->cap, (n), \
                                           ZTD_LIST__ARGS(l))),                \
   !(l)->failed)

/* Set the length to n; new elements are zero-filled. -> bool */
#define ZTD_LIST_RESIZE(l, n)                                                  \
  ((l)->ptr =                                                                  \
       ZTD_ARR__TYPED(l, ztd_list__resize((l)->ptr, &(l)->len, &(l)->cap, (n), \
                                          ZTD_LIST__ARGS(l))),                 \
   !(l)->failed)

/* Append one element. The value may contain unparenthesised commas, as in
 * ZTD_LIST_PUSH(&l, (Point){1, 2}). -> bool */
#define ZTD_LIST_PUSH(l, ...)                                                  \
  ((((l)->len < (l)->cap && !(l)->failed) ||                                   \
    ((l)->ptr =                                                                \
         ZTD_ARR__TYPED(l, ztd_list__reserve((l)->ptr, (l)->len, &(l)->cap, 1, \
                                             ZTD_LIST__ARGS(l))),              \
     !(l)->failed))                                                            \
       ? ((l)->ptr[(l)->len] = (__VA_ARGS__), (l)->len++, true)                \
       : false)

/* Append n elements copied from src (may point into l). -> bool */
#define ZTD_LIST_APPEND(l, src, n) ZTD_LIST_INSERT_N(l, (l)->len, src, n)

/* Append every element of another array (anything with .ptr/.len). -> bool */
#define ZTD_LIST_EXTEND(l, other) ZTD_LIST_APPEND(l, (other)->ptr, (other)->len)

/* Insert one element before index i (i == len appends). -> bool
 * The value is staged in the spare slot past the end, then rotated in; i is
 * evaluated after the value but before the length changes. */
#define ZTD_LIST_INSERT(l, i, ...)                                   \
  ((((l)->len < (l)->cap && !(l)->failed) || ZTD_LIST_RESERVE(l, 1)) \
       ? ((l)->ptr[(l)->len] = (__VA_ARGS__),                        \
          ztd_list__rotate_in((l)->ptr, &(l)->len, (l)->cap, (i),    \
                              sizeof *(l)->ptr),                     \
          true)                                                      \
       : false)

/* Insert n elements copied from src (may point into l) before index i.
 * -> bool */
#define ZTD_LIST_INSERT_N(l, i, src, n)                                        \
  (ZTD_ARR__SAME(l, src),                                                      \
   (l)->ptr =                                                                  \
       ZTD_ARR__TYPED(l, ztd_list__insert((l)->ptr, &(l)->len, &(l)->cap, (i), \
                                          (src), (n), ZTD_LIST__ARGS(l))),     \
   !(l)->failed)

/* Append n zero-filled elements and return a pointer to the first, or NULL
 * on failure (n == 0 returns the end pointer, NULL for an unallocated list).
 * Handy for building large elements in place. n is evaluated twice. */
#define ZTD_LIST_ADD_N(l, n)                                 \
  (ZTD_LIST_INSERT_N(l, (l)->len, NULL, n)                   \
       ? ((l)->ptr ? (l)->ptr + ((l)->len - (n)) : (l)->ptr) \
       : NULL)
#define ZTD_LIST_ADD(l) ZTD_LIST_ADD_N(l, 1)

/* Remove and return the last element (asserts non-empty). */
#define ZTD_LIST_POP(l) ((l)->ptr[ztd_arr__pop_idx(&(l)->len)])

/* If the list is non-empty, remove the last element into *out. -> bool */
#define ZTD_LIST_TRY_POP(l, out) \
  ((l)->len ? (*(out) = (l)->ptr[--(l)->len], true) : false)

/* Remove element i / n elements from i, keeping order. */
#define ZTD_LIST_REMOVE(l, i) ZTD_LIST_REMOVE_RANGE(l, i, 1)
#define ZTD_LIST_REMOVE_RANGE(l, i, n) \
  ztd_list__remove((l)->ptr, &(l)->len, (i), (n), sizeof *(l)->ptr)

/* Remove element i in O(1) by moving the last element into its place. */
#define ZTD_LIST_SWAP_REMOVE(l, i) \
  ztd_list__swap_remove((l)->ptr, &(l)->len, (i), sizeof *(l)->ptr)

/* Remove element i into *out (keeping order / by swapping). -> bool */
#define ZTD_LIST_REMOVE_INTO(l, i, out)                          \
  (ZTD_ARR__SAME(l, out),                                        \
   ztd_list__remove_into((l)->ptr, &(l)->len, (i), (out), false, \
                         sizeof *(l)->ptr))
#define ZTD_LIST_SWAP_REMOVE_INTO(l, i, out)                    \
  (ZTD_ARR__SAME(l, out),                                       \
   ztd_list__remove_into((l)->ptr, &(l)->len, (i), (out), true, \
                         sizeof *(l)->ptr))

/* Shorten to n elements; no-op if n >= len. */
#define ZTD_LIST_TRUNCATE(l, n) ztd_arr__truncate(&(l)->len, (n))

/* Drop all elements but keep the storage; resets the failed flag. */
#define ZTD_LIST_CLEAR(l) ((l)->len = 0, (l)->failed = false)

/* Give unused capacity back (no-op for arena-backed lists). */
#define ZTD_LIST_SHRINK(l)                                               \
  ((l)->ptr =                                                            \
       ZTD_ARR__TYPED(l, ztd_list__shrink((l)->ptr, (l)->len, &(l)->cap, \
                                          sizeof *(l)->ptr, (l)->arena)))

/* Release the storage (unless arena-backed) and empty the list. The list keeps
 * its arena, so it can be reused. */
#define ZTD_LIST_FREE(l)                                  \
  (ztd_list__free((l)->ptr, (l)->arena), (l)->ptr = NULL, \
   (l)->len = (l)->cap = 0, (l)->failed = false)

/* ---- Arrays (a is a pointer to anything with .ptr/.len) ----------------- */

/* Element count of a true C array. On GCC/Clang, passing a pointer fails to
 * compile. */
#if defined(__GNUC__) && !defined(__cplusplus)
#define ZTD_COUNTOF(arr)                                 \
  (sizeof(arr) / sizeof((arr)[0]) +                      \
   0 * sizeof(char[1 - 2 * __builtin_types_compatible_p( \
                               __typeof__(arr), __typeof__(&(arr)[0]))]))
#else
#define ZTD_COUNTOF(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

/* Brace initializer for a slice over a C array:
 *     IntSlice s = ZTD_ARR_VIEW(xs); */
#define ZTD_ARR_VIEW(arr) {(arr), ZTD_COUNTOF(arr)}

/* Bounds-checked element access (an lvalue). */
#define ZTD_ARR_AT(a, i) ((a)->ptr[ztd_arr__idx((i), (a)->len)])
#define ZTD_ARR_LAST(a) ((a)->ptr[ztd_arr__idx((a)->len - 1, (a)->len)])

/* for (T *it ...) over every element. T may itself be a pointer type. */
#define ZTD_ARR_FOREACH(T, it, a) \
  for (T* it = (a)->ptr; it && it < (a)->ptr + (a)->len; it++)

/* Point slice *dst at (*a)[start..end), clamped to the array. dst may be a. */
#define ZTD_SLICE_SET(dst, a, start, end)                               \
  (ZTD_ARR__SAME(dst, (a)->ptr),                                        \
   (void)((dst)->ptr = ZTD_ARR__TYPED(                                  \
              dst, ztd_arr__slice((a)->ptr, sizeof *(a)->ptr, (a)->len, \
                                  (start), (end), &(dst)->len))))

ZTD_ARR_DEF void ztd_arr_swap(void* ptr, size_t i, size_t j, size_t elem_size);
ZTD_ARR_DEF void ztd_arr_reverse(void* ptr, size_t n, size_t elem_size);
/* Linear search: index of the first element equal to *key, or ZTD_ARR_NPOS. */
ZTD_ARR_DEF size_t ztd_arr_find(const void* ptr,
                                size_t n,
                                size_t elem_size,
                                const void* key,
                                ZtdArrCmp cmp);
/* On a sorted array: index of the first element >= *key (n if none). */
ZTD_ARR_DEF size_t ztd_arr_lower_bound(const void* ptr,
                                       size_t n,
                                       size_t elem_size,
                                       const void* key,
                                       ZtdArrCmp cmp);
/* On a sorted array: index of the first element equal to *key, or
 * ZTD_ARR_NPOS. */
ZTD_ARR_DEF size_t ztd_arr_bsearch(const void* ptr,
                                   size_t n,
                                   size_t elem_size,
                                   const void* key,
                                   ZtdArrCmp cmp);
ZTD_ARR_DEF bool ztd_arr_is_sorted(const void* ptr,
                                   size_t n,
                                   size_t elem_size,
                                   ZtdArrCmp cmp);

/* Typed wrappers; keyp is a pointer to an element-typed key. The mutating
 * ones pass ptr without a cast, so a read-only array (ZtdStr) is diagnosed. */
#define ZTD_ARR_SORT(a, cmp) \
  ((a)->len ? qsort((a)->ptr, (a)->len, sizeof *(a)->ptr, (cmp)) : (void)0)
#define ZTD_ARR_REVERSE(a) ztd_arr_reverse((a)->ptr, (a)->len, sizeof *(a)->ptr)
#define ZTD_ARR_SWAP(a, i, j)                         \
  ztd_arr_swap((a)->ptr, ztd_arr__idx((i), (a)->len), \
               ztd_arr__idx((j), (a)->len), sizeof *(a)->ptr)
#define ZTD_ARR_FIND(a, keyp, cmp) \
  (ZTD_ARR__SAME(a, keyp),         \
   ztd_arr_find((a)->ptr, (a)->len, sizeof *(a)->ptr, (keyp), (cmp)))
#define ZTD_ARR_LOWER_BOUND(a, keyp, cmp) \
  (ZTD_ARR__SAME(a, keyp),                \
   ztd_arr_lower_bound((a)->ptr, (a)->len, sizeof *(a)->ptr, (keyp), (cmp)))
#define ZTD_ARR_BSEARCH(a, keyp, cmp) \
  (ZTD_ARR__SAME(a, keyp),            \
   ztd_arr_bsearch((a)->ptr, (a)->len, sizeof *(a)->ptr, (keyp), (cmp)))
#define ZTD_ARR_IS_SORTED(a, cmp) \
  ztd_arr_is_sorted((a)->ptr, (a)->len, sizeof *(a)->ptr, (cmp))

#ifdef __cplusplus
}
#endif

/* ---- Short names (see SHORT NAMES above) -------------------------------- */

#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_ARR_NO_SHORT_NAMES)
typedef ZtdArrCmp ArrCmp;

#define LIST(T) ZTD_LIST(T)
#define SLICE(T) ZTD_SLICE(T)
#define LIST_INIT ZTD_LIST_INIT
#define LIST_RESERVE(l, n) ZTD_LIST_RESERVE(l, n)
#define LIST_RESIZE(l, n) ZTD_LIST_RESIZE(l, n)
#define LIST_PUSH(l, ...) ZTD_LIST_PUSH(l, __VA_ARGS__)
#define LIST_APPEND(l, src, n) ZTD_LIST_APPEND(l, src, n)
#define LIST_EXTEND(l, other) ZTD_LIST_EXTEND(l, other)
#define LIST_INSERT(l, i, ...) ZTD_LIST_INSERT(l, i, __VA_ARGS__)
#define LIST_INSERT_N(l, i, s, n) ZTD_LIST_INSERT_N(l, i, s, n)
#define LIST_POP(l) ZTD_LIST_POP(l)
#define LIST_TRY_POP(l, out) ZTD_LIST_TRY_POP(l, out)
#define LIST_REMOVE(l, i) ZTD_LIST_REMOVE(l, i)
#define LIST_REMOVE_RANGE(l, i, n) ZTD_LIST_REMOVE_RANGE(l, i, n)
#define LIST_SWAP_REMOVE(l, i) ZTD_LIST_SWAP_REMOVE(l, i)
#define LIST_REMOVE_INTO(l, i, out) ZTD_LIST_REMOVE_INTO(l, i, out)
#define LIST_SWAP_REMOVE_INTO(l, i, out) ZTD_LIST_SWAP_REMOVE_INTO(l, i, out)
#define LIST_ADD(l) ZTD_LIST_ADD(l)
#define LIST_ADD_N(l, n) ZTD_LIST_ADD_N(l, n)
#define LIST_TRUNCATE(l, n) ZTD_LIST_TRUNCATE(l, n)
#define LIST_CLEAR(l) ZTD_LIST_CLEAR(l)
#define LIST_SHRINK(l) ZTD_LIST_SHRINK(l)
#define LIST_FREE(l) ZTD_LIST_FREE(l)

#define COUNTOF(arr) ZTD_COUNTOF(arr)
#define ARR_NPOS ZTD_ARR_NPOS
#define ARR_VIEW(arr) ZTD_ARR_VIEW(arr)
#define ARR_AT(a, i) ZTD_ARR_AT(a, i)
#define ARR_LAST(a) ZTD_ARR_LAST(a)
#define ARR_FOREACH(T, it, a) ZTD_ARR_FOREACH(T, it, a)
#define SLICE_SET(d, a, s, e) ZTD_SLICE_SET(d, a, s, e)
#define ARR_SORT(a, cmp) ZTD_ARR_SORT(a, cmp)
#define ARR_REVERSE(a) ZTD_ARR_REVERSE(a)
#define ARR_SWAP(a, i, j) ZTD_ARR_SWAP(a, i, j)
#define ARR_FIND(a, k, cmp) ZTD_ARR_FIND(a, k, cmp)
#define ARR_LOWER_BOUND(a, k, cmp) ZTD_ARR_LOWER_BOUND(a, k, cmp)
#define ARR_BSEARCH(a, k, cmp) ZTD_ARR_BSEARCH(a, k, cmp)
#define ARR_IS_SORTED(a, cmp) ZTD_ARR_IS_SORTED(a, cmp)

#define arr_swap ztd_arr_swap
#define arr_reverse ztd_arr_reverse
#define arr_find ztd_arr_find
#define arr_lower_bound ztd_arr_lower_bound
#define arr_bsearch ztd_arr_bsearch
#define arr_is_sorted ztd_arr_is_sorted
#endif

#endif /* ZTD_ARR_H */

/* ---- Arena integration (declarations) -----------------------------------
 * The integration sections are re-evaluated on every include, so they appear
 * as soon as both headers have been seen, whatever their order. */

#if defined(ZTD_ARENA_H) && !defined(ZTD_ARR__ARENA_DECL)
#define ZTD_ARR__ARENA_DECL

#ifdef __cplusplus
extern "C" {
#endif
/* Returns a; defined only by an implementation that saw ztd_arena.h, so using
 * ZTD_LIST_INIT_ARENA without arena support fails to link. */
ZTD_ARR_DEF ZtdArena* ztd_list__arena_linked(ZtdArena* a);
#ifdef __cplusplus
}
#endif

/* A list that grows inside arena *a (an automatic-storage initializer). */
#ifdef __cplusplus
#define ZTD_LIST_INIT_ARENA(a) {nullptr, 0, 0, ztd_list__arena_linked(a), false}
#else
#define ZTD_LIST_INIT_ARENA(a) {.arena = ztd_list__arena_linked(a)}
#endif

/* Copy the elements of any array into the arena; returns a pointer to the
 * element type where the compiler supports it (void * otherwise), NULL on
 * failure, e.g.  int *frozen = ZTD_ARR_ARENA_DUP(&arena, &list); */
#define ZTD_ARR_ARENA_DUP(arena, a)                                       \
  ZTD_ARR__TYPED(                                                         \
      a, ztd_arena_memdup((arena), (a)->ptr, (a)->len * sizeof *(a)->ptr, \
                          ZTD_ARR__ALIGNOF_EXPR(*(a)->ptr)))

#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_ARR_NO_SHORT_NAMES)
#define LIST_INIT_ARENA(a) ZTD_LIST_INIT_ARENA(a)
#define ARR_ARENA_DUP(arena, a) ZTD_ARR_ARENA_DUP(arena, a)
#endif
#endif /* arena declarations */

/* ---- String integration (declarations) ---------------------------------- */

#if defined(ZTD_STR_H) && !defined(ZTD_ARR__STR_DECL)
#define ZTD_ARR__STR_DECL

typedef ZTD_LIST(ZtdStr) ZtdStrList;

#ifdef __cplusplus
extern "C" {
#endif
/* Append every field of s split on sep (ztd_str_split semantics). */
ZTD_ARR_DEF bool ztd_strlist_split(ZtdStrList* out, ZtdStr s, ZtdStr sep);
/* Append every line of s (ztd_str_next_line semantics). */
ZTD_ARR_DEF bool ztd_strlist_lines(ZtdStrList* out, ZtdStr s);
/* Append the elements joined by sep to b. */
ZTD_ARR_DEF bool ztd_strlist_join(ZtdStrBuf* b,
                                  const ZtdStrList* l,
                                  ZtdStr sep);
#ifdef __cplusplus
}
#endif

#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_ARR_NO_SHORT_NAMES)
typedef ZtdStrList StrList;
#define strlist_split ztd_strlist_split
#define strlist_lines ztd_strlist_lines
#define strlist_join ztd_strlist_join
#endif
#endif /* string declarations */

/* ======================================================================== */

#ifdef ZTD_ARR_IMPLEMENTATION
#ifndef ZTD_ARR_IMPLEMENTATION_DONE
#define ZTD_ARR_IMPLEMENTATION_DONE

#ifdef __cplusplus
#error "ztd_arr.h: compile the implementation as C (C11)"
#endif

#include <string.h>

#if defined(ZTD_ARR_MALLOC) != defined(ZTD_ARR_FREE) || \
    defined(ZTD_ARR_MALLOC) != defined(ZTD_ARR_REALLOC)
#error \
    "ztd_arr.h: define all of ZTD_ARR_MALLOC, ZTD_ARR_REALLOC and ZTD_ARR_FREE, or none"
#endif
#ifndef ZTD_ARR_MALLOC
#if defined(ZTD_MALLOC) && defined(ZTD_REALLOC) && defined(ZTD_FREE)
#define ZTD_ARR_MALLOC(sz) ZTD_MALLOC(sz)
#define ZTD_ARR_REALLOC(p, sz) ZTD_REALLOC(p, sz)
#define ZTD_ARR_FREE(p) ZTD_FREE(p)
#else
#define ZTD_ARR_MALLOC(sz) malloc(sz)
#define ZTD_ARR_REALLOC(p, sz) realloc((p), (sz))
#define ZTD_ARR_FREE(p) free(p)
#endif
#endif

#define ZTD_ARR__MIN_CAP 8

/* Arena growth hook. A tentative definition: it stays NULL unless the arena
 * integration below is compiled into this file, which initialises it. */
static void* (*ztd_arr__arena_realloc)(struct ZtdArena* a,
                                       void* ptr,
                                       size_t old_size,
                                       size_t new_size,
                                       size_t align);

/* ---- Lists -------------------------------------------------------------- */

ZTD_ARR_DEF void* ztd_list__reserve(void* ptr,
                                    size_t len,
                                    size_t* cap,
                                    size_t extra,
                                    size_t elem_size,
                                    size_t align,
                                    struct ZtdArena* arena,
                                    bool* failed) {
  if (*failed)
    return ptr;
  if (extra <= *cap - len)
    return ptr;
  ZTD_ARR_ASSERT(elem_size > 0);
  if (extra > SIZE_MAX - len)
    goto fail;

  size_t need = len + extra;
  size_t max = SIZE_MAX / elem_size;
  if (need > max)
    goto fail;
  size_t new_cap = *cap ? *cap : ZTD_ARR__MIN_CAP;
  while (new_cap < need) {
    if (new_cap > max / 2) {
      new_cap = need;
      break;
    }
    new_cap *= 2;
  }

  void* p;
  if (arena) {
    ZTD_ARR_ASSERT(ztd_arr__arena_realloc &&
                   "ztd_arr: arena-backed list, but the implementation did not "
                   "include ztd_arena.h");
    if (!ztd_arr__arena_realloc)
      goto fail;
    p = ztd_arr__arena_realloc(arena, ptr, *cap * elem_size,
                               new_cap * elem_size, align);
  } else {
    (void)align; /* malloc's alignment is enough for any object type */
    p = ptr ? ZTD_ARR_REALLOC(ptr, new_cap * elem_size)
            : ZTD_ARR_MALLOC(new_cap * elem_size);
  }
  if (!p)
    goto fail;
  *cap = new_cap;
  return p;

fail:
  *failed = true;
  return ptr;
}

ZTD_ARR_DEF void* ztd_list__insert(void* ptr,
                                   size_t* len,
                                   size_t* cap,
                                   size_t idx,
                                   const void* src,
                                   size_t n,
                                   size_t elem_size,
                                   size_t align,
                                   struct ZtdArena* arena,
                                   bool* failed) {
  ZTD_ARR_ASSERT(idx <= *len && "ztd_arr: insert index out of range");
  if (idx > *len)
    idx = *len;
  if (*failed)
    return ptr;
  if (n == 0)
    return ptr;

  /* src may point into our own storage, which reserve can move. */
  size_t self_off = SIZE_MAX;
  if (ptr && src) {
    uintptr_t lo = (uintptr_t)ptr, hi = lo + *cap * elem_size,
              sp = (uintptr_t)src;
    if (sp >= lo && sp < hi)
      self_off = (size_t)(sp - lo);
  }

  ptr = ztd_list__reserve(ptr, *len, cap, n, elem_size, align, arena, failed);
  if (*failed)
    return ptr;

  unsigned char* b = (unsigned char*)ptr;
  size_t gap = idx * elem_size, bytes = n * elem_size;
  size_t tail = (*len - idx) * elem_size;
  if (tail)
    memmove(b + gap + bytes, b + gap, tail);

  if (!src) {
    memset(b + gap, 0, bytes);
  } else if (self_off == SIZE_MAX) {
    memcpy(b + gap, src, bytes);
  } else if (self_off + bytes <= gap) { /* source was before the gap: unmoved */
    memcpy(b + gap, b + self_off, bytes);
  } else if (self_off >= gap) { /* source was shifted up by `bytes`   */
    memcpy(b + gap, b + self_off + bytes, bytes);
  } else { /* source straddled the gap           */
    size_t first = gap - self_off;
    memcpy(b + gap, b + self_off, first);
    memcpy(b + gap + first, b + gap + bytes, bytes - first);
  }
  *len += n;
  return ptr;
}

ZTD_ARR_DEF void* ztd_list__resize(void* ptr,
                                   size_t* len,
                                   size_t* cap,
                                   size_t n,
                                   size_t elem_size,
                                   size_t align,
                                   struct ZtdArena* arena,
                                   bool* failed) {
  if (*failed)
    return ptr;
  if (n <= *len) {
    *len = n;
    return ptr;
  }
  return ztd_list__insert(ptr, len, cap, *len, NULL, n - *len, elem_size, align,
                          arena, failed);
}

ZTD_ARR_DEF void ztd_list__rotate_in(void* ptr,
                                     size_t* len_p,
                                     size_t cap,
                                     size_t idx,
                                     size_t elem_size) {
  ZTD_ARR_ASSERT(idx <= *len_p && "ztd_arr: insert index out of range");
  size_t len = ++*len_p;
  if (idx + 1 >= len)
    return; /* appending (or, with asserts off, out of range: append) */
  unsigned char* b = (unsigned char*)ptr;
  if (cap > len) {
    /* Use the spare slot past the end as scratch. */
    unsigned char* tmp = b + len * elem_size;
    memcpy(tmp, b + (len - 1) * elem_size, elem_size);
    memmove(b + (idx + 1) * elem_size, b + idx * elem_size,
            (len - 1 - idx) * elem_size);
    memcpy(b + idx * elem_size, tmp, elem_size);
  } else {
    /* Rotate right by one: reverse the range, then everything after idx. */
    ztd_arr_reverse(b + idx * elem_size, len - idx, elem_size);
    ztd_arr_reverse(b + (idx + 1) * elem_size, len - idx - 1, elem_size);
  }
}

ZTD_ARR_DEF void ztd_list__remove(void* ptr,
                                  size_t* len,
                                  size_t idx,
                                  size_t n,
                                  size_t elem_size) {
  ZTD_ARR_ASSERT(idx <= *len && n <= *len - idx &&
                 "ztd_arr: remove range out of range");
  if (idx > *len)
    return;
  if (n > *len - idx)
    n = *len - idx;
  if (n == 0)
    return;
  unsigned char* b = (unsigned char*)ptr;
  size_t tail = (*len - idx - n) * elem_size;
  if (tail)
    memmove(b + idx * elem_size, b + (idx + n) * elem_size, tail);
  *len -= n;
}

ZTD_ARR_DEF void ztd_list__swap_remove(void* ptr,
                                       size_t* len,
                                       size_t idx,
                                       size_t elem_size) {
  ZTD_ARR_ASSERT(idx < *len && "ztd_arr: index out of range");
  if (idx >= *len)
    return;
  size_t last = *len - 1;
  if (idx != last) {
    unsigned char* b = (unsigned char*)ptr;
    memcpy(b + idx * elem_size, b + last * elem_size, elem_size);
  }
  *len = last;
}

ZTD_ARR_DEF bool ztd_list__remove_into(void* ptr,
                                       size_t* len,
                                       size_t idx,
                                       void* out,
                                       bool swap,
                                       size_t elem_size) {
  ZTD_ARR_ASSERT(idx < *len && "ztd_arr: index out of range");
  if (idx >= *len)
    return false;
  memcpy(out, (unsigned char*)ptr + idx * elem_size, elem_size);
  if (swap)
    ztd_list__swap_remove(ptr, len, idx, elem_size);
  else
    ztd_list__remove(ptr, len, idx, 1, elem_size);
  return true;
}

ZTD_ARR_DEF void* ztd_list__shrink(void* ptr,
                                   size_t len,
                                   size_t* cap,
                                   size_t elem_size,
                                   struct ZtdArena* arena) {
  if (arena || len == *cap)
    return ptr;
  if (len == 0) {
    ZTD_ARR_FREE(ptr);
    *cap = 0;
    return NULL;
  }
  void* p = ZTD_ARR_REALLOC(ptr, len * elem_size);
  if (!p)
    return ptr; /* keeping the bigger block is fine */
  *cap = len;
  return p;
}

ZTD_ARR_DEF void ztd_list__free(void* ptr, struct ZtdArena* arena) {
  if (!arena)
    ZTD_ARR_FREE(ptr);
}

/* ---- Arrays ------------------------------------------------------------- */

ZTD_ARR_DEF void ztd_arr_swap(void* ptr, size_t i, size_t j, size_t elem_size) {
  if (i == j)
    return;
  unsigned char* a = (unsigned char*)ptr + i * elem_size;
  unsigned char* b = (unsigned char*)ptr + j * elem_size;
  unsigned char tmp[64];
  while (elem_size) {
    size_t k = elem_size < sizeof tmp ? elem_size : sizeof tmp;
    memcpy(tmp, a, k);
    memcpy(a, b, k);
    memcpy(b, tmp, k);
    a += k;
    b += k;
    elem_size -= k;
  }
}

ZTD_ARR_DEF void ztd_arr_reverse(void* ptr, size_t n, size_t elem_size) {
  if (n < 2)
    return;
  for (size_t i = 0, j = n - 1; i < j; i++, j--)
    ztd_arr_swap(ptr, i, j, elem_size);
}

ZTD_ARR_DEF size_t ztd_arr_find(const void* ptr,
                                size_t n,
                                size_t elem_size,
                                const void* key,
                                ZtdArrCmp cmp) {
  const unsigned char* b = (const unsigned char*)ptr;
  for (size_t i = 0; i < n; i++) {
    if (cmp(b + i * elem_size, key) == 0)
      return i;
  }
  return ZTD_ARR_NPOS;
}

ZTD_ARR_DEF size_t ztd_arr_lower_bound(const void* ptr,
                                       size_t n,
                                       size_t elem_size,
                                       const void* key,
                                       ZtdArrCmp cmp) {
  const unsigned char* b = (const unsigned char*)ptr;
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cmp(b + mid * elem_size, key) < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

ZTD_ARR_DEF size_t ztd_arr_bsearch(const void* ptr,
                                   size_t n,
                                   size_t elem_size,
                                   const void* key,
                                   ZtdArrCmp cmp) {
  size_t i = ztd_arr_lower_bound(ptr, n, elem_size, key, cmp);
  if (i < n && cmp((const unsigned char*)ptr + i * elem_size, key) == 0)
    return i;
  return ZTD_ARR_NPOS;
}

ZTD_ARR_DEF bool ztd_arr_is_sorted(const void* ptr,
                                   size_t n,
                                   size_t elem_size,
                                   ZtdArrCmp cmp) {
  const unsigned char* b = (const unsigned char*)ptr;
  for (size_t i = 1; i < n; i++) {
    if (cmp(b + (i - 1) * elem_size, b + i * elem_size) > 0)
      return false;
  }
  return true;
}

#endif /* ZTD_ARR_IMPLEMENTATION_DONE */

/* ---- Arena integration (implementation) --------------------------------- */

#if defined(ZTD_ARENA_H) && !defined(ZTD_ARR__ARENA_IMPL)
#define ZTD_ARR__ARENA_IMPL

static void* (*ztd_arr__arena_realloc)(struct ZtdArena* a,
                                       void* ptr,
                                       size_t old_size,
                                       size_t new_size,
                                       size_t align) = ztd_arena_realloc;

ZTD_ARR_DEF ZtdArena* ztd_list__arena_linked(ZtdArena* a) {
  return a;
}
#endif /* arena implementation */

/* ---- String integration (implementation) -------------------------------- */

#if defined(ZTD_STR_H) && !defined(ZTD_ARR__STR_IMPL)
#define ZTD_ARR__STR_IMPL

ZTD_ARR_DEF bool ztd_strlist_split(ZtdStrList* out, ZtdStr s, ZtdStr sep) {
  size_t start = out->len;
  ZtdStrSplit it = ztd_str_split(s, sep);
  ZtdStr tok;
  while (ztd_str_split_next(&it, &tok)) {
    if (!ZTD_LIST_PUSH(out, tok)) {
      ZTD_LIST_TRUNCATE(out, start);
      return false;
    }
  }
  return !out->failed;
}

ZTD_ARR_DEF bool ztd_strlist_lines(ZtdStrList* out, ZtdStr s) {
  size_t start = out->len;
  ZtdStr line;
  while (ztd_str_next_line(&s, &line)) {
    if (!ZTD_LIST_PUSH(out, line)) {
      ZTD_LIST_TRUNCATE(out, start);
      return false;
    }
  }
  return !out->failed;
}

ZTD_ARR_DEF bool ztd_strlist_join(ZtdStrBuf* b,
                                  const ZtdStrList* l,
                                  ZtdStr sep) {
  return ztd_sb_append_join(b, l->ptr, l->len, sep);
}
#endif /* string implementation */
#endif /* ZTD_ARR_IMPLEMENTATION */
