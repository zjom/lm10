#include "fdm.h"
#include "stb_ds.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

struct fdmdata {
  uint32_t key; // fd
  struct {
    void *data;
    uint32_t flags;
    int32_t (*cb)(struct fdm *, void *);
    struct epoll_event epev;
  } value;
};

struct fdm {
  uint32_t epfd;
  struct fdmdata *hm;
};

int fdm_init(struct fdm *fdm) {
  int epfd = epoll_create1(0);
  if (epfd == -1) {
    return -1;
  }
  fdm->epfd = epfd;
  fdm->hm = NULL;
  return 0;
}

void fdm_destroy(struct fdm *fdm) {
  close(fdm->epfd);
  for (int i = 0; i < hmlen(fdm->hm); ++i) {
    epoll_ctl(fdm->epfd, EPOLL_CTL_DEL, fdm->hm[i].key, NULL);
    if (fdm->hm[i].value.flags & FDM_AUTOCLOSE) {
      close(fdm->hm[i].key);
    }
  }
  hmfree(fdm->hm);
}

int fdm_add(struct fdm *fdm, uint32_t fd, uint32_t events, void *data,
            int32_t (*cb)(struct fdm *, void *), uint32_t flags) {
  struct fdmdata fdmdata = {.key = fd,
                            .value = {.data = data,
                                      .cb = cb,
                                      .epev = {.events = events, .data.fd = fd},
                                      .flags = flags}};

  int op = EPOLL_CTL_ADD;
  if (hmgeti(fdm->hm, fd) != -1) {
    op = EPOLL_CTL_MOD;
  }

  if (epoll_ctl(fdm->epfd, op, fd, &fdmdata.value.epev) == -1) {
    return -1;
  }

  hmputs(fdm->hm, fdmdata);
  return 0;
}

void fdm_remove(struct fdm *fdm, uint32_t fd) {
  struct fdmdata *item = hmgetp_null(fdm->hm, fd);
  if (item == NULL) {
    return;
  }

  epoll_ctl(fdm->epfd, EPOLL_CTL_DEL, fd, NULL);

  if (item->value.flags & FDM_AUTOCLOSE) {
    close(fd);
  }
}

int fdm_wait(struct fdm *fdm) {
  uint32_t MAX_EVENTS = 1024;
  struct epoll_event epoll_events[MAX_EVENTS];
  int32_t event_count;
  int32_t TIMEOUT = 5000;

  event_count = epoll_wait(fdm->epfd, epoll_events, MAX_EVENTS, TIMEOUT);
  if (event_count == -1) {
    return -1;
  }

  for (int i = 0; i < event_count; ++i) {
    struct fdmdata *item = hmgetp_null(fdm->hm, epoll_events[i].data.fd);
    if (item == NULL) {
      fprintf(stderr, "warning: unknown fd received from epoll\n");
      continue;
    }

    if (item->value.cb(fdm, item->value.data) == -1) {
      return -1;
    }
  }
  return 0;
}
