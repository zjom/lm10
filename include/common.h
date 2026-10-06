#ifndef LM_COMMON_H
#define LM_COMMON_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct strlist {
  size_t len;
  size_t cap;
  char **items;
};

static void strlist_init(struct strlist *sl) {
  sl->cap = 0;
  sl->len = 0;
  sl->items = NULL;
}

static int strlist_resize(struct strlist *sl) {
  size_t new_cap = sl->cap ? sl->cap * 2 : 8;
  char **tmp = realloc(sl->items, new_cap * sizeof(*tmp));
  if (!tmp)
    return -1;
  sl->items = tmp;
  sl->cap = new_cap;
  return 0;
}

static int strlist_push(struct strlist *sl, const char *s) {
  if (sl->len == sl->cap) {
    if (strlist_resize(sl)) {
      return -1;
    }
  }
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (!copy)
    return -1;
  memcpy(copy, s, n);
  sl->items[sl->len++] = copy;
  return 0;
}

static void strlist_free(struct strlist *sl) {
  for (size_t i = 0; i < sl->len; i++) {
    free(sl->items[i]);
  }
  free(sl->items);
  strlist_init(sl);
}

#endif // !LM_COMMON_H
