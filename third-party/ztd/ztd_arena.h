/* ztd_arena.h - single-header bump/arena allocator with mark & rewind (C11)
 *
 * USAGE
 *   In exactly one .c file:
 *       #define ZTD_ARENA_IMPLEMENTATION
 *       #include "ztd_arena.h"
 *   Everywhere else just #include "ztd_arena.h".
 *   Define ZTD_ARENA_STATIC before including to make every function static.
 *   The implementation must be compiled as C; the declarations and macros
 *   also work from C++.
 *
 *   ZtdArena a = ZTD_ARENA_INIT;                      // or ztd_arena_init(size)
 *   int *xs = ZTD_ARENA_NEW_ARRAY(&a, int, 100);      // typed, zeroed, aligned
 *   ZtdArenaMark m = ztd_arena_mark(&a);
 *   char *tmp = ztd_arena_alloc(&a, 4096, 1);         // raw, uninitialised
 *   ztd_arena_rewind(&a, m);                          // tmp is gone, xs is valid
 *   ztd_arena_free(&a);
 *
 * SHORT NAMES
 *   By default the header also defines unprefixed aliases:
 *       Arena, ArenaMark, ARENA_INIT, arena_init, arena_alloc, ...,
 *       ARENA_NEW, ARENA_NEW_ARRAY
 *   (every ztd_arena_* function is available as arena_*). Functions are aliased
 *   with object-like macros, so they still work as function pointers.
 *   To get only the ztd_ names, define before including:
 *       ZTD_NO_SHORT_NAMES          for every ztd_* header, or
 *       ZTD_ARENA_NO_SHORT_NAMES    for this header only.
 *   Configuration macros (ZTD_ARENA_IMPLEMENTATION etc.) are always prefixed.
 *
 * SEMANTICS
 *   - Memory is handed out from linked blocks by bumping a pointer; nothing is
 *     freed individually.
 *   - A zero-initialised ZtdArena ({0}, ZTD_ARENA_INIT) is a valid empty arena
 *     with the default block size.
 *   - ztd_arena_rewind(m) releases everything allocated (or grown in place by
 *     ztd_arena_realloc) after ztd_arena_mark() returned m. Marks are LIFO:
 *     rewinding to an older mark invalidates every newer mark. Rewinding to a
 *     mark from another arena, or to an invalidated mark, is a bug (caught by
 *     assert in debug builds).
 *   - ztd_arena_reset() releases everything but keeps the largest block.
 *   - ztd_arena_free() returns all blocks to the system; the arena can be reused.
 *   - Not thread-safe; use one arena per thread.
 *
 * CONFIGURATION (define before including the implementation)
 *   ZTD_ARENA_MALLOC(sz) / ZTD_ARENA_FREE(p)   backing allocator; define both
 *                                              or neither. Defaults to
 *                                              ZTD_MALLOC / ZTD_FREE if those
 *                                              are defined, else malloc/free.
 *   ZTD_ARENA_ASSERT(x)                        default ZTD_ASSERT(x) if
 *                                              defined, else assert(x)
 *   ZTD_ARENA_DEFAULT_BLOCK_SIZE               default 64 KiB
 *   ZTD_ARENA_NO_DEBUG_FILL                    don't fill released memory with
 *                                              0xCD in debug (!NDEBUG) builds
 *
 *   Block sizes are the full size passed to malloc (header included), so the
 *   default blocks are exactly 64 KiB.
 *
 *   Under AddressSanitizer, unused and released arena memory is poisoned, so
 *   use-after-rewind and reads past an allocation are reported. (GCC may skip
 *   re-checking a pointer it already checked earlier in the same function, so a
 *   stale access right after a rewind in that function can go unreported.)
 *
 *   Requires C11 (_Alignas/_Alignof, flexible array member).
 *
 * LICENSE: public domain / MIT-0. Do what you like.
 */
#ifndef ZTD_ARENA_H
#define ZTD_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef ZTD_ARENA_STATIC
#if defined(__GNUC__)
#define ZTD_ARENA_DEF static __attribute__((unused))
#else
#define ZTD_ARENA_DEF static
#endif
#else
#define ZTD_ARENA_DEF extern
#endif

#ifdef __cplusplus
#define ZTD_ARENA__ALIGNOF(T) alignof(T)
#else
#define ZTD_ARENA__ALIGNOF(T) _Alignof(T)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZtdArenaBlock ZtdArenaBlock;

