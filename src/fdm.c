#include "fdm.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

static struct fdmdata* fdm__find(struct fdm* fdm, uint32_t fd) {
  ARR_FOREACH(struct fdmdata, it, &fdm->fds) {
    if (it->key == fd) {
      return it;
    }
  }
  return NULL;
}

int fdm_init(struct fdm* fdm) {
  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd == -1) {
    return -1;
  }
  fdm->epfd = epfd;
  fdm->fds = (FdmDataList)LIST_INIT;
  return 0;
}

void fdm_destroy(struct fdm* fdm) {
  close(fdm->epfd);
  ARR_FOREACH(struct fdmdata, it, &fdm->fds) {
    epoll_ctl(fdm->epfd, EPOLL_CTL_DEL, it->key, NULL);
    if (it->value.flags & FDM_AUTOCLOSE) {
      close(it->key);
    }
  }
  LIST_FREE(&fdm->fds);
}

int fdm_add(struct fdm* fdm,
            uint32_t fd,
            uint32_t events,
            void* data,
            int32_t (*cb)(struct fdm*, void*),
            uint32_t flags) {
  struct fdmdata fdmdata = {.key = fd,
                            .value = {.data = data,
                                      .cb = cb,
                                      .epev = {.events = events, .data.fd = fd},
                                      .flags = flags}};

  struct fdmdata* item = fdm__find(fdm, fd);
  int op = item != NULL ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

  if (epoll_ctl(fdm->epfd, op, fd, &fdmdata.value.epev) == -1) {
    return -1;
  }

  if (item != NULL) {
    *item = fdmdata;
  } else if (!LIST_PUSH(&fdm->fds, fdmdata)) {
    epoll_ctl(fdm->epfd, EPOLL_CTL_DEL, fd, NULL);
    return -1;
  }
  return 0;
}

void fdm_remove(struct fdm* fdm, uint32_t fd) {
  struct fdmdata* item = fdm__find(fdm, fd);
  if (item == NULL) {
    return;
  }

  epoll_ctl(fdm->epfd, EPOLL_CTL_DEL, fd, NULL);

  if (item->value.flags & FDM_AUTOCLOSE) {
    close(fd);
  }
  LIST_SWAP_REMOVE(&fdm->fds, (size_t)(item - fdm->fds.ptr));
}

int fdm_wait(struct fdm* fdm) {
  uint32_t MAX_EVENTS = 1024;
  struct epoll_event epoll_events[MAX_EVENTS];
  int32_t event_count;
  int32_t TIMEOUT = 5000;

  event_count = epoll_wait(fdm->epfd, epoll_events, MAX_EVENTS, TIMEOUT);
  if (event_count == -1) {
    return -1;
  }

  for (int i = 0; i < event_count; ++i) {
    struct fdmdata* item = fdm__find(fdm, epoll_events[i].data.fd);
    if (item == NULL) {
      fprintf(stderr, "warning: unknown fd received from epoll\n");
      continue;
    }

    if (item->value.cb(fdm, item->value.data) != 0) {
      return -1;
    }
  }
  return 0;
}
