#include "spawn.h"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test.h"

struct exit_info {
  bool done;
  int status;
};

static void on_exit_cb(void* data, int status) {
  struct exit_info* e = data;
  e->done = true;
  e->status = status;
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_watch(void) {
  struct fdm fdm;
  CHECK(fdm_init(&fdm) == 0);
  struct procs ps;
  procs_init(&ps, &fdm);

  pid_t pid = spawn_sh("exit 3", STDERR_FILENO, false);
  CHECK(pid > 0);
  struct exit_info e = {0};
  CHECK(procs_watch(&ps, pid, false, on_exit_cb, &e));
  while (!e.done)
    CHECK(fdm_wait(&fdm) == LM_OK);
  CHECK(WIFEXITED(e.status) && WEXITSTATUS(e.status) == 3);
  CHECK(ps.list.len == 0);

  procs_destroy(&ps);
  fdm_destroy(&fdm);
}

/* Waits in the loop until pid is gone. Returns the seconds it took. */
static double wait_gone(struct fdm* fdm, struct procs* ps) {
  double start = now();
  while (ps->list.len > 0 && now() - start < 10)
    CHECK(fdm_wait(fdm) == LM_OK);
  return now() - start;
}

static void test_terminate(void) {
  struct fdm fdm;
  CHECK(fdm_init(&fdm) == 0);
  struct procs ps;
  procs_init(&ps, &fdm);

  /* An ordinary command, and a background child of it, go on SIGTERM. */
  pid_t pid = spawn_sh("sleep 30 & sleep 30", STDERR_FILENO, true);
  CHECK(procs_watch(&ps, pid, true, NULL, NULL));
  usleep(100 * 1000);
  double start = now();
  procs_terminate(&ps, pid);
  CHECK(now() - start < 0.1); /* doesn't wait */
  CHECK(wait_gone(&fdm, &ps) < 1);
  usleep(100 * 1000);
  CHECK(kill(-pid, 0) == -1); /* the whole group is gone */

  /* One that ignores SIGTERM is killed after the grace period. */
  pid = spawn_sh("trap '' TERM; sleep 30", STDERR_FILENO, true);
  CHECK(procs_watch(&ps, pid, true, NULL, NULL));
  usleep(100 * 1000);
  procs_terminate(&ps, pid);
  double took = wait_gone(&fdm, &ps);
  CHECK(took > 1.5 && took < 3);

  procs_destroy(&ps);
  fdm_destroy(&fdm);
}

static void test_destroy_terminates_owned(void) {
  struct fdm fdm;
  CHECK(fdm_init(&fdm) == 0);
  struct procs ps;
  procs_init(&ps, &fdm);
  pid_t owned = spawn_sh("sleep 30", STDERR_FILENO, true);
  pid_t other = spawn_sh("sleep 30", STDERR_FILENO, false);
  CHECK(procs_watch(&ps, owned, true, NULL, NULL));
  CHECK(procs_watch(&ps, other, false, NULL, NULL));
  usleep(100 * 1000);
  procs_destroy(&ps);
  fdm_destroy(&fdm);

  int status;
  CHECK(waitpid(owned, &status, 0) == owned && WIFSIGNALED(status));
  CHECK(waitpid(other, &status, WNOHANG) == 0); /* left running */
  kill(other, SIGKILL);
  waitpid(other, &status, 0);
}

static void test_detached(void) {
  char* ok[] = {"true", NULL};
  char* missing[] = {"/nonexistent/lm10-test", NULL};
  CHECK(spawn_detached(ok, NULL));
  CHECK(!spawn_detached(missing, NULL));
}

int main(void) {
  RUN(test_watch);
  RUN(test_terminate);
  RUN(test_destroy_terminates_owned);
  RUN(test_detached);
  return TEST_RESULT;
}
