#ifndef LM_ENTRY_PARSE_H
#define LM_ENTRY_PARSE_H

#include "state.h"
#include "ztd_arr.h"
#include "ztd_str.h"

#include <stdbool.h>
#include <stddef.h>

/* Turns a byte stream, fed in arbitrary chunks, into entries pushed into a
 * state, one per line. Words are shell-style quoted (see shellwords_next); a
 * line is
 *
 *   [OPTION...] COMMAND [ARG...]
 *
 *   -n/--name NAME   display name; defaults to the command line
 *   -i/--icon ICON   display icon; defaults to the command's basename, and
 *                    '' means none
 *   -x/--exec        exec the command in place of lm10
 *   -X/--detach      run the command in a new session, detached from lm10
 *
 * Without -x or -X, picking the entry prints its command line, shell-quoted,
 * to stdout. If both are given, the last one wins.
 * Long options also accept --opt=VALUE, and "--" ends the options. Malformed
 * lines are skipped with a warning. */
struct entry_parser {
  const char* name;  // where the entries come from, for warnings
  struct state* state;
  StrBuf buf;     // heap-backed; bytes fed but not yet a complete entry
  StrList words;  // heap-backed scratch; the current entry's command words
};

void entry_parser_init(struct entry_parser* ep,
                       const char* name,
                       struct state* state);
void entry_parser_destroy(struct entry_parser* ep);

/* Appends data and pushes every entry it completes. Returns true if an entry
 * was pushed. */
bool entry_parser_feed(struct entry_parser* ep, Str data);

/* The input has ended: pushes whatever is left. Returns true if an entry
 * was pushed. */
bool entry_parser_finish(struct entry_parser* ep);

#endif  // !LM_ENTRY_PARSE_H
