#include "state.h"

#include <ctype.h>

void state_init(struct state* state) {
  state->arena = (Arena)ARENA_INIT;
  state->entries = (StateEntryList)LIST_INIT_ARENA(&state->arena);
  state->query = (StrBuf)STRBUF_INIT;
  state->matches = (IndexList)LIST_INIT;
  state->selected = 0;
  state->gen = 0;
}

void state_destroy(struct state* state) {
  arena_free(&state->arena);
  sb_free(&state->query);
  LIST_FREE(&state->matches);
  state->entries = (StateEntryList)LIST_INIT;
  state->selected = 0;
  state->gen = 0;
}

/* ASCII case-insensitive substring search; an empty needle matches. */
static bool contains_nocase(Str hay, Str needle) {
  if (needle.len > hay.len)
    return false;
  for (size_t i = 0; i + needle.len <= hay.len; i++) {
    size_t j = 0;
    while (j < needle.len && tolower((unsigned char)hay.ptr[i + j]) ==
                                 tolower((unsigned char)needle.ptr[j]))
      j++;
    if (j == needle.len)
      return true;
  }
  return false;
}

static bool refilter(struct state* state) {
  Str q = sb_str(&state->query);
  LIST_CLEAR(&state->matches);
  state->selected = 0;
  state->gen++;
  for (size_t i = 0; i < state->entries.len; i++) {
    if (contains_nocase(state->entries.ptr[i].name, q) &&
        !LIST_PUSH(&state->matches, i))
      return false;
  }
  return true;
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

  if (contains_nocase(s, sb_str(&state->query)) &&
      !LIST_PUSH(&state->matches, state->entries.len - 1)) {
    return false;
  }
  return true;
}

bool state_query_append(struct state* state, Str text) {
  if (!sb_append(&state->query, text))
    return false;
  return refilter(state);
}

bool state_query_delete_prev(struct state* state) {
  size_t len = state->query.len;
  if (len == 0)
    return true;
  /* Back up over UTF-8 continuation bytes to the start of the last char. */
  do
    len--;
  while (len > 0 && ((unsigned char)state->query.ptr[len] & 0xC0) == 0x80);
  sb_truncate(&state->query, len);
  return refilter(state);
}

bool state_query_delete_prev_word(struct state* state) {
  size_t len = state->query.len;
  if (len == 0)
    return true;
  const char* q = state->query.ptr;
  while (len > 0 && isspace((unsigned char)q[len - 1]))
    len--;
  while (len > 0 && !isspace((unsigned char)q[len - 1]))
    len--;
  sb_truncate(&state->query, len);
  return refilter(state);
}

bool state_query_clear(struct state* state) {
  if (state->query.len == 0)
    return true;
  sb_clear(&state->query);
  return refilter(state);
}

void state_select_next(struct state* state) {
  size_t n = state->matches.len;
  if (n == 0)
    return;
  state->selected = state->selected + 1 >= n ? 0 : state->selected + 1;
  state->gen++;
}

void state_select_prev(struct state* state) {
  size_t n = state->matches.len;
  if (n == 0)
    return;
  state->selected = state->selected == 0 ? n - 1 : state->selected - 1;
  state->gen++;
}

const struct state_entry* state_match(const struct state* state, size_t i) {
  return &state->entries.ptr[state->matches.ptr[i]];
}

const struct state_entry* state_get_selection(struct state* state) {
  if (state->matches.len == 0) {
    return NULL;
  }
  return state_match(state, state->selected);
}
