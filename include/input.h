#ifndef LM_INPUT_H
#define LM_INPUT_H

#include "entry_parse.h"
#include "fdm.h"
#include "state.h"

#include <sys/types.h>

/* Reads entries from an fd, or from a command's output, and pushes them into
 * the state as they arrive; see entry_parser for the formats. */
struct input {
  struct fdm* fdm;
  int fd;     // -1 once EOF or an error has been reached
  pid_t pid;  // the command writing to fd, which is ours; or -1
  struct entry_parser parser;
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
