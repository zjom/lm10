#ifndef LM_RENDER_H
#define LM_RENDER_H

#include "common.h"
#include "config.h"
#include "draw.h"
#include "frame.h"
#include "state.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client-protocol.h>

/* Presents the state on a wl_surface: owns the buffer pool and paces redraws
 * with frame callbacks. */
struct render {
  struct wl_surface* surface;
  struct wl_shm* shm;
  struct wl_callback* frame_cb;  // non-NULL means a frame callback is pending
  struct frame frames[2];
  int width, height;  // target size in surface-local (logical) pixels
  uint32_t scale120;  // buffer pixels per logical pixel, in 120ths
  bool dirty;  // something besides the state changed since the last commit
  struct state* state;  // sorted before it is drawn
  uint64_t drawn_gen;  // state->gen when it was last drawn
  struct draw_ctx dctx;
  enum lm_err err;  // sticky; checked by the main loop
};

/* ui must outlive the render. */
int render_init(struct render* r,
                struct wl_shm* shm,
                struct wl_surface* surface,
                struct state* state,
                const struct config_ui* ui);
void render_destroy(struct render* r);

/* Sets the target logical size; takes effect on the next present. */
void render_resize(struct render* r, int w, int h);

/* Sets the scale in 120ths (120 = 1x); takes effect on the next present.
 * Non-integer scales need a wp_viewport on the surface whose destination
 * tracks the logical size. */
void render_set_scale(struct render* r, uint32_t scale120);

/* Forces the next present to commit, even if nothing changed. */
void render_invalidate(struct render* r);

/* Presents now if the state changed since it was last drawn (or the
 * render was invalidated, resized or rescaled) and a buffer and the frame
 * slot are free; otherwise a pending frame/release callback presents
 * later. Returns 0, or -1 with r->err set. */
int render_request(struct render* r);

#endif  // !LM_RENDER_H
