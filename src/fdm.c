#include "fdm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "log.h"

struct fdm_watch {
  int fd;  // -1 once removed
  uint32_t flags;
  fdm_cb cb;
  void* data;
};

/* The live watch for fd, or NULL. */
static struct fdm_watch* find(struct fdm* fdm, int fd) {
  ARR_FOREACH(struct fdm_watch*, it, &fdm->watches) {
    if ((*it)->fd == fd)
      return *it;
  }
  return NULL;
}

/* Frees the watches removed since the last call. */
static void sweep(struct fdm* fdm) {
  for (size_t i = 0; i < fdm->watches.len;) {
    struct fdm_watch* w = fdm->watches.ptr[i];
    if (w->fd != -1) {
      i++;
      continue;
    }
    free(w);
    LIST_SWAP_REMOVE(&fdm->watches, i);
  }
}

int fdm_init(struct fdm* fdm) {
  *fdm = (struct fdm){.epfd = -1, .watches = LIST_INIT};
  fdm->epfd = epoll_create1(EPOLL_CLOEXEC);
  return fdm->epfd == -1 ? -1 : 0;
}

void fdm_destroy(struct fdm* fdm) {
  ARR_FOREACH(struct fdm_watch*, it, &fdm->watches) {
    if ((*it)->fd != -1 && ((*it)->flags & FDM_AUTOCLOSE))
      close((*it)->fd);
    free(*it);
  }
  LIST_FREE(&fdm->watches);
  if (fdm->epfd != -1)
    close(fdm->epfd);
  *fdm = (struct fdm){.epfd = -1};
}

int fdm_add(struct fdm* fdm,
            int fd,
            uint32_t events,
            fdm_cb cb,
            void* data,
            uint32_t flags) {
  struct fdm_watch* w = find(fdm, fd);
  if (w != NULL) {
    struct epoll_event ev = {.events = events, .data.ptr = w};
    if (epoll_ctl(fdm->epfd, EPOLL_CTL_MOD, fd, &ev) == -1)
      return -1;
    *w = (struct fdm_watch){.fd = fd, .flags = flags, .cb = cb, .data = data};
    return 0;
  }

  w = malloc(sizeof *w);
  if (w == NULL)
    return -1;
  *w = (struct fdm_watch){.fd = fd, .flags = flags, .cb = cb, .data = data};
  if (!LIST_PUSH(&fdm->watches, w)) {
    free(w);
    errno = ENOMEM;
    return -1;
  }
  struct epoll_event ev = {.events = events, .data.ptr = w};
  if (epoll_ctl(fdm->epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    int err = errno;
    LIST_POP(&fdm->watches);
    free(w);
    errno = err;
    return -1;
  }
  return 0;
}

void fdm_remove(struct fdm* fdm, int fd) {
  struct fdm_watch* w = find(fdm, fd);
  if (w == NULL)
    return;

  epoll_ctl(fdm->epfd, EPOLL_CTL_DEL, fd, NULL);
  if (w->flags & FDM_AUTOCLOSE)
    close(fd);
  /* Events for it may still be pending in the batch being dispatched. */
  w->fd = -1;
  if (!fdm->dispatching)
    sweep(fdm);
}

enum lm_err fdm_wait(struct fdm* fdm) {
  struct epoll_event events[64];
  int n = epoll_wait(fdm->epfd, events, sizeof events / sizeof events[0], -1);
  if (n == -1) {
    if (errno == EINTR)
      return LM_OK;
    LOG_ERROR("epoll_wait failed: %s", strerror(errno));
    return LM_ERR_POLL;
  }

  enum lm_err err = LM_OK;
  fdm->dispatching = true;
  for (int i = 0; i < n && err == LM_OK; i++) {
    struct fdm_watch* w = events[i].data.ptr;
    if (w->fd != -1)
      err = w->cb(fdm, w->data);
  }
  fdm->dispatching = false;
  sweep(fdm);
  return err;
}
