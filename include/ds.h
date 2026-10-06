#ifndef INCLUDE_LM_DS_H
#define INCLUDE_LM_DS_H

#include "stb_ds.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct str_state {
  const char *s;
  size_t len;
};

struct str_span {
  size_t off, len;
};

struct str_arena {
  char *base;
  size_t used, cap;
};

/* Returns the offset of the copied string, or SIZE_MAX on failure. */
static inline size_t str_arena_push(struct str_arena *a, const char *src,
                                    size_t len) {
  if (len > SIZE_MAX - a->used - 1)
    return SIZE_MAX; // overflow guard
  size_t need = a->used + len + 1;

  if (need > a->cap) {
    size_t want = a->cap ? a->cap : 64;
    while (want < need) {
      if (want > SIZE_MAX / 2) {
        want = need;
        break;
      }
      want *= 2;
    }
    char *nb = realloc(a->base, want);
    if (!nb)
      return SIZE_MAX; // old block still valid
    a->base = nb;
    a->cap = want;
  }

  size_t off = a->used;
  if (len)
    memcpy(a->base + off, src, len);
  a->base[off + len] = '\0';
  a->used = need;
  return off;
}

struct str_arr {
  struct str_span *spans; // stb_ds array
  struct str_arena arena;
};

static inline void str_arr_init(struct str_arr *arr) {
  *arr = (struct str_arr){0};
}

static inline void str_arr_free(struct str_arr *arr) {
  arrfree(arr->spans);
  free(arr->arena.base);
  *arr = (struct str_arr){0};
}

/* Returns the new string's index, or -1 on allocation failure. */
static inline ptrdiff_t str_arr_push(struct str_arr *arr, const char *src,
                                     size_t len) {
  size_t off = str_arena_push(&arr->arena, src, len);
  if (off == SIZE_MAX)
    return -1;
  arrput(arr->spans, ((struct str_span){.off = off, .len = len}));
  return arrlen(arr->spans) - 1;
}

static inline ptrdiff_t str_arr_len(const struct str_arr *arr) {
  return arrlen(arr->spans);
}

/* The returned state is valid until the next push. */
static inline struct str_state str_arr_get(const struct str_arr *arr,
                                           ptrdiff_t i) {
  struct str_span sp = arr->spans[i];
  return (struct str_state){arr->arena.base + sp.off, sp.len};
}

#endif // !INCLUDE_LM_DS_H
