#include "state.h"

#include <ctype.h>
#include <string.h>

#include <glib.h>

void state_init(struct state* state) {
  *state = (struct state){
      .arena = ARENA_INIT,
      .query = STRBUF_INIT,
      .needle = LIST_INIT,
      .matches = LIST_INIT,
      .prompt = STRBUF_INIT,
      .header = STRBUF_INIT,
      .border_label = STRBUF_INIT,
  };
  state->entries = (StateEntryList)LIST_INIT_ARENA(&state->arena);
}

void state_destroy(struct state* state) {
  arena_free(&state->arena);
  sb_free(&state->query);
  LIST_FREE(&state->needle);
  LIST_FREE(&state->matches);
  sb_free(&state->prompt);
  sb_free(&state->header);
  sb_free(&state->border_label);
  *state = (struct state){0};
}

/* ---- matching ---- */

/* Finds the needle in hay, comparing lowercased code points. On a match,
 * sets the byte span it covers in hay; an empty needle matches at 0. hay
 * must be valid UTF-8. */
static bool find(Str hay,
                 const CodepointList* needle,
                 size_t* start,
                 size_t* len) {
  const char* end = hay.ptr + hay.len;
  for (const char* p = hay.ptr;; p = g_utf8_next_char(p)) {
    const char* q = p;
    size_t j = 0;
    while (j < needle->len && q < end &&
           g_unichar_tolower(g_utf8_get_char(q)) == needle->ptr[j]) {
      q = g_utf8_next_char(q);
      j++;
    }
    if (j == needle->len) {
      *start = (size_t)(p - hay.ptr);
      *len = (size_t)(q - p);
      return true;
    }
    if (p >= end)
      return false;
  }
}

static bool matches(const struct state* state, size_t entry) {
  size_t start, len;
  return find(state->entries.ptr[entry].name, &state->needle, &start, &len);
}

/* Lowercases the query into the needle. Returns false on allocation
 * failure. */
static bool fold_query(struct state* state) {
  LIST_CLEAR(&state->needle);
  const char* end = state->query.ptr + state->query.len;
  for (const char* p = state->query.ptr; p < end; p = g_utf8_next_char(p)) {
    if (!LIST_PUSH(&state->needle, g_unichar_tolower(g_utf8_get_char(p))))
      return false;
  }
  return true;
}

/* Recomputes the matches after a query change and selects the first. If
 * narrowing, the query only gained characters at its end, so the matches
 * can only shrink: just the current ones are checked. */
static bool refilter(struct state* state, bool narrowing) {
  state->selected = 0;
  state->gen++;
  if (!fold_query(state))
    return false;

  if (narrowing) {
    size_t kept = 0;
    for (size_t i = 0; i < state->matches.len; i++) {
      if (matches(state, state->matches.ptr[i]))
        state->matches.ptr[kept++] = state->matches.ptr[i];
    }
    state->matches.len = kept;
    return true;
  }

  LIST_CLEAR(&state->matches);
  for (size_t i = 0; i < state->entries.len; i++) {
    if (matches(state, i) && !LIST_PUSH(&state->matches, i))
      return false;
  }
  return true;
}

/* Sets b to s, with invalid UTF-8 replaced. Returns false on allocation
 * failure. */
static bool set_utf8(StrBuf* b, Str s) {
  sb_clear(b);
  if (g_utf8_validate_len(s.ptr, s.len, NULL))
    return sb_append(b, s);
  gchar* valid = g_utf8_make_valid(s.ptr, (gssize)s.len);
  bool ok = sb_append_cstr(b, valid);
  g_free(valid);
  return ok;
}

/* ---- entries ---- */

