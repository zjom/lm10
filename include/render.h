#ifndef LM_RENDER_H
#define LM_RENDER_H

#include "config.h"
#include "draw.h"
#include "state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cairo.h>
#include <wayland-client-protocol.h>

struct render;

/* One shm-backed wl_buffer plus a cairo surface over the same memory. */
struct frame {
  struct render* r;  // presents again once the compositor releases it
  struct wl_buffer* buffer;
  uint32_t* data;  // mmap'd, kept mapped for the frame's lifetime
  size_t size;
  int width, height;    // in buffer pixels (logical * scale)
  cairo_surface_t* cs;  // created once, reused for every draw
  bool busy;            // set on commit, cleared in wl_buffer.release
};

/* Presents the state on a wl_surface: owns the buffer pool and paces redraws
 * with frame callbacks. */
struct render {
  struct wl_surface* surface;
  struct wl_shm* shm;
  struct wl_callback* frame_cb;  // non-NULL means a frame callback is pending
  struct frame frames[2];
  int width, height;  // target size in surface-local (logical) pixels
  uint32_t scale120;  // buffer pixels per logical pixel, in 120ths
  /* Something besides the state changed since the last commit, or a commit
   * is due anyway (a configure must be answered with one). */
  bool dirty;
  struct state* state;  // sorted before it is drawn
  uint64_t drawn_gen;   // state->gen when it was last drawn
  struct draw_ctx dctx;
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

/* Presents now if the state changed since it was last drawn (or r->dirty
 * is set) and a buffer and the frame slot are free; otherwise a pending
 * frame or release callback presents later. */
void render_request(struct render* r);

#endif  // !LM_RENDER_H
