#ifndef LM_DRAW_H
#define LM_DRAW_H

#include "config.h"
#include "state.h"

#include <cairo.h>
#include <pango/pango-font.h>

#include <stddef.h>

/* Cached drawing resources and view state */
struct draw_ctx {
  const struct config_ui* ui;  // not owned, must outlive the ctx
  PangoFontDescription* font;
  int line_h;  // text height in the configured font
  size_t first;    // first visible match, kept so the selection stays in view
  SpanList spans;  // scratch: a row's highlighted parts
};

int draw_ctx_init(struct draw_ctx* dc, const struct config_ui* ui);
void draw_ctx_destroy(struct draw_ctx* dc);

/* The surface height that fits the query row, the state's header and
 * ui->lines rows. */
int draw_height(const struct draw_ctx* dc, const struct state* state);

void draw(const struct state* state,
          struct draw_ctx* dc,
          cairo_t* cr,
          int w,
          int h);

#endif  // !LM_DRAW_H