bool state_push(struct state* state,
                Str name,
                const StrList* argv,
                Str icon,
                enum state_run run) {
  struct state_entry e = {
      .argv = LIST_INIT_ARENA(&state->arena),
      .run = run,
  };
  if (icon.ptr != NULL) {
    e.icon = str_arena_dup(&state->arena, icon);
    if (e.icon.ptr == NULL)
      return false;
  }
  if (!LIST_RESERVE(&e.argv, argv->len))
    return false;
  ARR_FOREACH(const Str, w, argv) {
    Str owned = str_arena_dup(&state->arena, *w);
    if (owned.ptr == NULL || !LIST_PUSH(&e.argv, owned))
      return false;
  }

  /* Apps are usually installed with an icon named after their binary. The
   * basename ends where argv[0] does, so it stays NUL-terminated. */
  if (icon.ptr == NULL) {
    Str cmd = e.argv.ptr[0];
    size_t slash = str_rfind_char(cmd, '/');
    e.icon = slash == STR_NPOS ? cmd : str_slice(cmd, slash + 1, cmd.len);
    e.icon_guessed = true;
  }

  /* The name is drawn and matched as text, so it must be valid UTF-8. */
  StrBuf b = STRBUF_INIT_ARENA(&state->arena);
  if (name.len != 0) {
    if (!set_utf8(&b, name))
      return false;
  } else {
    StrBuf joined = STRBUF_INIT;
    bool ok = strlist_join(&joined, &e.argv, STR(" ")) &&
              set_utf8(&b, sb_str(&joined));
    sb_free(&joined);
    if (!ok)
      return false;
  }
  /* An arena StrBuf is NUL-terminated once it has storage; reserve some
   * even for an empty name. */
  if (!sb_reserve(&b, 0))
    return false;
  e.name = sb_str(&b);

  if (!LIST_PUSH(&state->entries, e))
    return false;
  state->gen++;

  if (matches(state, state->entries.len - 1) &&
      !LIST_PUSH(&state->matches, state->entries.len - 1))
    return false;
  return true;
}

void state_clear_entries(struct state* state) {
  arena_reset(&state->arena);
  state->entries = (StateEntryList)LIST_INIT_ARENA(&state->arena);
  LIST_CLEAR(&state->matches);
  state->selected = 0;
  state->gen++;
}

/* ---- query ---- */

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
  StrBuf valid = STRBUF_INIT;
  if (!set_utf8(&valid, text)) {
    sb_free(&valid);
    return false;
  }
  bool at_end = state->cursor == state->query.len;
  bool ok = sb_insert(&state->query, state->cursor, sb_str(&valid));
  if (ok)
    state->cursor += valid.len;
  sb_free(&valid);
  return ok && refilter(state, at_end);
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
  return refilter(state, false);
}

bool state_query_clear(struct state* state) {
  if (state->query.len == 0)
    return true;
  sb_clear(&state->query);
  state->cursor = 0;
  return refilter(state, false);
}

bool state_query_set(struct state* state, Str text) {
  bool ok = set_utf8(&state->query, text);
  state->cursor = state->query.len;
  return refilter(state, false) && ok;
}

void state_cursor_move(struct state* state, enum state_motion motion) {
  size_t i = motion_target(state, motion);
  if (i == state->cursor)
    return;
  state->cursor = i;
  state->gen++;
}

/* ---- texts ---- */

static bool set_text(struct state* state, StrBuf* dst, Str text) {
  state->gen++;
  return set_utf8(dst, text);
}

bool state_set_prompt(struct state* state, Str text) {
  return set_text(state, &state->prompt, text);
}

bool state_set_header(struct state* state, Str text) {
  return set_text(state, &state->header, text);
}

bool state_set_border_label(struct state* state, Str text) {
  return set_text(state, &state->border_label, text);
}

/* ---- selection ---- */

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

bool state_match_span(const struct state* state,
                      size_t i,
                      size_t* start,
                      size_t* len) {
  return state->needle.len > 0 &&
         find(state_match(state, i)->name, &state->needle, start, len);
}

const struct state_entry* state_get_selection(const struct state* state) {
  if (state->matches.len == 0)
    return NULL;
  return state_match(state, state->selected);
}
