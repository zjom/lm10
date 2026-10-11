#ifndef LM_FDM_H
#define LM_FDM_H

#include "common.h"
#include "ztd_arr.h"

#include <stdbool.h>
#include <stdint.h>

/* An epoll loop calling back on ready fds. Callbacks may add and remove fds,
 * including their own: a removed fd gets no further callbacks, even for
 * events already collected in the current fdm_wait, and a new registration
 * that reuses its number isn't mistaken for it. */
struct fdm;

/* Called when fd is ready. Returns LM_OK, or an error that makes fdm_wait
 * stop and return it. */
typedef enum lm_err (*fdm_cb)(struct fdm* fdm, void* data);

struct fdm_watch;
typedef LIST(struct fdm_watch*) FdmWatchList;

struct fdm {
  int epfd;
  FdmWatchList watches;  // heap-allocated, so epoll can point at them
  bool dispatching;      // removed watches are freed after the batch
};

enum fdm_flag {
  FDM_AUTOCLOSE = 1 << 0,  // fdm_remove and fdm_destroy close the fd
};

/* Returns 0, or -1 with errno set. */
int fdm_init(struct fdm* fdm);
void fdm_destroy(struct fdm* fdm);

/* Watches fd for events (EPOLLIN etc.), level-triggered; flags is a mask
 * of fdm_flag. Adding an fd that is already watched replaces its
 * registration. Returns 0, or -1 with errno set. */
int fdm_add(struct fdm* fdm,
            int fd,
            uint32_t events,
            fdm_cb cb,
            void* data,
            uint32_t flags);

/* Stops watching fd, closing it if it was added with FDM_AUTOCLOSE. Does
 * nothing if fd isn't watched. */
void fdm_remove(struct fdm* fdm, int fd);

/* Waits for events and runs their callbacks. Returns LM_OK (also when
 * interrupted by a signal), LM_ERR_POLL if waiting failed, or the first
 * error a callback returned. */
enum lm_err fdm_wait(struct fdm* fdm);

#endif  // !LM_FDM_H
