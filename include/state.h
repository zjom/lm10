#ifndef LM_STATE_H
#define LM_STATE_H

#include "ztd_arena.h"
#include "ztd_arr.h"
#include "ztd_str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct state_entry {
  Str name;      // NUL-terminated, lives in the state arena
  StrList argv;  // each Str NUL-terminated, lives in the state arena
};

typedef LIST(struct state_entry) StateEntryList;

/* The app state that gets drawn. Every mutation bumps gen so caches
 * (measurements, presented frames) can tell when they are stale. */
struct state {
  Arena arena;  // owns the entries list and everything the entries point to
  StateEntryList entries;
  size_t selected;
  uint64_t gen;
};

void state_init(struct state* state);
void state_destroy(struct state* state);

/* argv must live in state->arena. If name is NULL, the argv joined with
 * spaces is used. Returns false on allocation failure. */
bool state_push(struct state* state, const char* name, StrList argv);
void state_select_next(struct state* state);
void state_select_prev(struct state* state);
const struct state_entry* state_get_selection(struct state* state);
#endif  // !LM_STATE_H
