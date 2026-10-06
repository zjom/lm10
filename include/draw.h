#ifndef LM10_DRAW_H
#define LM10_DRAW_H

#include "view.h"

#include <cairo.h>
#include <pango/pango-font.h>

#include <stdint.h>

/* Cached drawing resources */
struct draw_ctx {
  PangoFontDescription *font;
  // cached measurements, recomputed when the view changes
  int max_w, line_h;
  uint64_t measured_gen;
};

int draw_ctx_init(struct draw_ctx *dc);
void draw_ctx_destroy(struct draw_ctx *dc);

void draw(const struct view *v, struct draw_ctx *dc, cairo_t *cr, int w, int h);

#endif // !LM10_DRAW_H
