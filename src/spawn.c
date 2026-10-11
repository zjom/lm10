#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

/* Points fd at /dev/null. Returns false on failure. */
static bool to_devnull(int fd, int flags) {
  int null = open("/dev/null", flags | O_CLOEXEC);
  if (null == -1)
    return false;
  bool ok = dup2(null, fd) != -1;
  close(null);
  return ok;
}

static void set_activation_env(const char* token) {
  if (token == NULL)
    return;
  /* For Wayland clients, and X11 startup notification. */
  setenv("XDG_ACTIVATION_TOKEN", token, 1);
  setenv("DESKTOP_STARTUP_ID", token, 1);
}

pid_t spawn_sh(const char* cmd, int out_fd, bool new_group) {
  pid_t pid = fork();
  if (pid == -1) {
    LOG_ERROR("fork failed: %s", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    if (new_group)
      setpgid(0, 0);
    if (!to_devnull(STDIN_FILENO, O_RDONLY) ||
        dup2(out_fd, STDOUT_FILENO) == -1)
      _exit(127);
    execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
    _exit(127);
  }
  if (new_group)
    setpgid(pid, pid);  // as the child does, whichever runs first
  return pid;
}

bool spawn_detached(char* const argv[], const char* token) {
  /* The grandchild reports a failed exec through this pipe; a successful
   * one closes it (O_CLOEXEC), so the parent reads EOF. */
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) == -1) {
    LOG_ERROR("failed to create pipe: %s", strerror(errno));
    return false;
  }

  pid_t pid = fork();
  if (pid == -1) {
    LOG_ERROR("fork failed: %s", strerror(errno));
    close(fds[0]);
    close(fds[1]);
    return false;
  }

  if (pid == 0) {
    /* Intermediate child: exits right away, so the grandchild is
     * reparented to init and never becomes our zombie. */
    close(fds[0]);
    if (setsid() == -1)
      goto child_err;
    set_activation_env(token);
    pid_t gpid = fork();
    if (gpid == -1)
      goto child_err;
    if (gpid > 0)
      _exit(0);
    if (!to_devnull(STDIN_FILENO, O_RDONLY))
      goto child_err;
    execvp(argv[0], argv);

  child_err: {
    int err = errno;
    (void)!write(fds[1], &err, sizeof err);
    _exit(1);
  }
  }

  close(fds[1]);
  int child_errno;
  ssize_t n;
  do {
    n = read(fds[0], &child_errno, sizeof child_errno);
  } while (n == -1 && errno == EINTR);
  int read_errno = errno;
  close(fds[0]);
  while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
    ;

  if (n == 0)
    return true;
  if (n == -1)
    LOG_ERROR("failed to read from pipe: %s", strerror(read_errno));
  else if (n == sizeof child_errno)
    LOG_ERROR("failed to run %s: %s", argv[0], strerror(child_errno));
  else
    LOG_ERROR("short read from pipe");
  return false;
}

void spawn_exec(char* const argv[], const char* token) {
  set_activation_env(token);
  if (!isatty(STDIN_FILENO)) {
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd != -1) {
      dup2(fd, STDIN_FILENO);
      close(fd);
    } else {
      to_devnull(STDIN_FILENO, O_RDONLY);
    }
  }
  execvp(argv[0], argv);
  LOG_ERROR("failed to run %s: %s", argv[0], strerror(errno));
}

/* ---- procs ---- */

struct proc {
  struct procs* ps;
  pid_t pid;
  int pidfd;
};

static struct proc* find(struct procs* ps, pid_t pid) {
  ARR_FOREACH(struct proc*, it, &ps->list) {
    if ((*it)->pid == pid)
      return *it;
  }
  return NULL;
}

/* Stops tracking p and frees it. */
static void drop(struct proc* p) {
  struct procs* ps = p->ps;
  fdm_remove(ps->fdm, p->pidfd);
  for (size_t i = 0; i < ps->list.len; i++) {
    if (ps->list.ptr[i] == p) {
      LIST_SWAP_REMOVE(&ps->list, i);
      break;
    }
  }
  free(p);
}

static void reap(pid_t pid, int options) {
  while (waitpid(pid, NULL, options) == -1 && errno == EINTR)
    ;
}

static void on_pidfd(struct fdm* fdm, void* data) {
  (void)fdm;
  struct proc* p = data;
  reap(p->pid, 0);  // it has exited, so this doesn't block
  drop(p);
}

void procs_init(struct procs* ps, struct fdm* fdm) {
  *ps = (struct procs){.fdm = fdm, .list = LIST_INIT};
}

void procs_destroy(struct procs* ps) {
  while (ps->list.len > 0) {
    struct proc* p = ps->list.ptr[ps->list.len - 1];
    /* Killed rather than terminated, so reaping it can't hang; whatever
     * lm10 execs next mustn't inherit it. */
    kill(-p->pid, SIGKILL);
    reap(p->pid, 0);
    drop(p);
  }
  LIST_FREE(&ps->list);
}

void procs_watch(struct procs* ps, pid_t pid) {
  int pidfd = pidfd_open(pid, 0);
  if (pidfd == -1)
    die("pidfd_open: %s", strerror(errno));
  struct proc* p = xmalloc(sizeof *p);
  *p = (struct proc){.ps = ps, .pid = pid, .pidfd = pidfd};
  if (fdm_add(ps->fdm, pidfd, EPOLLIN, on_pidfd, p, FDM_AUTOCLOSE) != 0)
    die("watching process %d: %s", (int)pid, strerror(errno));
  LIST_PUSH(&ps->list, p);
}

void procs_terminate(struct procs* ps, pid_t pid) {
  if (find(ps, pid) != NULL)  // else reaped: the pid may be someone else's
    kill(-pid, SIGTERM);
}
