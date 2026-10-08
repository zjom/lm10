#include "state.h"

void state_init(struct state* state) {
  state->arena = (Arena)ARENA_INIT;
  state->entries = (StateEntryList)LIST_INIT_ARENA(&state->arena);
  state->selected = 0;
  state->gen = 0;
}

void state_destroy(struct state* state) {
  arena_free(&state->arena);
  state->entries = (StateEntryList)LIST_INIT;
  state->selected = 0;
  state->gen = 0;
}

bool state_push(struct state* state, const char* name, StrList argv) {
  Str s;
  if (name != NULL) {
    s = str_arena_dup(&state->arena, str_from_cstr(name));
  } else {
    StrBuf b = STRBUF_INIT_ARENA(&state->arena);
    if (!strlist_join(&b, &argv, STR(" ")) || !sb_reserve(&b, 0)) {
      return false;
    }
    s = sb_str(&b);
  }
  if (s.ptr == NULL) {
    return false;
  }

  if (!LIST_PUSH(&state->entries, (struct state_entry){.name = s, .argv = argv})) {
    return false;
  }
  state->gen++;
  return true;
}

void state_select_next(struct state* state) {
  size_t n = state->entries.len;
  if (n == 0)
    return;
  state->selected = state->selected + 1 >= n ? 0 : state->selected + 1;
  state->gen++;
}

void state_select_prev(struct state* state) {
  size_t n = state->entries.len;
  if (n == 0)
    return;
  state->selected = state->selected == 0 ? n - 1 : state->selected - 1;
  state->gen++;
}

const struct state_entry* state_get_selection(struct state* state) {
  if (state->entries.len == 0) {
    return NULL;
  }
  return &state->entries.ptr[state->selected];
}
