/* ztd_str.h - single-header string library: views, builders, parsing (C11)
 *
 * USAGE
 *   In exactly one .c file:
 *       #define ZTD_STR_IMPLEMENTATION
 *       #include "ztd_str.h"
 *   Everywhere else just #include "ztd_str.h".
 *   Define ZTD_STR_STATIC before including to make every function static.
 *   The implementation must be compiled as C; the declarations and macros
 *   also work from C++.
 *
 * THREE PIECES
 *   ZtdStr      A non-owning view: pointer + length. Not NUL-terminated, never
 *               allocates, passed by value. Slicing, searching, trimming,
 *               splitting and number parsing all work on views.
 *
 *   ZtdStrBuf   An owning, growable, always NUL-terminated buffer for building
 *               strings: append, printf-append, join, replace. malloc-backed
 *               by default, or arena-backed (see below). Same layout and
 *               error convention as ZTD_LIST(char) from ztd_arr.h.
 *
 *   ZtdArena    Together with ztd_arena.h (included before or after this
 *               header, in any order) you also get ztd_str_arena_dup /
 *               ztd_str_arena_fmt, which put strings in a ZtdArena, and
 *               ZTD_STRBUF_INIT_ARENA(&a), a builder that grows inside the
 *               arena. The implementation file must include ztd_arena.h too.
 *
 * SHORT NAMES
 *   By default the header also defines unprefixed aliases:
 *       Str, StrBuf, StrSplit, STR(), STR_INIT(), STR_NPOS, STR_FMT, STR_ARG,
 *       STRBUF_INIT, STRBUF_INIT_ARENA(),
 *       str_* for every ztd_str_* function, sb_* for every ztd_sb_* function.
 *   Functions are aliased with object-like macros, so they still work as
 *   function pointers. To get only the ztd_ names, define before including:
 *       ZTD_NO_SHORT_NAMES        for every ztd_* header, or
 *       ZTD_STR_NO_SHORT_NAMES    for this header only.
 *   Configuration macros (ZTD_STR_IMPLEMENTATION etc.) are always prefixed.
 *
 * QUICK TOUR (short names)
 *   Str line = STR("  key = value  ");
 *   Str key, val;
 *   if (str_cut(str_trim(line), STR("="), &key, &val)) {
 *       key = str_trim(key);  val = str_trim(val);
 *       printf("[" STR_FMT "] -> [" STR_FMT "]\n", STR_ARG(key), STR_ARG(val));
 *   }
 *
 *   StrSplit it = str_split(STR("a,b,,c"), STR(","));
 *   Str tok;
 *   while (str_split_next(&it, &tok)) ...               // "a" "b" "" "c"
 *   it = str_split_any(STR("a,b;c"), STR(",;"));        // "a" "b" "c"
 *
 *   Str rest = STR("  ls  -l   /tmp ");
 *   while (str_next_field(&rest, &tok)) ...             // "ls" "-l" "/tmp"
 *
 *   StrBuf b = STRBUF_INIT;
 *   sb_appendf(&b, "%d items", 3);
 *   sb_append(&b, STR(" done"));
 *   sb_insert(&b, 0, STR(">> "));
 *   sb_append_repeat(&b, STR("-"), 3);
 *   puts(sb_cstr(&b));
 *   sb_free(&b);
 *
 *   static const Str keywords[] = { STR_INIT("if"), STR_INIT("else") };
 *
 * CONVENTIONS
 *   - "Not found" positions are ZTD_STR_NPOS.
 *   - Slicing clamps out-of-range indices instead of failing.
 *   - A ZtdStr with a NULL ptr and len 0 behaves exactly like "".
 *   - Character classes and case-folding are ASCII-only and locale-independent.
 *   - Number parsers leave *out untouched when they return false.
 *   - ZTD_STR_ARG passes the length as an int: views longer than INT_MAX
 *     print truncated. It also evaluates its argument twice.
 *   - ZtdStrBuf errors are sticky: after an allocation failure every further
 *     append returns false and leaves the buffer as it was, so you can do a
 *     run of appends and check b.failed once. An append that fails part way
 *     (join, replace) rolls back what it had appended. ztd_sb_clear() and
 *     ztd_sb_free() reset the flag.
 *   - ztd_sb_append, _insert, _append_repeat, _append_lower and _append_upper
 *     accept a ZtdStr that points into the same buffer. The other ztd_sb_*
 *     functions taking ZtdStr arguments do not; copy first.
 *   - Do not pass a buffer's own contents as a printf argument to
 *     ztd_sb_appendf.
 *   - Arena-backed buffers must not be used after the arena is rewound past
 *     their latest growth, or reset.
 *
 * CONFIGURATION (define before including the implementation)
 *   ZTD_STR_MALLOC(sz) / ZTD_STR_REALLOC(p, sz) / ZTD_STR_FREE(p)
 *       backing allocator; define all three or none. Defaults to
 *       ZTD_MALLOC / ZTD_REALLOC / ZTD_FREE if those are defined, else the
 *       C library.
 *   ZTD_STR_ASSERT(x)
 *       default ZTD_ASSERT(x) if defined, else assert(x)
 *
 * Requires C11. LICENSE: public domain / MIT-0. Do what you like.
 */
#ifndef ZTD_STR_H
#define ZTD_STR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ZTD_STR_STATIC
#if defined(__GNUC__)
#define ZTD_STR_DEF static __attribute__((unused))
#else
#define ZTD_STR_DEF static
#endif
#else
#define ZTD_STR_DEF extern
#endif

#if defined(__GNUC__)
#define ZTD_STR__PRINTF(fmt_idx, arg_idx) \
  __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define ZTD_STR__PRINTF(fmt_idx, arg_idx)
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct ZtdArena; /* defined by ztd_arena.h; only ever used by pointer here */

/* ---- Views -------------------------------------------------------------- */

typedef struct ZtdStr {
  const char* ptr;
  size_t len;
} ZtdStr;

#define ZTD_STR_NPOS ((size_t)-1)

/* View of a string literal (length computed at compile time). Literals only.
 * ZTD_STR is an expression; ZTD_STR_INIT is a brace initializer for static
 * tables and other constant initializers. */
#ifdef __cplusplus
#define ZTD_STR(lit) (ZtdStr{"" lit "", sizeof(lit) - 1})
#else
#define ZTD_STR(lit) ((ZtdStr){"" lit "", sizeof(lit) - 1})
#endif
#define ZTD_STR_INIT(lit) {"" lit "", sizeof(lit) - 1}

/* printf support:  printf("name: " ZTD_STR_FMT "\n", ZTD_STR_ARG(s)); */
#define ZTD_STR_FMT "%.*s"
#define ZTD_STR_ARG(s) (int)(s).len, (s).ptr

