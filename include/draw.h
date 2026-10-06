#ifndef LM10_DRAW_H
#define LM10_DRAW_H
#include "common.h"
#include "pango/pango-font.h"
#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app;

struct frame {
  struct wl_buffer *buffer;
  uint32_t *data; // mmap'd, kept mapped for the frame's lifetime
  size_t size;
  int width, height; // in buffer pixels (logical * scale)
  int stride;
  cairo_surface_t *cs; // optional: create once, reuse
  bool busy;           // set on commit, cleared in wl_buffer.release
  uint64_t rendered_gen;
};

void frame_init(struct frame *frame);
void frame_destroy(struct frame *frame);
struct frame *acquire_frame(struct app *app);

struct view {
  struct strlist *items;
  size_t selected;
  uint64_t gen; // bump on every change
};

struct render_ctx {
  PangoFontDescription *font; // parse "Sans Bold 28" once, not per frame
  // optional: cached measurements, recomputed when items change
  int max_w, line_h;
  uint64_t measured_gen;
};
void render(const struct view *v, struct render_ctx *rc, cairo_t *cr, int w,
            int h);

#endif // !LM10_DRAW_H
