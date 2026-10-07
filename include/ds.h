#ifndef INCLUDE_LM_DS_H
#define INCLUDE_LM_DS_H

#include "dlist.h"
#include "stb_ds.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -- heap allocated String
// Invariant: cap counts bytes allocated *including* the NUL terminator,
// so len < cap and data[len] == '\0' whenever data != NULL.
// A zero-initialised String ({0}) is a valid empty string.
#define STRING_INIT_N 8

typedef struct {
  size_t len;
  size_t cap;
  char *data;
} String;

// ensures at least `need` bytes are allocated (need includes the NUL)
// returns 0 on allocation failure, s is left unchanged
static inline int string_reserve(String *s, size_t need) {
  if (need <= s->cap) {
    return 1;
  }

  size_t want = s->cap ? s->cap : STRING_INIT_N;
  while (want < need) {
    if (want > SIZE_MAX / 2) {
      want = need;
      break;
    }
    want *= 2;
  }

  char *data = realloc(s->data, want);
  if (data == NULL) {
    return 0;
  }
  if (s->data == NULL) {
    data[0] = '\0'; // fresh buffer from a {0} String: establish invariant
  }
  s->data = data;
  s->cap = want;
  return 1;
}

static inline char *string_init_n(String *s, size_t n) {
  if (n == 0) {
    n = 1; // always room for the NUL
  }
  char *data = calloc(n, sizeof(*s->data));
  if (data == NULL) {
    return NULL;
  }

  *s = (String){.len = 0, .cap = n, .data = data};
  return data;
}

// initialises s with default capacity and len 0
// s->data is zeroed
// returns pointer to first character of string
// returns NULL if allocation errored
static inline char *string_init(String *s) {
  return string_init_n(s, STRING_INIT_N);
}

static inline void string_deinit(String *s) {
  free(s->data);
  *s = (String){0}; // data = NULL, so a second deinit is harmless
}

// returns pointer to first character of string
// returns NULL if allocation errored
static inline char *string_append_c(String *s, char c) {
  if (s->len > SIZE_MAX - 2 || !string_reserve(s, s->len + 2)) { // char + NUL
    return NULL;
  }

  s->data[s->len++] = c;
  s->data[s->len] = '\0';
  return s->data;
}

// builds a String from a sized, not necessarily NUL-terminated buffer
// on failure (or src == NULL) *dst is set to {0} and NULL is returned
static inline char *string_from_n(String *dst, const char *src, size_t len) {
  *dst = (String){0};
  if (src == NULL || len == SIZE_MAX) {
    return NULL;
  }
  String s;
  if (string_init_n(&s, len + 1) == NULL) {
    return NULL;
  }

  memcpy(s.data, src, len);
  s.data[len] = '\0';
  s.len = len;
  *dst = s;
  return s.data;
}

// returns NULL if allocation errored or src == NULL; *dst is then {0}
static inline char *string_from(String *dst, const char *src) {
  return string_from_n(dst, src, src ? strlen(src) : 0);
}

// returns {0} if allocation errored or src == NULL
static inline String string_of(const char *src) {
  String dst;
  string_from(&dst, src);
  return dst;
}

// src must not point into dst->data (realloc may move it)
// returns NULL if allocation errored
static inline char *string_append(String *dst, const char *src) {
  size_t src_len = strlen(src);
  if (src_len > SIZE_MAX - 1 - dst->len ||
      !string_reserve(dst, dst->len + src_len + 1)) {
    return NULL;
  }

  memcpy(dst->data + dst->len, src, src_len + 1); // copies the NUL too
  dst->len += src_len;
  return dst->data;
}

// -- str: a non-owning view of `len` bytes. NOT NUL-terminated in general.

struct str {
  const char *data;
  size_t len;
};

static inline struct str str_of_string(const String *s) {
  assert(s != NULL);
  return (struct str){.data = s->data, .len = s->len};
}

static inline struct str str_of_cstring(const char *s) {
  assert(s != NULL);
  return (struct str){.data = s, .len = strlen(s)};
}

// removes the first n bytes from s and returns them
// if s is shorter than n, s is unchanged and an empty str is returned
static struct str str_cut_left(struct str *s, size_t n) {
  if (s->len < n) {
    return (struct str){.data = NULL, .len = 0};
  }

  struct str out = {.data = s->data, .len = n};
  s->data += n;
  s->len -= n;
  return out;
}

// removes the last n bytes from s and returns them
// if s is shorter than n, s is unchanged and an empty str is returned
static inline struct str str_cut_right(struct str *s, size_t n) {
  if (s->len < n) {
    return (struct str){.data = NULL, .len = 0};
  }

  struct str out = {.data = s->data + s->len - n, .len = n};
  s->len -= n;
  return out;
}

// strips leading and trailing bytes matching pred (e.g. isspace)
static inline void str_trim(struct str *s, int (*pred)(int)) {
  // cast to unsigned char: ctype functions are UB on negative chars
  while (s->len > 0 && pred((unsigned char)s->data[0])) {
    s->data += 1;
    s->len -= 1;
  }
  while (s->len > 0 && pred((unsigned char)s->data[s->len - 1])) {
    s->len -= 1;
  }
}

// returns the part of s before the first delim and advances s past the delim
// if delim is absent, returns all of s and leaves s empty
static inline struct str str_split_left(struct str *s, char delim) {
  const char *p = s->len ? memchr(s->data, delim, s->len) : NULL;
  if (p == NULL) {
    return str_cut_left(s, s->len);
  }

  struct str out = str_cut_left(s, (size_t)(p - s->data));
  s->data += 1; // skip the delimiter
  s->len -= 1;
  return out;
}

// copies the view into a new owned String
// returns {0} on allocation failure
static inline String str_to_owned(const struct str *s) {
  String out;
  string_from_n(&out, s->data ? s->data : "", s->len);
  return out;
}

#endif // INCLUDE_LM_DS_H
