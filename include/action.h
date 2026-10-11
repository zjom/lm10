#ifndef LM_ACTION_H
#define LM_ACTION_H

#include "state.h"
#include "ztd_arr.h"
#include "ztd_str.h"

#include <stdbool.h>

/* What a bound key does: each bind runs a list of these in order. */
enum action_kind {
  ACTION_CANCEL,

  ACTION_DELETE_PREV,
  ACTION_DELETE_PREV_WORD,
  ACTION_DELETE_NEXT,
  ACTION_DELETE_NEXT_WORD,
  ACTION_DELETE_LINE_START,
  ACTION_DELETE_LINE_END,
  ACTION_DELETE_LINE,

  ACTION_CURSOR_LEFT,
  ACTION_CURSOR_RIGHT,
  ACTION_CURSOR_WORD_LEFT,
  ACTION_CURSOR_WORD_RIGHT,
  ACTION_CURSOR_START,
  ACTION_CURSOR_END,

  ACTION_MATCH_EXEC,
  ACTION_MATCH_NEXT,
  ACTION_MATCH_PREV,

  /* These take an argument. */
  ACTION_CHANGE_PROMPT,
  ACTION_CHANGE_QUERY,
  ACTION_CHANGE_HEADER,  // \n in arg breaks lines, as in the config
  ACTION_CHANGE_BORDER_LABEL,
  ACTION_RELOAD,   // sh -c arg; its output replaces the entries
  ACTION_EXECUTE,  // sh -c arg; the bind's later actions wait for it
};
#define ACTION_COUNT (ACTION_EXECUTE + 1)

struct action {
  enum action_kind kind;
  Str arg;  // NUL-terminated; empty for kinds without an argument
};

typedef LIST(struct action) ActionList;

/* The action called name (as in the config, e.g. "match-next"). Returns
 * false if there is none. */
bool action_from_name(Str name, enum action_kind* kind);
const char* action_name(enum action_kind kind);
/* Whether the action takes an argument, e.g. reload(CMD). */
bool action_takes_arg(enum action_kind kind);

/* Expands the placeholders in tmpl into out, each shell-quoted:
 *
 *   {}      the selected entry's command line
 *   {n}     its name
 *   {q}     the query
 *   {N}     its N-th argv word, from 0 (the command)
 *   {N..M}  argv words N to M inclusive; either end may be left out
 *
 * With no selection, or out of range, they expand to ''. Other braces are
 * copied as is. Returns false on allocation failure. */
bool action_expand(StrBuf* out, Str tmpl, const struct state* state);

#endif  // !LM_ACTION_H
