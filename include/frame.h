#ifndef LM_FRAME_H
#define LM_FRAME_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cairo.h>
#include <wayland-client-protocol.h>

struct frame;
typedef void (*frame_release_fn)(void *data, struct frame *frame);

/* One shm-backed wl_buffer plus a cairo surface over the same memory. */
struct frame {
  struct wl_buffer *buffer;
  uint32_t *data; // mmap'd, kept mapped for the frame's lifetime
  size_t size;
  int width, height; // in buffer pixels (logical * scale)
  int stride;
  cairo_surface_t *cs; // created once, reused for every draw
  bool busy;           // set on commit, cleared in wl_buffer.release

  frame_release_fn on_release; // called after busy is cleared
  void *release_data;
};

/* Returns LM_OK, or an error code with *f left zeroed. */
enum lm_err frame_init(struct frame *f, struct wl_shm *shm, int w, int h,
                       frame_release_fn on_release, void *data);
void frame_destroy(struct frame *f);

#endif // !LM_FRAME_H
