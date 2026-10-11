#include "spawn.h"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test.h"

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

  pid_t pid = spawn_sh("exit 3", STDERR_FILENO, true);
  CHECK(pid > 0);
  procs_watch(&ps, pid);
  while (ps.list.len > 0)
    fdm_wait(&fdm);
  CHECK(waitpid(pid, NULL, WNOHANG) == -1);  // reaped

  procs_destroy(&ps);
  fdm_destroy(&fdm);
}

/* Waits in the loop until every child is gone. Returns the seconds it
 * took. */
static double wait_gone(struct fdm* fdm, struct procs* ps) {
  double start = now();
  while (ps->list.len > 0 && now() - start < 10)
    fdm_wait(fdm);
  return now() - start;
}

static void test_terminate(void) {
  struct fdm fdm;
  CHECK(fdm_init(&fdm) == 0);
  struct procs ps;
  procs_init(&ps, &fdm);

  /* An ordinary command, and a background child of it, go on SIGTERM. */
  pid_t pid = spawn_sh("sleep 30 & sleep 30", STDERR_FILENO, true);
  procs_watch(&ps, pid);
  usleep(100 * 1000);
  procs_terminate(&ps, pid);
  CHECK(wait_gone(&fdm, &ps) < 1);
  usleep(100 * 1000);
  CHECK(kill(-pid, 0) == -1);  // the whole group is gone

  procs_destroy(&ps);
  fdm_destroy(&fdm);
}

static void test_destroy_kills_and_reaps(void) {
  struct fdm fdm;
  CHECK(fdm_init(&fdm) == 0);
  struct procs ps;
  procs_init(&ps, &fdm);
  /* Even one that ignores SIGTERM. */
  pid_t pid = spawn_sh("trap '' TERM; sleep 30", STDERR_FILENO, true);
  procs_watch(&ps, pid);
  usleep(100 * 1000);
  double start = now();
  procs_destroy(&ps);
  CHECK(now() - start < 1);
  fdm_destroy(&fdm);
  CHECK(waitpid(pid, NULL, WNOHANG) == -1);  // already reaped
  CHECK(kill(-pid, 0) == -1);
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
  RUN(test_destroy_kills_and_reaps);
  RUN(test_detached);
  return TEST_RESULT;
}