ZTD_STR_DEF ZtdStr ztd_str_from_cstr(const char* s); /* NULL -> "" */
ZTD_STR_DEF ZtdStr ztd_str_from_parts(const char* ptr, size_t len);

ZTD_STR_DEF bool ztd_str_eq(ZtdStr a, ZtdStr b);
ZTD_STR_DEF bool ztd_str_eq_cstr(ZtdStr a, const char* b);
ZTD_STR_DEF bool ztd_str_eq_nocase(ZtdStr a, ZtdStr b); /* ASCII only     */
ZTD_STR_DEF bool ztd_str_starts_with_nocase(ZtdStr s, ZtdStr prefix);
ZTD_STR_DEF bool ztd_str_ends_with_nocase(ZtdStr s, ZtdStr suffix);
ZTD_STR_DEF int ztd_str_cmp(ZtdStr a, ZtdStr b); /* -1, 0, 1 bytes */
/* qsort-style comparator for arrays of ZtdStr (byte order, as ztd_str_cmp). */
ZTD_STR_DEF int ztd_str_cmp_qsort(const void* a, const void* b);

ZTD_STR_DEF bool ztd_str_starts_with(ZtdStr s, ZtdStr prefix);
ZTD_STR_DEF bool ztd_str_ends_with(ZtdStr s, ZtdStr suffix);
ZTD_STR_DEF bool ztd_str_contains(ZtdStr s, ZtdStr needle);

/* First / last index, or ZTD_STR_NPOS. */
ZTD_STR_DEF size_t ztd_str_find(ZtdStr s, ZtdStr needle);
ZTD_STR_DEF size_t ztd_str_rfind(ZtdStr s, ZtdStr needle);
ZTD_STR_DEF size_t ztd_str_find_char(ZtdStr s, char c);
ZTD_STR_DEF size_t ztd_str_rfind_char(ZtdStr s, char c);
/* First / last index of any byte in `chars`, or ZTD_STR_NPOS. */
ZTD_STR_DEF size_t ztd_str_find_any(ZtdStr s, ZtdStr chars);
ZTD_STR_DEF size_t ztd_str_rfind_any(ZtdStr s, ZtdStr chars);

/* s[start..end), clamped to the string. */
ZTD_STR_DEF ZtdStr ztd_str_slice(ZtdStr s, size_t start, size_t end);

/* ASCII whitespace. */
ZTD_STR_DEF ZtdStr ztd_str_trim(ZtdStr s);
ZTD_STR_DEF ZtdStr ztd_str_trim_left(ZtdStr s);
ZTD_STR_DEF ZtdStr ztd_str_trim_right(ZtdStr s);

/* If *s starts/ends with the affix, remove it and return true. */
ZTD_STR_DEF bool ztd_str_chop_prefix(ZtdStr* s, ZtdStr prefix);
ZTD_STR_DEF bool ztd_str_chop_suffix(ZtdStr* s, ZtdStr suffix);

/* Split around the first `sep`. Returns false (before = s, after = "") if
 * sep isn't found. before/after may be NULL. */
ZTD_STR_DEF bool ztd_str_cut(ZtdStr s,
                             ZtdStr sep,
                             ZtdStr* before,
                             ZtdStr* after);
/* Same, around the last `sep`. Returns false (before = s, after = "") if sep
 * isn't found. */
ZTD_STR_DEF bool ztd_str_cut_last(ZtdStr s,
                                  ZtdStr sep,
                                  ZtdStr* before,
                                  ZtdStr* after);

/* Tokenizer. Yields every field, including empty ones:
 *   "a,,b" -> "a" "" "b";  "" -> "";  "a," -> "a" "".
 * An empty sep yields s as a single field. ztd_str_split_any splits on every
 * single byte that is in `chars`, with the same rules. */
typedef struct ZtdStrSplit {
  ZtdStr rest;
  ZtdStr sep;
  bool done;
  bool any; /* sep is a set of bytes */
} ZtdStrSplit;

ZTD_STR_DEF ZtdStrSplit ztd_str_split(ZtdStr s, ZtdStr sep);
ZTD_STR_DEF ZtdStrSplit ztd_str_split_any(ZtdStr s, ZtdStr chars);
ZTD_STR_DEF bool ztd_str_split_next(ZtdStrSplit* it, ZtdStr* token);

/* Line iterator: splits on '\n', strips a trailing '\r', and does not yield
 * an empty line after a final newline ("" yields nothing). */
ZTD_STR_DEF bool ztd_str_next_line(ZtdStr* rest, ZtdStr* line);

/* Field iterator: yields the runs of non-whitespace in *rest, skipping any
 * amount of ASCII whitespace between them (so never an empty field). */
ZTD_STR_DEF bool ztd_str_next_field(ZtdStr* rest, ZtdStr* field);

/* Whole-string parsers: no whitespace, no trailing junk, overflow fails.
 * base 0 = auto: "0x"/"0b"/"0o" prefixes, otherwise decimal (a leading 0 does
 * NOT mean octal). Base 16 / 2 / 8 also accept their own prefix.
 * Both accept a leading '+'; signed parsing also accepts '-'. */
ZTD_STR_DEF bool ztd_str_to_u64(ZtdStr s, int base, uint64_t* out);
ZTD_STR_DEF bool ztd_str_to_i64(ZtdStr s, int base, int64_t* out);
/* Uses strtod, so it follows the C locale's decimal point. Rejects overflow
 * to infinity; accepts "inf"/"nan" spellings strtod accepts. */
ZTD_STR_DEF bool ztd_str_to_double(ZtdStr s, double* out);

ZTD_STR_DEF uint64_t ztd_str_hash(ZtdStr s); /* FNV-1a, 64-bit */

/* UTF-8: decode one code point and advance. Returns 1 for a valid code point,
 * 0 at the end of s, and -1 for an invalid or truncated sequence (overlongs,
 * surrogates, > U+10FFFF), which yields U+FFFD. An invalid sequence consumes
 * its longest valid-looking prefix (Unicode's "maximal subpart" practice), so
 * a truncated character gives one U+FFFD, not one per byte. So
 * `while (ztd_str_utf8_next(&s, &cp))` decodes leniently and `> 0` strictly. */
ZTD_STR_DEF int ztd_str_utf8_next(ZtdStr* s, uint32_t* codepoint);
ZTD_STR_DEF bool ztd_str_utf8_valid(ZtdStr s);

/* ---- Builder ------------------------------------------------------------ */