typedef struct ZtdArena {
    ZtdArenaBlock *cur;     /* newest block; older blocks hang off ->prev   */
    size_t block_size;      /* malloc size of a regular block (0 = default) */
    size_t blocks_made;     /* id source, so stale marks can be detected    */
} ZtdArena;

typedef struct ZtdArenaMark {
    ZtdArenaBlock *block;   /* block that was current when the mark was taken */
    size_t block_id;        /* its id (guards against address reuse)         */
    size_t used;            /* its fill level at that moment                 */
} ZtdArenaMark;

#ifdef __cplusplus
#define ZTD_ARENA_INIT {}
#else
#define ZTD_ARENA_INIT {0}
#endif

/* Make an empty arena whose regular blocks are `block_size` bytes (0 = default).
 * No memory is allocated until the first allocation. */
ZTD_ARENA_DEF ZtdArena ztd_arena_init(size_t block_size);

/* Allocate `size` uninitialised bytes aligned to `align` (a power of two).
 * Returns NULL on out-of-memory or overflow. size 0 returns a valid pointer
 * that must not be dereferenced (it may equal the next allocation). */
ZTD_ARENA_DEF void *ztd_arena_alloc(ZtdArena *a, size_t size, size_t align);

/* Same, but zero-filled. */
ZTD_ARENA_DEF void *ztd_arena_alloc_zero(ZtdArena *a, size_t size, size_t align);

/* Allocate `count` elements of `elem_size` bytes with overflow checking. */
ZTD_ARENA_DEF void *ztd_arena_alloc_array(ZtdArena *a, size_t count, size_t elem_size, size_t align);
ZTD_ARENA_DEF void *ztd_arena_alloc_array_zero(ZtdArena *a, size_t count, size_t elem_size, size_t align);

/* Resize an allocation of `old_size` bytes. If it is the most recent
 * allocation and there is room, it grows or shrinks in place; otherwise a
 * larger request copies into a new allocation (the old one stays valid until
 * the arena releases it) and a smaller one returns ptr unchanged.
 * ptr NULL behaves like ztd_arena_alloc. Returns NULL on failure, in which
 * case ptr is untouched. */
ZTD_ARENA_DEF void *ztd_arena_realloc(ZtdArena *a, void *ptr, size_t old_size, size_t new_size, size_t align);

/* Copy a NUL-terminated string (NULL -> "") / a byte range into the arena. */
ZTD_ARENA_DEF char *ztd_arena_strdup(ZtdArena *a, const char *s);
ZTD_ARENA_DEF void *ztd_arena_memdup(ZtdArena *a, const void *p, size_t size, size_t align);

ZTD_ARENA_DEF ZtdArenaMark ztd_arena_mark(const ZtdArena *a);
ZTD_ARENA_DEF void ztd_arena_rewind(ZtdArena *a, ZtdArenaMark m);
ZTD_ARENA_DEF void ztd_arena_reset(ZtdArena *a);
ZTD_ARENA_DEF void ztd_arena_free(ZtdArena *a);

/* Bytes handed out across all blocks (including alignment padding). */
ZTD_ARENA_DEF size_t ztd_arena_used(const ZtdArena *a);
/* Payload bytes held across all blocks, used or not. */
ZTD_ARENA_DEF size_t ztd_arena_capacity(const ZtdArena *a);

#ifdef __cplusplus
}
#endif

/* Typed helpers; both zero-fill. Use ztd_arena_alloc* for raw memory. */
#define ZTD_ARENA_NEW(a, T) \
    ((T *)ztd_arena_alloc_zero((a), sizeof(T), ZTD_ARENA__ALIGNOF(T)))
#define ZTD_ARENA_NEW_ARRAY(a, T, n) \
    ((T *)ztd_arena_alloc_array_zero((a), (n), sizeof(T), ZTD_ARENA__ALIGNOF(T)))

