#ifndef LM_INPUT_H
#define LM_INPUT_H

#include "fdm.h"
#include "state.h"
#include "ztd_arr.h"
#include "ztd_str.h"

#include <stdbool.h>

/* Reads entries from an fd, one per line, and pushes them into the state as
 * they arrive. Words are shell-style quoted; a line is
 *
 *   [OPTION...] COMMAND [ARG...]
 *
 *   -n/--name NAME   display name; defaults to the command line
 *   -i/--icon ICON   display icon
 *   -D/--detached    run in a new session, detached from the terminal
 *
 * Long options also accept --opt=VALUE, and "--" ends the options. Malformed
 * lines are skipped with a warning. */
struct input {
  struct fdm* fdm;
  int fd;         // -1 once EOF or an error has been reached
  StrBuf buf;     // heap-backed; bytes read but not yet a complete line
  StrList words;  // heap-backed scratch; the current line's command words
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
void input_destroy(struct input* in);

#endif  // !LM_INPUT_H
