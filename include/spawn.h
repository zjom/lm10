#ifndef LM_SPAWN_H
#define LM_SPAWN_H

#include "fdm.h"
#include "ztd_arr.h"

#include <stdbool.h>
#include <sys/types.h>

/* Runs sh -c cmd with stdin on /dev/null and stdout on out_fd. With
 * new_group it leads a process group of its own, so it can be terminated
 * along with whatever it starts. Returns its pid, or -1 (logged). */
pid_t spawn_sh(const char* cmd, int out_fd, bool new_group);

/* Runs argv (NULL-terminated) in a new session, detached from lm10, with
 * stdin on /dev/null and the activation token, if any, in its environment.
 * Returns once it has exec'd: true, or false (logged) if it couldn't. */
bool spawn_detached(char* const argv[], const char* token);

/* Replaces lm10 with argv, which keeps its terminal, session and stdout.
 * Its stdin is the terminal, since lm10's carried the entries. Only returns
 * on failure (logged). */
void spawn_exec(char* const argv[], const char* token);

struct proc;
typedef LIST(struct proc*) ProcList;

/* Children that lead process groups of their own (spawn_sh with
 * new_group), each reaped when its pidfd fires in the event loop. */
struct procs {
  struct fdm* fdm;
  ProcList list;
};

void procs_init(struct procs* ps, struct fdm* fdm);
/* Kills and reaps every child still running, with its group. Must run
 * before the fdm is destroyed. */
void procs_destroy(struct procs* ps);

/* Reaps pid in the event loop once it has exited. Dies if it can't be
 * watched. */
void procs_watch(struct procs* ps, pid_t pid);

/* Sends SIGTERM to pid's process group; procs reaps it once it exits.
 * Does nothing if pid has already been reaped. */
void procs_terminate(struct procs* ps, pid_t pid);

#endif  // !LM_SPAWN_H
