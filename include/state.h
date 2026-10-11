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
  Str name;           // NUL-terminated, lives in the state arena
  StrList argv;       // each Str NUL-terminated, lives in the state arena
  Str icon;           // NUL-terminated, lives in the state arena; may be empty
  bool icon_guessed;  // icon wasn't given; it is the command's basename
  enum state_run run;
};

typedef LIST(struct state_entry) StateEntryList;
typedef LIST(size_t) IndexList;

/* The app state that gets drawn. Every mutation bumps gen so caches
 * (measurements, presented frames) can tell when they are stale. */
struct state {
  Arena arena;  // owns the entries list and everything the entries point to
  StateEntryList entries;
  StrBuf query;       // heap-backed; matched case-insensitively as a substring
  size_t cursor;      // byte offset into query, on a UTF-8 char boundary
  IndexList matches;  // heap-backed; indices into entries, in entry order
  size_t selected;    // index into matches
  uint64_t gen;
};

void state_init(struct state* state);
void state_destroy(struct state* state);

/* Copies name, argv and icon into state->arena. argv must be non-empty. If
 * name is empty, argv joined with spaces is used. If icon is null (ptr NULL),
 * the basename of argv[0] is guessed; an empty non-null icon means none.
 * Returns false on allocation failure. */
bool state_push(struct state* state,
                Str name,
                const StrList* argv,
                Str icon,
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

/* Query edits refilter and reset the selection to the first match. Returns
 * false on allocation failure. */
bool state_query_insert(struct state* state, Str text);  // at the cursor
/* Deletes between the cursor and where the motion would move it. */
bool state_query_delete(struct state* state, enum state_motion motion);
bool state_query_clear(struct state* state);
/* Replaces the query, with the cursor at its end. */
bool state_query_set(struct state* state, Str text);
void state_cursor_move(struct state* state, enum state_motion motion);

void state_select_next(struct state* state);
void state_select_prev(struct state* state);
/* The i-th match. i must be < state->matches.len. */
const struct state_entry* state_match(const struct state* state, size_t i);
/* Byte offset of the query within the i-th match's name; the matched part is
 * query.len bytes long. */
size_t state_match_offset(const struct state* state, size_t i);
const struct state_entry* state_get_selection(struct state* state);
#endif  // !LM_STATE_H