/* Short names (see SHORT NAMES above). */
#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_ARENA_NO_SHORT_NAMES)
typedef ZtdArena     Arena;
typedef ZtdArenaMark ArenaMark;
#define ARENA_INIT             ZTD_ARENA_INIT
#define arena_init             ztd_arena_init
#define arena_alloc            ztd_arena_alloc
#define arena_alloc_zero       ztd_arena_alloc_zero
#define arena_alloc_array      ztd_arena_alloc_array
#define arena_alloc_array_zero ztd_arena_alloc_array_zero
#define arena_realloc          ztd_arena_realloc
#define arena_strdup           ztd_arena_strdup
#define arena_memdup           ztd_arena_memdup
#define arena_mark             ztd_arena_mark
#define arena_rewind           ztd_arena_rewind
#define arena_reset            ztd_arena_reset
#define arena_free             ztd_arena_free
#define arena_used             ztd_arena_used
#define arena_capacity         ztd_arena_capacity
#define ARENA_NEW              ZTD_ARENA_NEW
#define ARENA_NEW_ARRAY        ZTD_ARENA_NEW_ARRAY
#endif

#endif /* ZTD_ARENA_H */

/* ======================================================================== */

#ifdef ZTD_ARENA_IMPLEMENTATION
#ifndef ZTD_ARENA_IMPLEMENTATION_DONE
#define ZTD_ARENA_IMPLEMENTATION_DONE

#ifdef __cplusplus
#error "ztd_arena.h: compile the implementation as C (C11)"
#endif

#include <string.h>

#if defined(ZTD_ARENA_MALLOC) != defined(ZTD_ARENA_FREE)
#error "ztd_arena.h: define both ZTD_ARENA_MALLOC and ZTD_ARENA_FREE, or neither"
#endif
#ifndef ZTD_ARENA_MALLOC
#if defined(ZTD_MALLOC) && defined(ZTD_FREE)
#define ZTD_ARENA_MALLOC(sz) ZTD_MALLOC(sz)
#define ZTD_ARENA_FREE(p)    ZTD_FREE(p)
#else
#include <stdlib.h>
#define ZTD_ARENA_MALLOC(sz) malloc(sz)
#define ZTD_ARENA_FREE(p)    free(p)
#endif
#endif

#ifndef ZTD_ARENA_ASSERT
#ifdef ZTD_ASSERT
#define ZTD_ARENA_ASSERT(x) ZTD_ASSERT(x)
#else
#include <assert.h>
#define ZTD_ARENA_ASSERT(x) assert(x)
#endif
#endif

#ifndef ZTD_ARENA_DEFAULT_BLOCK_SIZE
#define ZTD_ARENA_DEFAULT_BLOCK_SIZE (64u * 1024u)
#endif

/* ---- AddressSanitizer integration -------------------------------------- */
#if defined(__SANITIZE_ADDRESS__)
#define ZTD_ARENA__ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ZTD_ARENA__ASAN 1
#endif
#endif

#ifdef ZTD_ARENA__ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define ZTD_ARENA__POISON(p, n)   __asan_poison_memory_region((p), (n))
#define ZTD_ARENA__UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
/* GCC warns about passing never-written memory to a const pointer parameter;
 * poisoning fresh memory is exactly what we want here. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#define ZTD_ARENA__POP_DIAGNOSTICS 1
#endif
#else
#define ZTD_ARENA__POISON(p, n)   ((void)(p), (void)(n))
#define ZTD_ARENA__UNPOISON(p, n) ((void)(p), (void)(n))
#endif

/* Fill released memory in debug builds so stale reads stand out. */
#if !defined(NDEBUG) && !defined(ZTD_ARENA_NO_DEBUG_FILL) && !defined(ZTD_ARENA__ASAN)
#define ZTD_ARENA__SCRIBBLE(p, n) memset((p), 0xCD, (n))
#else
#define ZTD_ARENA__SCRIBBLE(p, n) ((void)(p), (void)(n))
#endif

struct ZtdArenaBlock {
    ZtdArenaBlock *prev;
    size_t id;
    size_t cap;    /* payload bytes */
    size_t used;   /* payload bytes handed out */
    _Alignas(max_align_t) unsigned char data[];
};

/* Payload of a regular block: the configured malloc size minus the header. */
static size_t ztd_arena__regular_cap(const ZtdArena *a) {
    size_t bs = a->block_size ? a->block_size : ZTD_ARENA_DEFAULT_BLOCK_SIZE;
    return bs > 2 * sizeof(ZtdArenaBlock) ? bs - sizeof(ZtdArenaBlock) : bs;
}

