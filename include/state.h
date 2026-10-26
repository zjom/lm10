#ifndef LM_STATE_H
#define LM_STATE_H

#include "ztd_arena.h"
#include "ztd_arr.h"
#include "ztd_str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What picking an entry does. */
enum state_run {
  STATE_RUN_PRINT,   // print the argv to stdout
  STATE_RUN_EXEC,    // exec in place of lm10
  STATE_RUN_DETACH,  // run in a new session, detached from lm10
};

struct state_entry {
  Str name;      // valid UTF-8, NUL-terminated, lives in the state arena
  StrList argv;  // each Str NUL-terminated, lives in the state arena
  enum state_run run;
};

typedef LIST(struct state_entry) StateEntryList;
typedef LIST(uint32_t) CodepointList;

/* An entry that matches the query, and how well (fuzzy only). */
struct state_match {
  size_t entry;  // index into entries
  int32_t score;
};
typedef LIST(struct state_match) MatchList;

/* A byte range of a name, e.g. where it matched the query. */
struct state_span {
  size_t start, len;
};
typedef LIST(struct state_span) SpanList;

/* The app state that gets drawn. Every change to it bumps gen, so the
 * renderer can tell whether there is anything new to draw. */
struct state {
  Arena arena;  // owns the entries list and everything the entries point to
  StateEntryList entries;
  StrBuf query;   // heap-backed, valid UTF-8
  size_t cursor;  // byte offset into query, on a UTF-8 char boundary
  /* The query lowercased, code point by code point: matched against names
   * ignoring case, as a substring, or with fuzzy as a subsequence. */
  CodepointList needle;  // heap-backed
  bool fuzzy;
  /* In entry order, or with fuzzy the best match first once sorted.
   * Heap-backed. */
  MatchList matches;
  size_t sorted;    // with fuzzy, matches before this one are ranked
  size_t selected;  // index into matches
  /* Texts drawn around the query; actions can change them. Heap-backed. */
  StrBuf prompt;
  StrBuf header;  // lines between the query and the rows; may be empty
  uint64_t gen;
};

void state_init(struct state* state);
void state_destroy(struct state* state);

/* Copies name and argv into state->arena. argv must be non-empty. If name
 * is empty, argv joined with spaces is used; invalid UTF-8 in it is
 * replaced. */
void state_push(struct state* state,
                Str name,
                const StrList* argv,
                enum state_run run);

/* Drops every entry, keeping the query. */
void state_clear_entries(struct state* state);

/* Cursor targets. Words are runs of non-space characters. */
enum state_motion {
  STATE_MOTION_CHAR_LEFT,
  STATE_MOTION_CHAR_RIGHT,
  STATE_MOTION_WORD_LEFT,
  STATE_MOTION_WORD_RIGHT,
  STATE_MOTION_START,
  STATE_MOTION_END,
};

/* Query edits refilter and reset the selection to the first match. Invalid
 * UTF-8 in text is replaced. */
void state_query_insert(struct state* state, Str text);  // at the cursor
/* Deletes between the cursor and where the motion would move it. */
void state_query_delete(struct state* state, enum state_motion motion);
void state_query_clear(struct state* state);
/* Replaces the query, with the cursor at its end. */
void state_query_set(struct state* state, Str text);
void state_cursor_move(struct state* state, enum state_motion motion);

/* Replace the texts around the query (the header's lines are split by \n). */
void state_set_prompt(struct state* state, Str text);
void state_set_header(struct state* state, Str text);

/* Matches the query as a subsequence, ranking the best matches first,
 * rather than as a substring in entry order. */
void state_set_fuzzy(struct state* state, bool fuzzy);

/* Puts fuzzy matches of entries pushed since the last query change in
 * their place by score. Pushing only appends them, so a stream of entries
 * isn't ranked one insert at a time; call this before reading matches in
 * order. */
void state_sort(struct state* state);

/* These sort first, so the selection moves through the ranked matches. */
void state_select_next(struct state* state);
void state_select_prev(struct state* state);
/* Selects the i-th match. i must be < state->matches.len. */
void state_select(struct state* state, size_t i);
/* The i-th match. i must be < state->matches.len. */
const struct state_entry* state_match(const struct state* state, size_t i);
/* Sets spans to the byte ranges of the i-th match's name that matched the
 * query: one for a substring, up to one per character when fuzzy, none
 * for an empty query. */
void state_match_spans(const struct state* state, size_t i, SpanList* spans);
/* The selected match, or NULL if there are none. */
const struct state_entry* state_get_selection(const struct state* state);

#endif  // !LM_STATE_H
