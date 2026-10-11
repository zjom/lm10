#ifndef LM_INPUT_H
#define LM_INPUT_H

#include "fdm.h"
#include "state.h"
#include "ztd_arr.h"
#include "ztd_str.h"

#include <stdbool.h>
#include <sys/types.h>

/* Which syntax the input is in, picked from its first non-blank byte. */
enum input_format {
  INPUT_FORMAT_UNKNOWN,  // nothing but blanks read yet
  INPUT_FORMAT_LINES,
  INPUT_FORMAT_JSON,
};

enum input_json_comment {
  INPUT_JSON_NO_COMMENT,
  INPUT_JSON_LINE_COMMENT,
  INPUT_JSON_BLOCK_COMMENT,
};

/* Where the JSON scanner is in the buffer, between reads. */
struct input_json {
  size_t pos;         // next byte to scan
  size_t start;       // where the value being read starts, if depth > 0
  size_t line;        // input line of pos, from 1
  size_t start_line;  // input line of start
  int depth;          // brackets open within the current value
  bool in_array;      // inside a top-level array, whose elements are values
  bool in_str;
  bool esc;    // the previous byte in a string was a backslash
  bool stray;  // bytes that can't start a value have been warned about
  enum input_json_comment comment;
};

/* Reads entries from an fd and pushes them into the state as they arrive.
 * Input starting with '{', '[' or a comment is JSON (see below); anything
 * else is one entry per line. Words are shell-style quoted; a line is
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
struct input {
  const char* name;  // where the entries come from, for warnings
  struct fdm* fdm;
  int fd;     // -1 once EOF or an error has been reached
  pid_t pid;  // the command writing to fd, which is ours; or -1
  enum input_format format;
  StrBuf buf;     // heap-backed; bytes read but not yet a complete entry
  StrList words;  // heap-backed scratch; the current entry's command words
  struct input_json json;
  struct state* state;
  void (*on_change)(void* data);  // called after entries were pushed
  void* data;
};

/* Starts watching fd. If fd can't be polled (e.g. a regular file), it is read
 * to EOF right away instead. Returns 0, or -1 on failure. */
int input_init(struct input* in,
               struct fdm* fdm,
               int fd,
               struct state* state,
               void (*on_change)(void* data),
               void* data);
/* Like input_init, reading the output of sh -c cmd. Its stdin is
 * /dev/null. */
int input_init_command(struct input* in,
                       struct fdm* fdm,
                       const char* cmd,
                       struct state* state,
                       void (*on_change)(void* data),
                       void* data);

/* Stops reading. A command that is still running is killed. */
void input_destroy(struct input* in);

#endif  // !LM_INPUT_H