static ZtdArenaBlock *ztd_arena__block_new(ZtdArena *a, size_t cap) {
    if (cap > SIZE_MAX - sizeof(ZtdArenaBlock)) return NULL;
    ZtdArenaBlock *b = (ZtdArenaBlock *)ZTD_ARENA_MALLOC(sizeof(ZtdArenaBlock) + cap);
    if (!b) return NULL;
    b->prev = a->cur;
    b->id = ++a->blocks_made;
    b->cap = cap;
    b->used = 0;
    ZTD_ARENA__POISON(b->data, cap);
    return b;
}

static void ztd_arena__block_free(ZtdArenaBlock *b) {
    ZTD_ARENA__UNPOISON(b->data, b->cap);
    ZTD_ARENA_FREE(b);
}

/* Give back b's bytes from `keep` up to its fill level. */
static void ztd_arena__release(ZtdArenaBlock *b, size_t keep) {
    size_t released = b->used - keep;
    ZTD_ARENA__SCRIBBLE(b->data + keep, released);
    ZTD_ARENA__POISON(b->data + keep, released);
    b->used = keep;
}

/* Try to carve from block b. Returns NULL if it doesn't fit. */
static void *ztd_arena__carve(ZtdArenaBlock *b, size_t size, size_t align) {
    uintptr_t base = (uintptr_t)b->data;
    uintptr_t cur = base + b->used;
    uintptr_t p = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
    if (p < cur) return NULL;                 /* address overflow */
    size_t off = (size_t)(p - base);
    if (off > b->cap || size > b->cap - off) return NULL;
    b->used = off + size;
    ZTD_ARENA__UNPOISON((void *)p, size);
    return (void *)p;
}

ZTD_ARENA_DEF ZtdArena ztd_arena_init(size_t block_size) {
    ZtdArena a;
    a.cur = NULL;
    a.block_size = block_size;
    a.blocks_made = 0;
    return a;
}

ZTD_ARENA_DEF void *ztd_arena_alloc(ZtdArena *a, size_t size, size_t align) {
    ZTD_ARENA_ASSERT(align != 0 && (align & (align - 1)) == 0);

    if (a->cur) {
        void *p = ztd_arena__carve(a->cur, size, align);
        if (p) return p;
    }

    /* Slow path: new block guaranteed to fit this request at any base address. */
    if (size > SIZE_MAX - (align - 1)) return NULL;
    size_t need = size + (align - 1);
    size_t regular = ztd_arena__regular_cap(a);
    size_t cap = need > regular ? need : regular;

    ZtdArenaBlock *b = ztd_arena__block_new(a, cap);
    if (!b) return NULL;
    a->cur = b;

    void *p = ztd_arena__carve(b, size, align);
    ZTD_ARENA_ASSERT(p != NULL);
    return p;
}

ZTD_ARENA_DEF void *ztd_arena_alloc_zero(ZtdArena *a, size_t size, size_t align) {
    void *p = ztd_arena_alloc(a, size, align);
    if (p) memset(p, 0, size);
    return p;
}

ZTD_ARENA_DEF void *ztd_arena_alloc_array(ZtdArena *a, size_t count, size_t elem_size, size_t align) {
    if (elem_size != 0 && count > SIZE_MAX / elem_size) return NULL;
    return ztd_arena_alloc(a, count * elem_size, align);
}

ZTD_ARENA_DEF void *ztd_arena_alloc_array_zero(ZtdArena *a, size_t count, size_t elem_size, size_t align) {
    if (elem_size != 0 && count > SIZE_MAX / elem_size) return NULL;
    return ztd_arena_alloc_zero(a, count * elem_size, align);
}

ZTD_ARENA_DEF void *ztd_arena_realloc(ZtdArena *a, void *ptr, size_t old_size, size_t new_size, size_t align) {
    if (!ptr) return ztd_arena_alloc(a, new_size, align);

    ZtdArenaBlock *b = a->cur;
    if (b) {
        uintptr_t base = (uintptr_t)b->data, p = (uintptr_t)ptr;
        if (p >= base && p - base <= b->used && b->used - (p - base) == old_size) {
            /* ptr is the most recent allocation in the current block. */
            size_t off = (size_t)(p - base);
            if (new_size <= b->cap - off) {
                if (new_size < old_size) {
                    ztd_arena__release(b, off + new_size);
                } else {
                    ZTD_ARENA__UNPOISON(b->data + b->used, new_size - old_size);
                    b->used = off + new_size;
                }
                return ptr;
            }
        }
    }
    if (new_size <= old_size) return ptr;

    void *q = ztd_arena_alloc(a, new_size, align);
    if (q && old_size) memcpy(q, ptr, old_size);
    return q;
}

