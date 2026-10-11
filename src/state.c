#include "state.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "util.h"

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

static uint32_t fold(const char* p) {
  return g_unichar_tolower(g_utf8_get_char(p));
}

/* Finds the needle in hay as a substring, comparing lowercased code points.
 * On a match, adds the byte span it covers to spans (if not NULL); an empty
 * needle matches at 0. hay must be valid UTF-8. */
static bool find_substring(Str hay,
                           const CodepointList* needle,
                           SpanList* spans) {
  const char* end = hay.ptr + hay.len;
  for (const char* p = hay.ptr;; p = g_utf8_next_char(p)) {
    const char* q = p;
    size_t j = 0;
    while (j < needle->len && q < end && fold(q) == needle->ptr[j]) {
      q = g_utf8_next_char(q);
      j++;
    }
    if (j == needle->len) {
      if (spans != NULL && q > p)
        LIST_PUSH(spans, ((struct state_span){(size_t)(p - hay.ptr),
                                              (size_t)(q - p)}));
      return true;
    }
    if (p >= end)
      return false;
  }
}

/* Character classes for fuzzy scoring, after fzf's. */
enum char_class {
  CLASS_SEP,
  CLASS_LOWER,
  CLASS_UPPER,
  CLASS_DIGIT,
  CLASS_OTHER
};

static enum char_class classify(gunichar c) {
  if (g_unichar_isspace(c) ||
      (c > 0 && c < 0x80 && strchr("-_./\\:,;()[]{}<>|'\"", (int)c) != NULL))
    return CLASS_SEP;
  if (g_unichar_islower(c))
    return CLASS_LOWER;
  if (g_unichar_isupper(c))
    return CLASS_UPPER;
  if (g_unichar_isdigit(c))
    return CLASS_DIGIT;
  return CLASS_OTHER;
}

enum {
  SCORE_MATCH = 16,
  SCORE_GAP_START = -3,
  SCORE_GAP_EXTENSION = -1,
  BONUS_BOUNDARY = 8,  // after a separator, or at the start
  BONUS_CAMEL = 7,     // fooBar, foo1
  BONUS_CONSECUTIVE = 4,
  BONUS_FIRST_FACTOR = 2,  // the first character's bonus counts double
};

static int boundary_bonus(enum char_class prev, enum char_class cur) {
  if (cur == CLASS_SEP)
    return 0;
  if (prev == CLASS_SEP)
    return BONUS_BOUNDARY;
  if ((prev == CLASS_LOWER && cur == CLASS_UPPER) ||
      (prev != CLASS_DIGIT && cur == CLASS_DIGIT))
    return BONUS_CAMEL;
  return 0;
}

/* Scores matching the needle's characters, in order and as early as
 * possible, within [start, stop) of hay, which must hold them all: on word
 * boundaries and next to each other is better, gaps are worse. Adds the
 * matched byte spans to spans, if not NULL. */
static int32_t score_window(Str hay,
                            const char* start,
                            const char* stop,
                            const CodepointList* needle,
                            SpanList* spans) {
  enum char_class prev = CLASS_SEP;
  if (start > hay.ptr)
    prev = classify(g_utf8_get_char(g_utf8_prev_char(start)));
  int32_t total = 0;
  int first_bonus = 0; /* of the run of consecutive matches going on */
  bool in_gap = false, in_run = false;
  size_t j = 0;
  for (const char* p = start; p < stop; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    enum char_class cls = classify(c);
    if (j < needle->len && g_unichar_tolower(c) == needle->ptr[j]) {
      int bonus = boundary_bonus(prev, cls);
      if (in_run) {
        /* A run keeps the bonus of the boundary it started on. */
        if (bonus < first_bonus)
          bonus = first_bonus;
        if (bonus < BONUS_CONSECUTIVE)
          bonus = BONUS_CONSECUTIVE;
      } else {
        first_bonus = bonus;
      }
      total += SCORE_MATCH + (j == 0 ? bonus * BONUS_FIRST_FACTOR : bonus);
      if (spans != NULL) {
        size_t off = (size_t)(p - hay.ptr);
        size_t len = (size_t)(g_utf8_next_char(p) - p);
        struct state_span* last =
            spans->len > 0 ? &spans->ptr[spans->len - 1] : NULL;
        if (in_run && last != NULL && last->start + last->len == off)
          last->len += len;
        else
          LIST_PUSH(spans, ((struct state_span){off, len}));
      }
      in_run = true;
      in_gap = false;
      j++;
    } else {
      total += in_gap ? SCORE_GAP_EXTENSION : SCORE_GAP_START;
      in_gap = true;
      in_run = false;
    }
    prev = cls;
  }
  return total;
}

/* Finds the needle's code points in hay in order, possibly apart, after
 * fzf's v1 algorithm: the first occurrence going forward, and the shortest
 * window ending where it does going back. Both windows are scored, and the
 * better one counts: the shortest can lose a word-initial first character
 * ("fb" in "firefox browser" shrinks to "fox b"). On a match, sets *score
 * and adds the matched byte spans to spans, if not NULL. */
static bool find_fuzzy(Str hay,
                       const CodepointList* needle,
                       int32_t* score,
                       SpanList* spans) {
  size_t n = needle->len;
  *score = 0;
  if (n == 0)
    return true;

  const char* end = hay.ptr + hay.len;
  const char* first = NULL;
  const char* p = hay.ptr;
  size_t j = 0;
  for (; p < end && j < n; p = g_utf8_next_char(p)) {
    if (fold(p) == needle->ptr[j]) {
      if (j == 0)
        first = p;
      j++;
    }
  }
  if (j < n)
    return false;
  const char* stop = p; /* just past the last needle character */

  const char* shortest = stop;
  for (j = n; j > 0;) {
    shortest = g_utf8_prev_char(shortest);
    if (fold(shortest) == needle->ptr[j - 1])
      j--;
  }

  const char* start = first;
  *score = score_window(hay, first, stop, needle, NULL);
  if (shortest != first) {
    int32_t s = score_window(hay, shortest, stop, needle, NULL);
    if (s > *score) {
      *score = s;
      start = shortest;
    }
  }
  if (spans != NULL)
    score_window(hay, start, stop, needle, spans);
  return true;
}

