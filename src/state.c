#include "state.h"
#include "dlist.h"
#include "ds.h"

// -- state_entry

void state_entry_free(struct state_entry *entry) {
  string_deinit(&entry->name);
  dlist_clear(&entry->argv);
}

char **state_entry_argv(const struct state_entry *entry) {
  size_t argc = dlist_size(&entry->argv);
  char **retv = malloc(argc * sizeof(*retv));
  size_t i = 0;
  DLIST_FOREACH(&entry->argv, n) {
    retv[i] = DLIST_AT(n, String)->data;
    i++;
  }

  return retv;
}

String state_entry_join_argv(const struct state_entry *entry, const char *sep) {
  String s;
  string_init(&s);
  size_t i = 0;
  size_t len = dlist_size(&entry->argv);
  DLIST_FOREACH(&entry->argv, n) {
    string_append(&s, DLIST_AT(n, String)->data);
    if (i != len - 1) {
      string_append(&s, sep);
    }
    i++;
  }
  return s;
}

// -- state

void state_init(struct state *state) {
  state->entries = NULL;
  state->selected = 0;
  state->gen = 0;
}

void state_destroy(struct state *state) {
  arrfree(state->entries);
  state->entries = NULL;
  state->selected = 0;
  state->gen = 0;
}

void state_push(struct state *state, struct state_entry entry) {
  arrpush(state->entries, entry);
  state->gen++;
  return;
}

void state_select_next(struct state *state) {
  size_t n = arrlen(state->entries);
  if (n == 0)
    return;
  state->selected = state->selected + 1 >= n ? 0 : state->selected + 1;
  state->gen++;
}

void state_select_prev(struct state *state) {
  size_t n = arrlen(state->entries);
  if (n == 0)
    return;
  state->selected = state->selected == 0 ? n - 1 : state->selected - 1;
  state->gen++;
}

const struct state_entry *state_get_selection(struct state *state) {
  return &state->entries[state->selected];
}