typedef struct ZtdStrBuf {
  char* ptr; /* NULL until first growth; NUL-terminated once non-NULL */
  size_t len;
  size_t cap;
  struct ZtdArena* arena; /* grow inside this arena instead of the heap */
  bool failed;            /* sticky allocation-failure flag */
} ZtdStrBuf;

#ifdef __cplusplus
#define ZTD_STRBUF_INIT \
  {                     \
  }
#else
#define ZTD_STRBUF_INIT {0}
#endif

ZTD_STR_DEF bool ztd_sb_reserve(ZtdStrBuf* b,
                                size_t extra); /* room for `extra` more */
ZTD_STR_DEF bool ztd_sb_append(ZtdStrBuf* b, ZtdStr s); /* s may alias b */
ZTD_STR_DEF bool ztd_sb_append_cstr(ZtdStrBuf* b, const char* s);
ZTD_STR_DEF bool ztd_sb_append_char(ZtdStrBuf* b, char c);
/* Append the UTF-8 encoding of a code point (invalid ones become U+FFFD). */
ZTD_STR_DEF bool ztd_sb_append_utf8(ZtdStrBuf* b, uint32_t codepoint);
ZTD_STR_DEF bool ztd_sb_appendf(ZtdStrBuf* b, const char* fmt, ...)
    ZTD_STR__PRINTF(2, 3);
ZTD_STR_DEF bool ztd_sb_vappendf(ZtdStrBuf* b, const char* fmt, va_list ap);
/* Insert s at byte position pos (clamped to the length). */
ZTD_STR_DEF bool ztd_sb_insert(ZtdStrBuf* b, size_t pos, ZtdStr s);
/* Append s n times. */
ZTD_STR_DEF bool ztd_sb_append_repeat(ZtdStrBuf* b, ZtdStr s, size_t n);
/* Append s with ASCII letters lower- / upper-cased. */
ZTD_STR_DEF bool ztd_sb_append_lower(ZtdStrBuf* b, ZtdStr s);
ZTD_STR_DEF bool ztd_sb_append_upper(ZtdStrBuf* b, ZtdStr s);
ZTD_STR_DEF bool ztd_sb_append_join(ZtdStrBuf* b,
                                    const ZtdStr* parts,
                                    size_t count,
                                    ZtdStr sep);
/* Append s with every occurrence of `from` replaced by `to`. */
ZTD_STR_DEF bool ztd_sb_append_replace(ZtdStrBuf* b,
                                       ZtdStr s,
                                       ZtdStr from,
                                       ZtdStr to);

ZTD_STR_DEF void ztd_sb_truncate(ZtdStrBuf* b,
                                 size_t len); /* no-op if len >= b->len  */
ZTD_STR_DEF void ztd_sb_clear(ZtdStrBuf* b);  /* keeps cap, resets failed */
/* Release the storage (unless arena-backed) and empty the buffer. The buffer
 * keeps its arena, so it can be reused. */
ZTD_STR_DEF void ztd_sb_free(ZtdStrBuf* b);

ZTD_STR_DEF ZtdStr ztd_sb_str(const ZtdStrBuf* b); /* view of the contents */
ZTD_STR_DEF const char* ztd_sb_cstr(const ZtdStrBuf* b); /* never NULL */
/* Take ownership of the NUL-terminated string and reset the buffer. Free a
 * heap-backed result with ZTD_STR_FREE / free; an arena-backed result lives
 * in the arena. Returns NULL if the buffer had failed or allocation fails. */
ZTD_STR_DEF char* ztd_sb_detach(ZtdStrBuf* b);

#ifdef __cplusplus
}
#endif

/* ---- Short names (see SHORT NAMES above) -------------------------------- */

#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_STR_NO_SHORT_NAMES)
typedef ZtdStr Str;
typedef ZtdStrBuf StrBuf;
typedef ZtdStrSplit StrSplit;

#define STR(lit) ZTD_STR(lit)
#define STR_INIT(lit) ZTD_STR_INIT(lit)
#define STR_NPOS ZTD_STR_NPOS
#define STR_FMT ZTD_STR_FMT
#define STR_ARG(s) ZTD_STR_ARG(s)
#define STRBUF_INIT ZTD_STRBUF_INIT

#define str_from_cstr ztd_str_from_cstr
#define str_from_parts ztd_str_from_parts
#define str_eq ztd_str_eq
#define str_eq_cstr ztd_str_eq_cstr
#define str_eq_nocase ztd_str_eq_nocase
#define str_starts_with_nocase ztd_str_starts_with_nocase
#define str_ends_with_nocase ztd_str_ends_with_nocase
#define str_cmp ztd_str_cmp
#define str_cmp_qsort ztd_str_cmp_qsort
#define str_starts_with ztd_str_starts_with
#define str_ends_with ztd_str_ends_with
#define str_contains ztd_str_contains
#define str_find ztd_str_find
#define str_rfind ztd_str_rfind
#define str_find_char ztd_str_find_char
#define str_rfind_char ztd_str_rfind_char
#define str_find_any ztd_str_find_any
#define str_rfind_any ztd_str_rfind_any
#define str_cut_last ztd_str_cut_last
#define str_split_any ztd_str_split_any
#define str_next_field ztd_str_next_field
#define str_slice ztd_str_slice
#define str_trim ztd_str_trim
#define str_trim_left ztd_str_trim_left
#define str_trim_right ztd_str_trim_right
#define str_chop_prefix ztd_str_chop_prefix
#define str_chop_suffix ztd_str_chop_suffix
#define str_cut ztd_str_cut
#define str_split ztd_str_split
#define str_split_next ztd_str_split_next
#define str_next_line ztd_str_next_line
#define str_to_u64 ztd_str_to_u64
#define str_to_i64 ztd_str_to_i64
#define str_to_double ztd_str_to_double
#define str_hash ztd_str_hash
#define str_utf8_next ztd_str_utf8_next
#define str_utf8_valid ztd_str_utf8_valid

#define sb_reserve ztd_sb_reserve
#define sb_append ztd_sb_append
#define sb_append_cstr ztd_sb_append_cstr
#define sb_append_char ztd_sb_append_char
#define sb_append_utf8 ztd_sb_append_utf8
#define sb_appendf ztd_sb_appendf
#define sb_vappendf ztd_sb_vappendf
#define sb_insert ztd_sb_insert
#define sb_append_repeat ztd_sb_append_repeat
#define sb_append_lower ztd_sb_append_lower
#define sb_append_upper ztd_sb_append_upper
#define sb_append_join ztd_sb_append_join
#define sb_append_replace ztd_sb_append_replace
#define sb_truncate ztd_sb_truncate
#define sb_clear ztd_sb_clear
#define sb_free ztd_sb_free
#define sb_str ztd_sb_str
#define sb_cstr ztd_sb_cstr
#define sb_detach ztd_sb_detach
#endif

