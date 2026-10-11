#include "state.h"

#include <ctype.h>
#include <string.h>

void state_init(struct state* state) {
  state->arena = (Arena)ARENA_INIT;
  state->entries = (StateEntryList)LIST_INIT_ARENA(&state->arena);
  state->query = (StrBuf)STRBUF_INIT;
  state->cursor = 0;
  state->matches = (IndexList)LIST_INIT;
  state->selected = 0;
  state->gen = 0;
}

void state_destroy(struct state* state) {
  arena_free(&state->arena);
  sb_free(&state->query);
  LIST_FREE(&state->matches);
  state->cursor = 0;
  state->entries = (StateEntryList)LIST_INIT;
  state->selected = 0;
  state->gen = 0;
}

/* ASCII case-insensitive substring search; an empty needle matches at 0.
 * Returns STR_NPOS if there is no match. */
static size_t find_nocase(Str hay, Str needle) {
  if (needle.len > hay.len)
    return STR_NPOS;
  for (size_t i = 0; i + needle.len <= hay.len; i++) {
    size_t j = 0;
    while (j < needle.len && tolower((unsigned char)hay.ptr[i + j]) ==
                                 tolower((unsigned char)needle.ptr[j]))
      j++;
    if (j == needle.len)
      return i;
  }
  return STR_NPOS;
}

static bool contains_nocase(Str hay, Str needle) {
  return find_nocase(hay, needle) != STR_NPOS;
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

bool state_push(struct state* state,
                Str name,
                const StrList* argv,
                Str icon,
                bool detached) {
  struct state_entry e = {
      .argv = LIST_INIT_ARENA(&state->arena),
      .icon = str_arena_dup(&state->arena, icon),
      .detached = detached,
  };
  if (e.icon.ptr == NULL || !LIST_RESERVE(&e.argv, argv->len)) {
    return false;
  }
  ARR_FOREACH(const Str, w, argv) {
    Str owned = str_arena_dup(&state->arena, *w);
    if (owned.ptr == NULL || !LIST_PUSH(&e.argv, owned)) {
      return false;
    }
  }

  /* Apps are usually installed with an icon named after their binary. The
   * basename ends where argv[0] does, so it stays NUL-terminated. */
  if (e.icon.len == 0) {
    Str cmd = e.argv.ptr[0];
    size_t slash = str_rfind_char(cmd, '/');
    e.icon = slash == STR_NPOS ? cmd : str_slice(cmd, slash + 1, cmd.len);
    e.icon_guessed = true;
  }

  if (name.len != 0) {
    e.name = str_arena_dup(&state->arena, name);
  } else {
    StrBuf b = STRBUF_INIT_ARENA(&state->arena);
    if (!strlist_join(&b, &e.argv, STR(" ")) || !sb_reserve(&b, 0)) {
      return false;
    }
    e.name = sb_str(&b);
  }
  if (e.name.ptr == NULL) {
    return false;
  }

  if (!LIST_PUSH(&state->entries, e)) {
    return false;
  }
  state->gen++;

  if (contains_nocase(e.name, sb_str(&state->query)) &&
      !LIST_PUSH(&state->matches, state->entries.len - 1)) {
    return false;
  }
  return true;
}

static bool is_cont(char c) {
  return ((unsigned char)c & 0xC0) == 0x80;  // UTF-8 continuation byte
}

static size_t motion_target(const struct state* state,
                            enum state_motion motion) {
  const char* q = state->query.ptr;
  size_t len = state->query.len, i = state->cursor;
  switch (motion) {
    case STATE_MOTION_CHAR_LEFT:
      if (i > 0)
        do
          i--;
        while (i > 0 && is_cont(q[i]));
      break;
    case STATE_MOTION_CHAR_RIGHT:
      if (i < len)
        do
          i++;
        while (i < len && is_cont(q[i]));
      break;
    case STATE_MOTION_WORD_LEFT:
      while (i > 0 && isspace((unsigned char)q[i - 1]))
        i--;
      while (i > 0 && !isspace((unsigned char)q[i - 1]))
        i--;
      break;
    case STATE_MOTION_WORD_RIGHT:
      while (i < len && isspace((unsigned char)q[i]))
        i++;
      while (i < len && !isspace((unsigned char)q[i]))
        i++;
      break;
    case STATE_MOTION_START:
      i = 0;
      break;
    case STATE_MOTION_END:
      i = len;
      break;
  }
  return i;
}

bool state_query_insert(struct state* state, Str text) {
  if (!sb_insert(&state->query, state->cursor, text))
    return false;
  state->cursor += text.len;
  return refilter(state);
}

bool state_query_delete(struct state* state, enum state_motion motion) {
  size_t a = state->cursor, b = motion_target(state, motion);
  if (a == b)
    return true;
  if (a > b) {
    size_t t = a;
    a = b;
    b = t;
  }
  char* q = state->query.ptr;
  size_t len = state->query.len;
  memmove(q + a, q + b, len - b);
  sb_truncate(&state->query, len - (b - a));
  state->cursor = a;
  return refilter(state);
}

bool state_query_clear(struct state* state) {
  if (state->query.len == 0)
    return true;
  sb_clear(&state->query);
  state->cursor = 0;
  return refilter(state);
}

void state_cursor_move(struct state* state, enum state_motion motion) {
  size_t i = motion_target(state, motion);
  if (i == state->cursor)
    return;
  state->cursor = i;
  state->gen++;
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

size_t state_match_offset(const struct state* state, size_t i) {
  return find_nocase(state_match(state, i)->name, sb_str(&state->query));
}

const struct state_entry* state_get_selection(struct state* state) {
  if (state->matches.len == 0) {
    return NULL;
  }
  return state_match(state, state->selected);
}
