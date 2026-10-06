#include "state.h"

void state_init(struct state *state) {
  str_arr_init(&state->items);
  state->selected = 0;
  state->gen = 0;
}

void state_destroy(struct state *state) {
  str_arr_free(&state->items);
  state->selected = 0;
  state->gen = 0;
}

int state_push(struct state *state, const char *s, size_t len) {
  if (str_arr_push(&state->items, s, len) < 0)
    return -1;
  state->gen++;
  return 0;
}

void state_select_next(struct state *state) {
  size_t n = str_arr_len(&state->items);
  if (n == 0)
    return;
  state->selected = state->selected + 1 >= n ? 0 : state->selected + 1;
  state->gen++;
}

void state_select_prev(struct state *state) {
  size_t n = str_arr_len(&state->items);
  if (n == 0)
    return;
  state->selected = state->selected == 0 ? n - 1 : state->selected - 1;
  state->gen++;
}

const char *state_get_selection(struct state *state) {
  return str_arr_get(&state->items, state->selected).s;
}