#endif /* ZTD_STR_H */

/* ---- Arena integration (declarations) -----------------------------------
 * Re-evaluated on every include, so it appears as soon as both headers have
 * been seen, whatever their order. */

#if defined(ZTD_ARENA_H) && !defined(ZTD_STR__ARENA_DECL)
#define ZTD_STR__ARENA_DECL

#ifdef __cplusplus
extern "C" {
#endif
/* NUL-terminated copy in the arena; .ptr is NULL on failure. */
ZTD_STR_DEF ZtdStr ztd_str_arena_dup(ZtdArena* a, ZtdStr s);
ZTD_STR_DEF ZtdStr ztd_str_arena_fmt(ZtdArena* a, const char* fmt, ...)
    ZTD_STR__PRINTF(2, 3);
ZTD_STR_DEF ZtdStr ztd_str_arena_vfmt(ZtdArena* a, const char* fmt, va_list ap);
/* Returns a; defined only by an implementation that saw ztd_arena.h, so using
 * ZTD_STRBUF_INIT_ARENA without arena support fails to link. */
ZTD_STR_DEF ZtdArena* ztd_sb__arena_linked(ZtdArena* a);
#ifdef __cplusplus
}
#endif

/* A builder that grows inside arena *a (an automatic-storage initializer). */
#ifdef __cplusplus
#define ZTD_STRBUF_INIT_ARENA(a) {nullptr, 0, 0, ztd_sb__arena_linked(a), false}
#else
#define ZTD_STRBUF_INIT_ARENA(a) {.arena = ztd_sb__arena_linked(a)}
#endif

#if !defined(ZTD_NO_SHORT_NAMES) && !defined(ZTD_STR_NO_SHORT_NAMES)
#define STRBUF_INIT_ARENA(a) ZTD_STRBUF_INIT_ARENA(a)
#define str_arena_dup ztd_str_arena_dup
#define str_arena_fmt ztd_str_arena_fmt
#define str_arena_vfmt ztd_str_arena_vfmt
#endif
#endif /* arena declarations */

/* ======================================================================== */

#ifdef ZTD_STR_IMPLEMENTATION
#ifndef ZTD_STR_IMPLEMENTATION_DONE
#define ZTD_STR_IMPLEMENTATION_DONE

#ifdef __cplusplus
#error "ztd_str.h: compile the implementation as C (C11)"
#endif

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(ZTD_STR_MALLOC) != defined(ZTD_STR_FREE) || \
    defined(ZTD_STR_MALLOC) != defined(ZTD_STR_REALLOC)
#error \
    "ztd_str.h: define all of ZTD_STR_MALLOC, ZTD_STR_REALLOC and ZTD_STR_FREE, or none"
#endif
#ifndef ZTD_STR_MALLOC
#if defined(ZTD_MALLOC) && defined(ZTD_REALLOC) && defined(ZTD_FREE)
#define ZTD_STR_MALLOC(sz) ZTD_MALLOC(sz)
#define ZTD_STR_REALLOC(p, sz) ZTD_REALLOC(p, sz)
#define ZTD_STR_FREE(p) ZTD_FREE(p)
#else
#include <stdlib.h>
#define ZTD_STR_MALLOC(sz) malloc(sz)
#define ZTD_STR_REALLOC(p, sz) realloc((p), (sz))
#define ZTD_STR_FREE(p) free(p)
#endif
#endif

#ifndef ZTD_STR_ASSERT
#ifdef ZTD_ASSERT
#define ZTD_STR_ASSERT(x) ZTD_ASSERT(x)
#else
#include <assert.h>
#define ZTD_STR_ASSERT(x) assert(x)
#endif
#endif

/* Arena growth hook. A tentative definition: it stays NULL unless the arena
 * integration below is compiled into this file, which initialises it. */
static void* (*ztd_str__arena_realloc)(struct ZtdArena* a,
                                       void* ptr,
                                       size_t old_size,
                                       size_t new_size,
                                       size_t align);

static int ztd_str__lower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ztd_str__upper(int c) {
  return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

static bool ztd_str__eq_nocase_n(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (ztd_str__lower((unsigned char)a[i]) !=
        ztd_str__lower((unsigned char)b[i]))
      return false;
  }
  return true;
}

/* Byte set for the *_any functions. */
typedef struct {
  unsigned char bits[32];
} ZtdStr__Set;

static ZtdStr__Set ztd_str__set(ZtdStr chars) {
  ZtdStr__Set set;
  memset(&set, 0, sizeof set);
  for (size_t i = 0; i < chars.len; i++) {
    unsigned char c = (unsigned char)chars.ptr[i];
    set.bits[c >> 3] |= (unsigned char)(1u << (c & 7));
  }
  return set;
}

static bool ztd_str__in_set(const ZtdStr__Set* set, char ch) {
  unsigned char c = (unsigned char)ch;
  return (set->bits[c >> 3] >> (c & 7)) & 1;
}

