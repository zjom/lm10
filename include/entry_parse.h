#ifndef LM_ENTRY_PARSE_H
#define LM_ENTRY_PARSE_H

#include "state.h"
#include "ztd_arr.h"
#include "ztd_str.h"

#include <stdbool.h>
#include <stddef.h>

/* Which syntax the input is in, picked from its first non-blank byte. */
enum entry_format {
  ENTRY_FORMAT_UNKNOWN,  // nothing but blanks read yet
  ENTRY_FORMAT_LINES,
  ENTRY_FORMAT_JSON,
};

enum entry_json_comment {
  ENTRY_JSON_NO_COMMENT,
  ENTRY_JSON_LINE_COMMENT,
  ENTRY_JSON_BLOCK_COMMENT,
};

/* Where the JSON scanner is in the buffer, between feeds. */
struct entry_json {
  size_t pos;         // next byte to scan
  size_t start;       // where the value being read starts, if depth > 0
  size_t line;        // input line of pos, from 1
  size_t start_line;  // input line of start
  int depth;          // brackets open within the current value
  bool in_array;      // inside a top-level array, whose elements are values
  bool in_str;
  bool esc;    // the previous byte in a string was a backslash
  bool stray;  // bytes that can't start a value have been warned about
  enum entry_json_comment comment;
};

/* Turns a byte stream, fed in arbitrary chunks, into entries pushed into a
 * state. Input starting with '{', '[' or a comment is JSON (see below);
 * anything else is one entry per line. Words are shell-style quoted (see
 * shellwords_next); a line is
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
 * lines are skipped with a warning.
 *
 * JSON input is a stream of values (JSONL, a single array, or both), with
 * comments and trailing commas allowed. Each value is an entry object or an
 * array of them:
 *
 *   {"name": "ranger", "icon": "", "run": "exec",
 *    "command": ["kitty", "ranger"]}
 *
 *   command  an array of argv strings, or a string split like a line
 *   name     as --name
 *   icon     as --icon; null means none
 *   run      "print" (the default), "exec" (-x) or "detach" (-X)
 *
 * Unknown keys are ignored with a warning; malformed values are skipped with
 * one, each element of a top-level array on its own. */
struct entry_parser {
  const char* name;  // where the entries come from, for warnings
  struct state* state;
  enum entry_format format;
  StrBuf buf;     // heap-backed; bytes fed but not yet a complete entry
  StrList words;  // heap-backed scratch; the current entry's command words
  struct entry_json json;
};

void entry_parser_init(struct entry_parser* ep,
                       const char* name,
                       struct state* state);
void entry_parser_destroy(struct entry_parser* ep);

/* Appends data and pushes every entry it completes; *pushed is set if any
 * was. Returns false on allocation failure, with data dropped. */
bool entry_parser_feed(struct entry_parser* ep, Str data, bool* pushed);

/* The input has ended: pushes whatever is left. Returns true if an entry
 * was pushed. */
bool entry_parser_finish(struct entry_parser* ep);

#endif  // !LM_ENTRY_PARSE_H
