#include "fdm.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "test.h"

/* Two pipes, both readable when the test waits. Whichever callback runs
 * first removes the other pipe and registers a fresh, empty one, which gets
 * the other's fd number. The other's event is still in the batch; it must
 * not reach the new registration. */
struct reuse {
  int a[2], b[2];
  int fresh[2];
  int first_calls, fresh_calls;
};

struct side {
  struct reuse* r;
  int self, other;
};

static void on_fresh(struct fdm* fdm, void* data) {
  (void)fdm;
  struct reuse* r = data;
  r->fresh_calls++;
}

static void on_first(struct fdm* fdm, void* data) {
  struct side* s = data;
  struct reuse* r = s->r;
  if (r->first_calls++ > 0)
    return;
  fdm_remove(fdm, s->other);
  close(s->other);
  CHECK(pipe(r->fresh) == 0);
  CHECK(r->fresh[0] == s->other);  // the number is reused
  CHECK(fdm_add(fdm, r->fresh[0], EPOLLIN, on_fresh, r, 0) == 0);
}

static void test_reused_fd(void) {
  struct fdm fdm;
  CHECK(fdm_init(&fdm) == 0);
  struct reuse r = {0};
  CHECK(pipe(r.a) == 0 && pipe(r.b) == 0);
  CHECK(write(r.a[1], "x", 1) == 1 && write(r.b[1], "x", 1) == 1);
  struct side sa = {&r, r.a[0], r.b[0]}, sb = {&r, r.b[0], r.a[0]};
  CHECK(fdm_add(&fdm, r.a[0], EPOLLIN, on_first, &sa, 0) == 0);
  CHECK(fdm_add(&fdm, r.b[0], EPOLLIN, on_first, &sb, 0) == 0);
  fdm_wait(&fdm);
  CHECK(r.first_calls == 1);
  CHECK(r.fresh_calls == 0);
  fdm_destroy(&fdm);
}

static void on_count(struct fdm* fdm, void* data) {
  (void)fdm;
  int* calls = data;
  (*calls)++;
}

static void on_close(struct fdm* fdm, void* data) {
  int* fd = data;
  fdm_remove(fdm, *fd);  // removes and closes itself
  *fd = -1;
}

static void test_replace_and_autoclose(void) {
  struct fdm fdm;
  CHECK(fdm_init(&fdm) == 0);
  int p[2];
  CHECK(pipe(p) == 0);
  CHECK(write(p[1], "x", 1) == 1);
  int calls = 0;
  CHECK(fdm_add(&fdm, p[0], EPOLLIN, on_count, &calls, 0) == 0);
  fdm_wait(&fdm);
  CHECK(calls == 1);

  /* Re-adding replaces the registration. */
  int fd = p[0];
  CHECK(fdm_add(&fdm, p[0], EPOLLIN, on_close, &fd, FDM_AUTOCLOSE) == 0);
  fdm_wait(&fdm);
  CHECK(fd == -1);
  CHECK(fcntl(p[0], F_GETFD) == -1);  // closed
  CHECK(fdm.watches.len == 0);

  CHECK(fdm_add(&fdm, 12345, EPOLLIN, on_count, &calls, 0) == -1);
  close(p[1]);
  fdm_destroy(&fdm);
}

int main(void) {
  RUN(test_reused_fd);
  RUN(test_replace_and_autoclose);
  return TEST_RESULT;
}