static bool ztd_str__is_space(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

static ZtdStr ztd_str__make(const char* p, size_t n) {
  ZtdStr r;
  r.ptr = p;
  r.len = n;
  return r;
}

/* ---- Views -------------------------------------------------------------- */

ZTD_STR_DEF ZtdStr ztd_str_from_cstr(const char* s) {
  return s ? ztd_str__make(s, strlen(s)) : ztd_str__make("", 0);
}

ZTD_STR_DEF ZtdStr ztd_str_from_parts(const char* ptr, size_t len) {
  ZTD_STR_ASSERT((ptr || len == 0) &&
                 "ztd_str_from_parts: NULL pointer with nonzero length");
  return ztd_str__make(ptr, len);
}

ZTD_STR_DEF bool ztd_str_eq(ZtdStr a, ZtdStr b) {
  return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

ZTD_STR_DEF bool ztd_str_eq_cstr(ZtdStr a, const char* b) {
  return ztd_str_eq(a, ztd_str_from_cstr(b));
}

ZTD_STR_DEF bool ztd_str_eq_nocase(ZtdStr a, ZtdStr b) {
  return a.len == b.len && ztd_str__eq_nocase_n(a.ptr, b.ptr, a.len);
}

ZTD_STR_DEF bool ztd_str_starts_with_nocase(ZtdStr s, ZtdStr p) {
  return p.len <= s.len && ztd_str__eq_nocase_n(s.ptr, p.ptr, p.len);
}

ZTD_STR_DEF bool ztd_str_ends_with_nocase(ZtdStr s, ZtdStr p) {
  return p.len <= s.len &&
         ztd_str__eq_nocase_n(s.ptr + (s.len - p.len), p.ptr, p.len);
}

ZTD_STR_DEF int ztd_str_cmp(ZtdStr a, ZtdStr b) {
  size_t n = a.len < b.len ? a.len : b.len;
  int c = n ? memcmp(a.ptr, b.ptr, n) : 0;
  if (c)
    return c < 0 ? -1 : 1;
  return (a.len > b.len) - (a.len < b.len);
}

ZTD_STR_DEF int ztd_str_cmp_qsort(const void* a, const void* b) {
  return ztd_str_cmp(*(const ZtdStr*)a, *(const ZtdStr*)b);
}

ZTD_STR_DEF bool ztd_str_starts_with(ZtdStr s, ZtdStr p) {
  return p.len <= s.len && (p.len == 0 || memcmp(s.ptr, p.ptr, p.len) == 0);
}

ZTD_STR_DEF bool ztd_str_ends_with(ZtdStr s, ZtdStr p) {
  return p.len <= s.len &&
         (p.len == 0 || memcmp(s.ptr + (s.len - p.len), p.ptr, p.len) == 0);
}

ZTD_STR_DEF size_t ztd_str_find(ZtdStr h, ZtdStr n) {
  if (n.len == 0)
    return 0;
  if (n.len > h.len)
    return ZTD_STR_NPOS;
  const char* p = h.ptr;
  const char* last = h.ptr + (h.len - n.len); /* last possible start */
  while (p <= last) {
    const char* q = (const char*)memchr(p, n.ptr[0], (size_t)(last - p) + 1);
    if (!q)
      return ZTD_STR_NPOS;
    if (memcmp(q, n.ptr, n.len) == 0)
      return (size_t)(q - h.ptr);
    p = q + 1;
  }
  return ZTD_STR_NPOS;
}

ZTD_STR_DEF size_t ztd_str_rfind(ZtdStr h, ZtdStr n) {
  if (n.len == 0)
    return h.len;
  if (n.len > h.len)
    return ZTD_STR_NPOS;
  size_t i = h.len - n.len + 1;
  while (i-- > 0) {
    if (h.ptr[i] == n.ptr[0] && memcmp(h.ptr + i, n.ptr, n.len) == 0)
      return i;
  }
  return ZTD_STR_NPOS;
}

ZTD_STR_DEF size_t ztd_str_find_char(ZtdStr s, char c) {
  if (s.len == 0)
    return ZTD_STR_NPOS;
  const char* q = (const char*)memchr(s.ptr, c, s.len);
  return q ? (size_t)(q - s.ptr) : ZTD_STR_NPOS;
}

ZTD_STR_DEF size_t ztd_str_rfind_char(ZtdStr s, char c) {
  size_t i = s.len;
  while (i-- > 0) {
    if (s.ptr[i] == c)
      return i;
  }
  return ZTD_STR_NPOS;
}

ZTD_STR_DEF size_t ztd_str_find_any(ZtdStr s, ZtdStr chars) {
  ZtdStr__Set set = ztd_str__set(chars);
  for (size_t i = 0; i < s.len; i++) {
    if (ztd_str__in_set(&set, s.ptr[i]))
      return i;
  }
  return ZTD_STR_NPOS;
}

ZTD_STR_DEF size_t ztd_str_rfind_any(ZtdStr s, ZtdStr chars) {
  ZtdStr__Set set = ztd_str__set(chars);
  size_t i = s.len;
  while (i-- > 0) {
    if (ztd_str__in_set(&set, s.ptr[i]))
      return i;
  }
  return ZTD_STR_NPOS;
}

ZTD_STR_DEF bool ztd_str_contains(ZtdStr s, ZtdStr needle) {
  return ztd_str_find(s, needle) != ZTD_STR_NPOS;
}

ZTD_STR_DEF ZtdStr ztd_str_slice(ZtdStr s, size_t start, size_t end) {
  if (!s.ptr)
    return s;
  if (end > s.len)
    end = s.len;
  if (start > end)
    start = end;
  return ztd_str__make(s.ptr + start, end - start);
}

ZTD_STR_DEF ZtdStr ztd_str_trim_left(ZtdStr s) {
  while (s.len && ztd_str__is_space((unsigned char)s.ptr[0])) {
    s.ptr++;
    s.len--;
  }
  return s;
}

ZTD_STR_DEF ZtdStr ztd_str_trim_right(ZtdStr s) {
  while (s.len && ztd_str__is_space((unsigned char)s.ptr[s.len - 1]))
    s.len--;
  return s;
}

ZTD_STR_DEF ZtdStr ztd_str_trim(ZtdStr s) {
  return ztd_str_trim_right(ztd_str_trim_left(s));
}

ZTD_STR_DEF bool ztd_str_chop_prefix(ZtdStr* s, ZtdStr p) {
  if (!ztd_str_starts_with(*s, p))
    return false;
  *s = ztd_str_slice(*s, p.len, s->len);
  return true;
}

ZTD_STR_DEF bool ztd_str_chop_suffix(ZtdStr* s, ZtdStr p) {
  if (!ztd_str_ends_with(*s, p))
    return false;
  s->len -= p.len;
  return true;
}

ZTD_STR_DEF bool ztd_str_cut(ZtdStr s,
                             ZtdStr sep,
                             ZtdStr* before,
                             ZtdStr* after) {
  size_t i = ztd_str_find(s, sep);
  if (i == ZTD_STR_NPOS) {
    if (before)
      *before = s;
    if (after)
      *after = ztd_str_slice(s, s.len, s.len);
    return false;
  }
  if (before)
    *before = ztd_str_slice(s, 0, i);
  if (after)
    *after = ztd_str_slice(s, i + sep.len, s.len);
  return true;
}

ZTD_STR_DEF bool ztd_str_cut_last(ZtdStr s,
                                  ZtdStr sep,
                                  ZtdStr* before,
                                  ZtdStr* after) {
  size_t i = ztd_str_rfind(s, sep);
  if (i == ZTD_STR_NPOS) {
    if (before)
      *before = s;
    if (after)
      *after = ztd_str_slice(s, s.len, s.len);
    return false;
  }
  if (before)
    *before = ztd_str_slice(s, 0, i);
  if (after)
    *after = ztd_str_slice(s, i + sep.len, s.len);
  return true;
}

ZTD_STR_DEF ZtdStrSplit ztd_str_split(ZtdStr s, ZtdStr sep) {
  ZtdStrSplit it;
  it.rest = s;
  it.sep = sep;
  it.done = false;
  it.any = false;
  return it;
}

ZTD_STR_DEF ZtdStrSplit ztd_str_split_any(ZtdStr s, ZtdStr chars) {
  ZtdStrSplit it = ztd_str_split(s, chars);
  it.any = true;
  return it;
}

ZTD_STR_DEF bool ztd_str_split_next(ZtdStrSplit* it, ZtdStr* token) {
  if (it->done)
    return false;
  size_t i = ZTD_STR_NPOS, sep_len = 0;
  if (it->sep.len) {
    i = it->any ? ztd_str_find_any(it->rest, it->sep)
                : ztd_str_find(it->rest, it->sep);
    sep_len = it->any ? 1 : it->sep.len;
  }
  if (i == ZTD_STR_NPOS) {
    *token = it->rest;
    it->rest = ztd_str_slice(it->rest, it->rest.len, it->rest.len);
    it->done = true;
    return true;
  }
  *token = ztd_str_slice(it->rest, 0, i);
  it->rest = ztd_str_slice(it->rest, i + sep_len, it->rest.len);
  return true;
}

ZTD_STR_DEF bool ztd_str_next_line(ZtdStr* rest, ZtdStr* line) {
  if (rest->len == 0)
    return false;
  size_t i = ztd_str_find_char(*rest, '\n');
  ZtdStr l;
  if (i == ZTD_STR_NPOS) {
    l = *rest;
    *rest = ztd_str_slice(*rest, rest->len, rest->len);
  } else {
    l = ztd_str_slice(*rest, 0, i);
    *rest = ztd_str_slice(*rest, i + 1, rest->len);
  }
  if (l.len && l.ptr[l.len - 1] == '\r')
    l.len--;
  *line = l;
  return true;
}

ZTD_STR_DEF bool ztd_str_next_field(ZtdStr* rest, ZtdStr* field) {
  ZtdStr s = ztd_str_trim_left(*rest);
  if (s.len == 0) {
    *rest = s;
    return false;
  }
  size_t n = 0;
  while (n < s.len && !ztd_str__is_space((unsigned char)s.ptr[n]))
    n++;
  *field = ztd_str_slice(s, 0, n);
  *rest = ztd_str_slice(s, n, s.len);
  return true;
}

static int ztd_str__digit(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  c = ztd_str__lower(c);
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 10;
  return 99;
}

/* Unsigned magnitude: no sign accepted. */
static bool ztd_str__to_u64(ZtdStr s, int base, uint64_t* out) {
  if (s.len > 2 && s.ptr[0] == '0') {
    int c = ztd_str__lower((unsigned char)s.ptr[1]);
    int prefix_base = c == 'x' ? 16 : c == 'b' ? 2 : c == 'o' ? 8 : 0;
    if (prefix_base && (base == 0 || base == prefix_base)) {
      base = prefix_base;
      s = ztd_str_slice(s, 2, s.len);
    }
  }
  if (base == 0)
    base = 10;
  if (base < 2 || base > 36 || s.len == 0)
    return false;

  uint64_t v = 0;
  for (size_t i = 0; i < s.len; i++) {
    int d = ztd_str__digit((unsigned char)s.ptr[i]);
    if (d >= base)
      return false;
    if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
      return false;
    v = v * (uint64_t)base + (uint64_t)d;
  }
  *out = v;
  return true;
}

ZTD_STR_DEF bool ztd_str_to_u64(ZtdStr s, int base, uint64_t* out) {
  if (s.len && s.ptr[0] == '+')
    s = ztd_str_slice(s, 1, s.len);
  return ztd_str__to_u64(s, base, out);
}

ZTD_STR_DEF bool ztd_str_to_i64(ZtdStr s, int base, int64_t* out) {
  bool neg = false;
  if (s.len && (s.ptr[0] == '-' || s.ptr[0] == '+')) {
    neg = s.ptr[0] == '-';
    s = ztd_str_slice(s, 1, s.len);
  }
  uint64_t mag;
  if (!ztd_str__to_u64(s, base, &mag))
    return false;
  const uint64_t lim = (uint64_t)INT64_MAX;
  if (neg) {
    if (mag > lim + 1)
      return false;
    *out = mag == lim + 1 ? INT64_MIN : -(int64_t)mag;
  } else {
    if (mag > lim)
      return false;
    *out = (int64_t)mag;
  }
  return true;
}

ZTD_STR_DEF bool ztd_str_to_double(ZtdStr s, double* out) {
  if (s.len == 0 || ztd_str__is_space((unsigned char)s.ptr[0]))
    return false;
  if (s.len == SIZE_MAX)
    return false;

  char stack_buf[128];
  char* buf = stack_buf;
  if (s.len >= sizeof stack_buf) {
    buf = (char*)ZTD_STR_MALLOC(s.len + 1);
    if (!buf)
      return false;
  }
  memcpy(buf, s.ptr, s.len);
  buf[s.len] = '\0';

  char* end;
  int saved_errno = errno;
  errno = 0;
  double v = strtod(buf, &end);
  bool ok = (size_t)(end - buf) == s.len &&
            !(errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL));
  errno = saved_errno;

  if (buf != stack_buf)
    ZTD_STR_FREE(buf);
  if (ok)
    *out = v;
  return ok;
}

ZTD_STR_DEF uint64_t ztd_str_hash(ZtdStr s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < s.len; i++) {
    h ^= (unsigned char)s.ptr[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

/* Decode the sequence at p (len >= 1 bytes) into *cp and return the bytes
 * consumed. An invalid sequence sets *bad, yields U+FFFD and consumes its
 * maximal subpart: the lead byte plus every following byte that could still
 * have been part of a valid sequence (Unicode 3.9, Table 3-7). */
static size_t ztd_str__utf8_decode(const unsigned char* p,
                                   size_t len,
                                   uint32_t* cp,
                                   bool* bad) {
  uint32_t c = p[0];
  *bad = false;
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  size_t n;
  unsigned char lo = 0x80, hi = 0xBF; /* allowed range of the next byte */
  if (c >= 0xC2 && c <= 0xDF) {
    n = 2;
    c &= 0x1F;
  } else if (c >= 0xE0 && c <= 0xEF) {
    n = 3;
    c &= 0x0F;
    if (c == 0x0)
      lo = 0xA0; /* no overlongs */
    else if (c == 0xD)
      hi = 0x9F; /* no surrogates */
  } else if (c >= 0xF0 && c <= 0xF4) {
    n = 4;
    c &= 0x07;
    if (c == 0)
      lo = 0x90; /* no overlongs */
    else if (c == 4)
      hi = 0x8F; /* nothing above U+10FFFF */
  } else {
    *bad = true;
    *cp = 0xFFFD;
    return 1;
  }
  size_t i = 1;
  for (; i < n && i < len; i++) {
    if (p[i] < lo || p[i] > hi)
      break;
    c = (c << 6) | (p[i] & 0x3F);
    lo = 0x80;
    hi = 0xBF;
  }
  if (i < n) {
    *bad = true;
    *cp = 0xFFFD;
    return i;
  }
  *cp = c;
  return n;
}

ZTD_STR_DEF int ztd_str_utf8_next(ZtdStr* s, uint32_t* codepoint) {
  if (s->len == 0)
    return 0;
  bool bad;
  size_t n = ztd_str__utf8_decode((const unsigned char*)s->ptr, s->len,
                                  codepoint, &bad);
  s->ptr += n;
  s->len -= n;
  return bad ? -1 : 1;
}

ZTD_STR_DEF bool ztd_str_utf8_valid(ZtdStr s) {
  uint32_t cp;
  bool bad;
  while (s.len) {
    size_t n =
        ztd_str__utf8_decode((const unsigned char*)s.ptr, s.len, &cp, &bad);
    if (bad)
      return false;
    s.ptr += n;
    s.len -= n;
  }
  return true;
}

/* ---- Builder ------------------------------------------------------------ */

/* Offset of s inside b's storage, or SIZE_MAX if it points elsewhere. Needed
 * because growing b can move what s points at. */
static size_t ztd_sb__self_offset(const ZtdStrBuf* b, ZtdStr s) {
  if (!b->ptr || !s.ptr)
    return SIZE_MAX;
  uintptr_t lo = (uintptr_t)b->ptr, hi = lo + b->cap, sp = (uintptr_t)s.ptr;
  return sp >= lo && sp < hi ? (size_t)(sp - lo) : SIZE_MAX;
}

ZTD_STR_DEF bool ztd_sb_reserve(ZtdStrBuf* b, size_t extra) {
  if (b->failed)
    return false;
  if (extra > SIZE_MAX - 1 - b->len)
    goto fail;
  size_t need = b->len + extra + 1; /* +1 for the NUL */
  if (need <= b->cap)
    return true;

  size_t cap = b->cap ? b->cap : 32;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  char* p;
  if (b->arena) {
    ZTD_STR_ASSERT(ztd_str__arena_realloc &&
                   "ztd_str: arena-backed buffer, but the implementation did "
                   "not include ztd_arena.h");
    if (!ztd_str__arena_realloc)
      goto fail;
    p = (char*)ztd_str__arena_realloc(b->arena, b->ptr, b->cap, cap, 1);
  } else {
    p = (char*)(b->ptr ? ZTD_STR_REALLOC(b->ptr, cap) : ZTD_STR_MALLOC(cap));
  }
  if (!p)
    goto fail;
  p[b->len] = '\0';
  b->ptr = p;
  b->cap = cap;
  return true;

fail:
  b->failed = true;
  return false;
}

ZTD_STR_DEF bool ztd_sb_append(ZtdStrBuf* b, ZtdStr s) {
  if (b->failed)
    return false;
  if (s.len == 0)
    return true;

  /* s may point into our own buffer, which reserve can move. */
  size_t self_off = ztd_sb__self_offset(b, s);
  if (!ztd_sb_reserve(b, s.len))
    return false;
  const char* src = self_off != SIZE_MAX ? b->ptr + self_off : s.ptr;

  memmove(b->ptr + b->len, src, s.len);
  b->len += s.len;
  b->ptr[b->len] = '\0';
  return true;
}

ZTD_STR_DEF bool ztd_sb_append_cstr(ZtdStrBuf* b, const char* s) {
  return ztd_sb_append(b, ztd_str_from_cstr(s));
}

ZTD_STR_DEF bool ztd_sb_append_char(ZtdStrBuf* b, char c) {
  if (!ztd_sb_reserve(b, 1))
    return false;
  b->ptr[b->len++] = c;
  b->ptr[b->len] = '\0';
  return true;
}

ZTD_STR_DEF bool ztd_sb_append_utf8(ZtdStrBuf* b, uint32_t cp) {
  char u[4];
  size_t n;
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    cp = 0xFFFD;
  if (cp < 0x80) {
    u[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    u[0] = (char)(0xC0 | (cp >> 6));
    u[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    u[0] = (char)(0xE0 | (cp >> 12));
    u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    u[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    u[0] = (char)(0xF0 | (cp >> 18));
    u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    u[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  return ztd_sb_append(b, ztd_str__make(u, n));
}

ZTD_STR_DEF bool ztd_sb_vappendf(ZtdStrBuf* b, const char* fmt, va_list ap) {
  if (b->failed)
    return false;

  /* Format straight into the spare capacity; only if it doesn't fit, grow
   * and format a second time. */
  size_t avail = b->ptr ? b->cap - b->len : 0;
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(avail ? b->ptr + b->len : NULL, avail, fmt, ap2);
  va_end(ap2);
  if (n < 0) {
    if (b->ptr)
      b->ptr[b->len] = '\0';
    b->failed = true;
    return false;
  }
  if ((size_t)n < avail) {
    b->len += (size_t)n;
    return true;
  }
  if (!ztd_sb_reserve(b, (size_t)n)) {
    if (b->ptr)
      b->ptr[b->len] = '\0'; /* drop the truncated attempt */
    return false;
  }
  vsnprintf(b->ptr + b->len, (size_t)n + 1, fmt, ap);
  b->len += (size_t)n;
  return true;
}

ZTD_STR_DEF bool ztd_sb_appendf(ZtdStrBuf* b, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool ok = ztd_sb_vappendf(b, fmt, ap);
  va_end(ap);
  return ok;
}

ZTD_STR_DEF bool ztd_sb_insert(ZtdStrBuf* b, size_t pos, ZtdStr s) {
  if (b->failed)
    return false;
  if (s.len == 0)
    return true;
  if (pos > b->len)
    pos = b->len;
  size_t self_off = ztd_sb__self_offset(b, s);
  if (!ztd_sb_reserve(b, s.len))
    return false;

  char* p = b->ptr;
  size_t n = s.len;
  memmove(p + pos + n, p + pos, b->len - pos + 1); /* with the NUL */
  if (self_off == SIZE_MAX) {
    memcpy(p + pos, s.ptr, n);
  } else if (self_off + n <= pos) { /* source was before the gap: unmoved */
    memcpy(p + pos, p + self_off, n);
  } else if (self_off >= pos) { /* source was shifted up by n */
    memcpy(p + pos, p + self_off + n, n);
  } else { /* source straddled the gap */
    size_t first = pos - self_off;
    memcpy(p + pos, p + self_off, first);
    memcpy(p + pos + first, p + pos + n, n - first);
  }
  b->len += n;
  return true;
}

ZTD_STR_DEF bool ztd_sb_append_repeat(ZtdStrBuf* b, ZtdStr s, size_t n) {
  if (b->failed)
    return false;
  if (s.len == 0 || n == 0)
    return true;
  if (s.len > SIZE_MAX / n) {
    b->failed = true;
    return false;
  }
  size_t self_off = ztd_sb__self_offset(b, s);
  if (!ztd_sb_reserve(b, s.len * n))
    return false;
  const char* src = self_off != SIZE_MAX ? b->ptr + self_off : s.ptr;
  for (size_t i = 0; i < n; i++) {
    memcpy(b->ptr + b->len, src, s.len);
    b->len += s.len;
  }
  b->ptr[b->len] = '\0';
  return true;
}

ZTD_STR_DEF bool ztd_sb_append_lower(ZtdStrBuf* b, ZtdStr s) {
  size_t start = b->len;
  if (!ztd_sb_append(b, s))
    return false;
  for (size_t i = start; i < b->len; i++)
    b->ptr[i] = (char)ztd_str__lower((unsigned char)b->ptr[i]);
  return true;
}

ZTD_STR_DEF bool ztd_sb_append_upper(ZtdStrBuf* b, ZtdStr s) {
  size_t start = b->len;
  if (!ztd_sb_append(b, s))
    return false;
  for (size_t i = start; i < b->len; i++)
    b->ptr[i] = (char)ztd_str__upper((unsigned char)b->ptr[i]);
  return true;
}

ZTD_STR_DEF bool ztd_sb_append_join(ZtdStrBuf* b,
                                    const ZtdStr* parts,
                                    size_t count,
                                    ZtdStr sep) {
  size_t start = b->len;
  for (size_t i = 0; i < count; i++) {
    if (i)
      ztd_sb_append(b, sep);
    ztd_sb_append(b, parts[i]);
  }
  if (b->failed)
    ztd_sb_truncate(b, start);
  return !b->failed;
}

ZTD_STR_DEF bool ztd_sb_append_replace(ZtdStrBuf* b,
                                       ZtdStr s,
                                       ZtdStr from,
                                       ZtdStr to) {
  if (from.len == 0)
    return ztd_sb_append(b, s);
  size_t start = b->len;
  size_t i;
  while ((i = ztd_str_find(s, from)) != ZTD_STR_NPOS) {
    ztd_sb_append(b, ztd_str_slice(s, 0, i));
    ztd_sb_append(b, to);
    s = ztd_str_slice(s, i + from.len, s.len);
  }
  ztd_sb_append(b, s);
  if (b->failed)
    ztd_sb_truncate(b, start);
  return !b->failed;
}

ZTD_STR_DEF void ztd_sb_truncate(ZtdStrBuf* b, size_t len) {
  if (len >= b->len)
    return;
  b->len = len;
  b->ptr[len] = '\0';
}

ZTD_STR_DEF void ztd_sb_clear(ZtdStrBuf* b) {
  b->len = 0;
  b->failed = false;
  if (b->ptr)
    b->ptr[0] = '\0';
}

ZTD_STR_DEF void ztd_sb_free(ZtdStrBuf* b) {
  if (!b->arena)
    ZTD_STR_FREE(b->ptr);
  b->ptr = NULL;
  b->len = b->cap = 0;
  b->failed = false;
}

ZTD_STR_DEF ZtdStr ztd_sb_str(const ZtdStrBuf* b) {
  return b->ptr ? ztd_str__make(b->ptr, b->len) : ztd_str__make("", 0);
}

ZTD_STR_DEF const char* ztd_sb_cstr(const ZtdStrBuf* b) {
  return b->ptr ? b->ptr : "";
}

ZTD_STR_DEF char* ztd_sb_detach(ZtdStrBuf* b) {
  if (b->failed) {
    ztd_sb_free(b);
    return NULL;
  }
  if (!b->ptr && !ztd_sb_reserve(b, 0)) {
    ztd_sb_free(b);
    return NULL;
  }
  char* p = b->ptr;
  b->ptr = NULL;
  b->len = b->cap = 0;
  return p;
}

#endif /* ZTD_STR_IMPLEMENTATION_DONE */

/* ---- Arena integration (implementation) --------------------------------- */

#if defined(ZTD_ARENA_H) && !defined(ZTD_STR__ARENA_IMPL)
#define ZTD_STR__ARENA_IMPL

static void* (*ztd_str__arena_realloc)(struct ZtdArena* a,
                                       void* ptr,
                                       size_t old_size,
                                       size_t new_size,
                                       size_t align) = ztd_arena_realloc;

ZTD_STR_DEF ZtdArena* ztd_sb__arena_linked(ZtdArena* a) {
  return a;
}

ZTD_STR_DEF ZtdStr ztd_str_arena_dup(ZtdArena* a, ZtdStr s) {
  if (s.len == SIZE_MAX)
    return ztd_str__make(NULL, 0);
  char* p = (char*)ztd_arena_alloc(a, s.len + 1, 1);
  if (!p)
    return ztd_str__make(NULL, 0);
  if (s.len)
    memcpy(p, s.ptr, s.len);
  p[s.len] = '\0';
  return ztd_str__make(p, s.len);
}

ZTD_STR_DEF ZtdStr ztd_str_arena_vfmt(ZtdArena* a,
                                      const char* fmt,
                                      va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n < 0)
    return ztd_str__make(NULL, 0);
  char* p = (char*)ztd_arena_alloc(a, (size_t)n + 1, 1);
  if (!p)
    return ztd_str__make(NULL, 0);
  vsnprintf(p, (size_t)n + 1, fmt, ap);
  return ztd_str__make(p, (size_t)n);
}

ZTD_STR_DEF ZtdStr ztd_str_arena_fmt(ZtdArena* a, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ZtdStr r = ztd_str_arena_vfmt(a, fmt, ap);
  va_end(ap);
  return r;
}
#endif /* arena implementation */
#endif /* ZTD_STR_IMPLEMENTATION */

/* ---- Integration ----------------------------------------------------------
 * ztd_arr.h adds string lists, ztd_map.h ZtdStr keys and ztd_test.h ZtdStr
 * assertions once they see this header. If any was included first, include
 * it again so it can catch up. */
#ifdef ZTD_ARR_H
#include "ztd_arr.h"
#endif
#ifdef ZTD_MAP_H
#include "ztd_map.h"
#endif
#ifdef ZTD_TEST_H
#include "ztd_test.h"
#endif
