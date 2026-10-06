#ifndef LM_VIEW_H
#define LM_VIEW_H

#include "ds.h"

#include <stddef.h>
#include <stdint.h>

/* The app state that gets drawn. Every mutation bumps gen so caches
 * (measurements, presented frames) can tell when they are stale. */
struct view {
  struct str_arr items;
  size_t selected;
  uint64_t gen;
};

void view_init(struct view *view);
void view_destroy(struct view *view);

/* Returns 0 on success, -1 on allocation failure. */
int view_push(struct view *view, const char *s, size_t len);
void view_select_next(struct view *view);
void view_select_prev(struct view *view);

#endif // !LM_VIEW_H
