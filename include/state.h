#ifndef LM_STATE_H
#define LM_STATE_H

#include "ds.h"

#include <stddef.h>
#include <stdint.h>

/* The app state that gets drawn. Every mutation bumps gen so caches
 * (measurements, presented frames) can tell when they are stale. */
struct state {
  struct str_arr items;
  size_t selected;
  uint64_t gen;
};

void state_init(struct state *state);
void state_destroy(struct state *state);

/* Returns 0 on success, -1 on allocation failure. */
int state_push(struct state *state, const char *s, size_t len);
void state_select_next(struct state *state);
void state_select_prev(struct state *state);
const char *state_get_selection(struct state *state);
#endif // !LM_STATE_H
