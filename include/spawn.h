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

/* Called once a child has exited and been reaped, with its wait status. */
typedef void (*proc_exit_fn)(void* data, int status);

struct proc;
typedef LIST(struct proc*) ProcList;

/* Children being waited for without blocking: each is reaped when its pidfd
 * fires in the event loop. */
struct procs {
  struct fdm* fdm;
  ProcList list;
};

void procs_init(struct procs* ps, struct fdm* fdm);
/* Stops waiting for every child, terminating the owned ones. Must run
 * before the fdm is destroyed. */
void procs_destroy(struct procs* ps);

/* Waits for pid in the event loop and calls on_exit (may be NULL) once it
 * has exited. An owned child leads its own process group, which lm10 may
 * terminate (procs_terminate, procs_destroy). If the kernel can't watch
 * pids (before 5.3), on_exit runs right away after a blocking wait, or
 * without on_exit the child is left until lm10 exits. Returns false on
 * failure. */
bool procs_watch(struct procs* ps,
                 pid_t pid,
                 bool owned,
                 proc_exit_fn on_exit,
                 void* data);

/* Sends SIGTERM to an owned child's process group, and SIGKILL if it is
 * still running two seconds later. Its on_exit isn't called. Does nothing
 * if pid isn't watched (it has been reaped, or couldn't be watched). */
void procs_terminate(struct procs* ps, pid_t pid);

#endif  // !LM_SPAWN_H
