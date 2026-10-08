#include "state.h"
#include "dlist.h"
#include "ds.h"

// -- state_entry

struct state_entry *state_entry_init(struct state_entry *entry,
                                     const char *name, const DList argv) {
  entry->argv = argv;
  entry->name =
      name != NULL ? string_of(name) : state_entry_join_argv(entry, " ");
  if (entry->name.data == NULL) {
    return NULL;
  }
  return entry;
}

void state_entry_free(struct state_entry *entry) {
  string_deinit(&entry->name);
  dlist_clear(&entry->argv);
}

String state_entry_join_argv(const struct state_entry *entry, const char *sep) {
  String s = {0};
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
  for (int i = 0; i < arrlen(state->entries); ++i) {
    state_entry_free(&state->entries[i]);
  }
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
  if (arrlen(state->entries) == 0) {
    return NULL;
  }
  return &state->entries[state->selected];
}
