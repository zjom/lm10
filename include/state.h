#ifndef LM_STATE_H
#define LM_STATE_H

#include "ds.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct state_entry {
  String name;
  DList argv; // strings
};

struct state_entry *state_entry_init(struct state_entry *entry,
                                     const char *name, size_t argc,
                                     char *argv[]);
void state_entry_free(struct state_entry *entry);
String state_entry_join_argv(const struct state_entry *entry, const char *sep);

/* The app state that gets drawn. Every mutation bumps gen so caches
 * (measurements, presented frames) can tell when they are stale. */
struct state {
  struct state_entry *entries;
  size_t selected;
  uint64_t gen;
};

void state_init(struct state *state);
void state_destroy(struct state *state);

void state_push(struct state *state, struct state_entry entry);
void state_select_next(struct state *state);
void state_select_prev(struct state *state);
const struct state_entry *state_get_selection(struct state *state);
#endif // !LM_STATE_H
