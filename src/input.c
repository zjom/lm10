#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

/* Stops reading; at_eof says the command, if any, is done writing, so it is
 * waited for rather than killed. */
static void stop(struct input* in, bool at_eof) {
  if (in->fd == -1)
    return;
  fdm_remove(in->fdm, in->fd);
  if (in->pid > 0) {
    close(in->fd);
    if (!at_eof)
      kill(-in->pid, SIGTERM); /* its process group: sh and its children */
    while (waitpid(in->pid, NULL, 0) == -1 && errno == EINTR)
      ;
    in->pid = -1;
  }
  in->fd = -1;
  entry_parser_destroy(&in->parser);
}

/* One read. Returns true while there may be more to read. */
static bool read_some(struct input* in, bool* pushed) {
  char chunk[4096];
  ssize_t n = read(in->fd, chunk, sizeof chunk);
  if (n == -1) {
    if (errno == EINTR || errno == EAGAIN)
      return true;
    LOG_WARN("%s: read failed: %s", in->parser.name, strerror(errno));
  } else if (n > 0) {
    if (entry_parser_feed(&in->parser, str_from_parts(chunk, (size_t)n),
                          pushed))
      return true;
    LOG_WARN("%s: allocation failed", in->parser.name);
  }
  *pushed |= entry_parser_finish(&in->parser);
  return false;
}

static enum lm_err on_readable(struct fdm* fdm, void* data) {
  (void)fdm;
  struct input* in = data;
  bool pushed = false;
  /* Level-triggered, so one read per wakeup: never blocks, and fd stays in
   * whatever blocking mode it was handed to us in. */
  if (!read_some(in, &pushed))
    stop(in, true);
  if (pushed && in->on_change)
    in->on_change(in->data);
  return LM_OK;
}

int input_init(struct input* in,
               struct fdm* fdm,
               int fd,
               struct state* state,
               void (*on_change)(void* data),
               void* data) {
  *in = (struct input){
      .fdm = fdm,
      .fd = fd,
      .pid = -1,
      .on_change = on_change,
      .data = data,
  };
  entry_parser_init(&in->parser, "stdin", state);

  if (fdm_add(fdm, fd, EPOLLIN, on_readable, in, 0) == 0)
    return 0;

  if (errno != EPERM) {
    LOG_ERROR("failed to watch stdin: %s", strerror(errno));
    in->fd = -1;
    entry_parser_destroy(&in->parser);
    return -1;
  }

  /* Regular files and the like can't be polled, but never block either. */
  bool pushed = false;
  while (read_some(in, &pushed))
    ;
  in->fd = -1;
  entry_parser_destroy(&in->parser);
  if (pushed && on_change)
    on_change(data);
  return 0;
}

int input_init_command(struct input* in,
                       struct fdm* fdm,
                       const char* cmd,
                       struct state* state,
                       void (*on_change)(void* data),
                       void* data) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) == -1) {
    LOG_ERROR("reload: failed to create pipe: %s", strerror(errno));
    return -1;
  }

  pid_t pid = fork();
  if (pid == -1) {
    LOG_ERROR("reload: fork failed: %s", strerror(errno));
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    /* Its own process group, so stop() can kill whatever it started. */
    setpgid(0, 0);
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull == -1 || dup2(devnull, STDIN_FILENO) == -1 ||
        dup2(fds[1], STDOUT_FILENO) == -1)
      _exit(127);
    execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
    _exit(127);
  }
  setpgid(pid, pid); /* as the child does, whichever runs first */
  close(fds[1]);
  /* Only our end: reads must never block the event loop. */
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

  *in = (struct input){
      .fdm = fdm,
      .fd = fds[0],
      .pid = pid,
      .on_change = on_change,
      .data = data,
  };
  entry_parser_init(&in->parser, "reload", state);
  if (fdm_add(fdm, fds[0], EPOLLIN, on_readable, in, 0) != 0) {
    LOG_ERROR("reload: failed to watch output: %s", strerror(errno));
    close(fds[0]);
    kill(-pid, SIGTERM);
    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
      ;
    in->fd = -1;
    in->pid = -1;
    entry_parser_destroy(&in->parser);
    return -1;
  }
  return 0;
}

void input_destroy(struct input* in) {
  stop(in, false);
}
