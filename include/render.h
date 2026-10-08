#ifndef LM_RENDER_H
#define LM_RENDER_H

#include "common.h"
#include "draw.h"
#include "frame.h"
#include "state.h"

#include <stdbool.h>
#include <wayland-client-protocol.h>

/* Presents the state on a wl_surface: owns the buffer pool and paces redraws
 * with frame callbacks. */
struct render {
  struct wl_surface* surface;
  struct wl_shm* shm;
  struct wl_callback* frame_cb;  // non-NULL means a frame callback is pending
  struct frame frames[2];
  int width, height;  // target size in buffer pixels
  bool dirty;         // something changed since the last commit
  const struct state* state;
  struct draw_ctx dctx;
  enum lm_err err;  // sticky; checked by the main loop
};

int render_init(struct render* r,
                struct wl_shm* shm,
                struct wl_surface* surface,
                const struct state* state);
void render_destroy(struct render* r);

/* Sets the target size; takes effect on the next present. */
void render_resize(struct render* r, int w, int h);

/* Marks the state dirty and presents now if a buffer and the frame slot are
 * free; otherwise a pending frame/release callback presents later.
 * Returns 0, or -1 with r->err set. */
int render_request(struct render* r);

#endif  // !LM_RENDER_H