ZTD_ARENA_DEF void *ztd_arena_memdup(ZtdArena *a, const void *src, size_t size, size_t align) {
    void *p = ztd_arena_alloc(a, size, align);
    if (p && size) memcpy(p, src, size);
    return p;
}

ZTD_ARENA_DEF char *ztd_arena_strdup(ZtdArena *a, const char *s) {
    if (!s) s = "";
    return (char *)ztd_arena_memdup(a, s, strlen(s) + 1, 1);
}

ZTD_ARENA_DEF ZtdArenaMark ztd_arena_mark(const ZtdArena *a) {
    ZtdArenaMark m;
    m.block = a->cur;
    m.block_id = a->cur ? a->cur->id : 0;
    m.used = a->cur ? a->cur->used : 0;
    return m;
}

ZTD_ARENA_DEF void ztd_arena_rewind(ZtdArena *a, ZtdArenaMark m) {
    /* Drop every block created after the mark. For a mark taken on an empty
     * arena, keep the oldest block for reuse instead of freeing it. */
    while (a->cur != m.block) {
        ZTD_ARENA_ASSERT(a->cur != NULL && "ztd_arena_rewind: mark is not from this arena or was invalidated");
        if (!a->cur) return;
        if (!m.block && !a->cur->prev) {
            ztd_arena__release(a->cur, 0);
            return;
        }
        ZtdArenaBlock *prev = a->cur->prev;
        ztd_arena__block_free(a->cur);
        a->cur = prev;
    }
    if (!a->cur) return;

    ZtdArenaBlock *b = a->cur;
    ZTD_ARENA_ASSERT(b->id == m.block_id && "ztd_arena_rewind: mark is not from this arena or was invalidated");
    ZTD_ARENA_ASSERT(m.used <= b->used && "ztd_arena_rewind: mark was invalidated by an earlier rewind");
    if (b->id != m.block_id || m.used > b->used) return;
    ztd_arena__release(b, m.used);
}

ZTD_ARENA_DEF void ztd_arena_reset(ZtdArena *a) {
    if (!a->cur) return;
    /* Keep only the largest block. */
    ZtdArenaBlock *keep = a->cur;
    for (ZtdArenaBlock *b = a->cur->prev; b; b = b->prev) {
        if (b->cap > keep->cap) keep = b;
    }
    while (a->cur) {
        ZtdArenaBlock *prev = a->cur->prev;
        if (a->cur != keep) ztd_arena__block_free(a->cur);
        a->cur = prev;
    }
    keep->prev = NULL;
    ztd_arena__release(keep, 0);
    a->cur = keep;
}

ZTD_ARENA_DEF void ztd_arena_free(ZtdArena *a) {
    while (a->cur) {
        ZtdArenaBlock *prev = a->cur->prev;
        ztd_arena__block_free(a->cur);
        a->cur = prev;
    }
}

ZTD_ARENA_DEF size_t ztd_arena_used(const ZtdArena *a) {
    size_t total = 0;
    for (const ZtdArenaBlock *b = a->cur; b; b = b->prev) total += b->used;
    return total;
}

ZTD_ARENA_DEF size_t ztd_arena_capacity(const ZtdArena *a) {
    size_t total = 0;
    for (const ZtdArenaBlock *b = a->cur; b; b = b->prev) total += b->cap;
    return total;
}

#ifdef ZTD_ARENA__POP_DIAGNOSTICS
#pragma GCC diagnostic pop
#endif

#endif /* ZTD_ARENA_IMPLEMENTATION_DONE */
#endif /* ZTD_ARENA_IMPLEMENTATION */

/* ---- Integration ----------------------------------------------------------
 * ztd_str.h and ztd_arr.h add arena support once they see ztd_arena.h. If
 * either was included before this header, include it again so it can catch
 * up; the order you include the ztd headers in does not matter. */
#ifdef ZTD_STR_H
#include "ztd_str.h"
#endif
#ifdef ZTD_ARR_H
#include "ztd_arr.h"
#endif