/* Whether the entry matches the query, and how well (fuzzy only). */
static bool matches(const struct state* state, size_t entry, int32_t* score) {
  Str name = state->entries.ptr[entry].name;
  *score = 0;
  if (state->fuzzy)
    return find_fuzzy(name, &state->needle, score, NULL);
  return find_substring(name, &state->needle, NULL);
}

/* Lowercases the query into the needle. */
static void fold_query(struct state* state) {
  LIST_CLEAR(&state->needle);
  const char* end = state->query.ptr + state->query.len;
  for (const char* p = state->query.ptr; p < end; p = g_utf8_next_char(p))
    LIST_PUSH(&state->needle, fold(p));
}

/* Better scores first, then entry order. */
static int compare_match(const void* a, const void* b) {
  const struct state_match *x = a, *y = b;
  if (x->score != y->score)
    return x->score > y->score ? -1 : 1;
  return x->entry < y->entry ? -1 : x->entry > y->entry;
}

/* Recomputes the matches after a query change and selects the first. If
 * narrowing, the query only gained characters at its end, so the matches
 * can only shrink: just the current ones are checked. */
static void refilter(struct state* state, bool narrowing) {
  state->selected = 0;
  state->gen++;
  fold_query(state);
  if (!narrowing) {
    LIST_CLEAR(&state->matches);
    LIST_RESERVE(&state->matches, state->entries.len);
    for (size_t i = 0; i < state->entries.len; i++)
      LIST_PUSH(&state->matches, ((struct state_match){i, 0}));
  }
  size_t kept = 0;
  for (size_t i = 0; i < state->matches.len; i++) {
    struct state_match m = state->matches.ptr[i];
    if (matches(state, m.entry, &m.score))
      state->matches.ptr[kept++] = m;
  }
  state->matches.len = kept;
  state->sorted = 0;
  state_sort(state);
}

/* Adds a new entry (the last one) to the matches if it matches, at the
 * end: state_sort ranks it. The selection stays where it is. */
static void add_match(struct state* state, size_t entry) {
  int32_t score;
  if (!matches(state, entry, &score))
    return;
  LIST_PUSH(&state->matches, ((struct state_match){entry, score}));
}

void state_sort(struct state* state) {
  struct state_match* m = state->matches.ptr;
  size_t len = state->matches.len, i = state->sorted;
  state->sorted = len;
  if (!state->fuzzy || state->needle.len == 0 || i == len)
    return;
  /* Sort the new ones, then merge them in from the back, so a stream of
   * entries costs one pass over the ranked ones per sort. */
  size_t n = len - i;
  qsort(m + i, n, sizeof *m, compare_match);
  if (i == 0)
    return;
  struct state_match* tail = xmalloc(n * sizeof *tail);
  memcpy(tail, m + i, n * sizeof *tail);
  for (size_t k = len; n > 0;) {
    if (i > 0 && compare_match(&m[i - 1], &tail[n - 1]) > 0)
      m[--k] = m[--i];
    else
      m[--k] = tail[--n];
  }
  free(tail);
}

void state_set_fuzzy(struct state* state, bool fuzzy) {
  if (state->fuzzy == fuzzy)
    return;
  state->fuzzy = fuzzy;
  refilter(state, false);
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

  add_match(state, state->entries.len - 1);
  return true;
}

void state_clear_entries(struct state* state) {
  arena_reset(&state->arena);
  state->entries = (StateEntryList)LIST_INIT_ARENA(&state->arena);
  LIST_CLEAR(&state->matches);
  state->sorted = 0;
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
  if (ok)
    refilter(state, at_end);
  return ok;
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
  refilter(state, false);
  return true;
}

bool state_query_clear(struct state* state) {
  if (state->query.len == 0)
    return true;
  sb_clear(&state->query);
  state->cursor = 0;
  refilter(state, false);
  return true;
}

bool state_query_set(struct state* state, Str text) {
  bool ok = set_utf8(&state->query, text);
  state->cursor = state->query.len;
  refilter(state, false);
  return ok;
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
  state_sort(state);
  size_t n = state->matches.len;
  if (n == 0)
    return;
  state->selected = state->selected + 1 >= n ? 0 : state->selected + 1;
  state->gen++;
}

void state_select_prev(struct state* state) {
  state_sort(state);
  size_t n = state->matches.len;
  if (n == 0)
    return;
  state->selected = state->selected == 0 ? n - 1 : state->selected - 1;
  state->gen++;
}

const struct state_entry* state_match(const struct state* state, size_t i) {
  return &state->entries.ptr[state->matches.ptr[i].entry];
}

bool state_match_spans(const struct state* state, size_t i, SpanList* spans) {
  LIST_CLEAR(spans);
  if (state->needle.len == 0)
    return true;
  Str name = state_match(state, i)->name;
  int32_t score;
  if (state->fuzzy)
    find_fuzzy(name, &state->needle, &score, spans);
  else
    find_substring(name, &state->needle, spans);
  return !spans->failed;
}

const struct state_entry* state_get_selection(const struct state* state) {
  if (state->matches.len == 0)
    return NULL;
  return state_match(state, state->selected);
}
