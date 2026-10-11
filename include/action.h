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
  ACTION_RELOAD,   // sh -c arg; its output replaces the entries
  ACTION_EXECUTE,  // sh -c arg, waited for
};

struct action {
  enum action_kind kind;
  Str arg;  // NUL-terminated; empty for kinds without an argument
};

typedef LIST(struct action) ActionList;

/* Appends s to out, single-quoted unless it is made only of characters no
 * shell treats specially. Returns false on allocation failure. */
bool action_quote(StrBuf* out, Str s);

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
bool action_expand(StrBuf* out, Str tmpl, struct state* state);

/* Runs cmd with sh -c and waits for it. Its stdin is /dev/null and its stdout
 * goes to stderr, so it can't disturb lm10's output. */
void action_execute(const char* cmd);

#endif  // !LM_ACTION_H
