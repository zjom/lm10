#ifndef LM_DRAW_H
#define LM_DRAW_H

#include "config.h"
#include "state.h"

#include <cairo.h>
#include <pango/pango-font.h>

#include <stdbool.h>
#include <stddef.h>

/* Cached drawing resources and view state */
struct draw_ctx {
  const struct config_ui* ui;  // not owned, must outlive the ctx
  PangoFontDescription* font;
  int line_h;      // text height in the configured font
  size_t first;    // first visible match, kept so the selection stays in view
  size_t visible;  // rows that fit in the list when it was last drawn
  SpanList spans;  // scratch: a row's highlighted parts
};

int draw_ctx_init(struct draw_ctx* dc, const struct config_ui* ui);
void draw_ctx_destroy(struct draw_ctx* dc);

/* The surface height that fits the query row, the state's header and
 * ui->lines rows. */
int draw_height(const struct draw_ctx* dc, const struct state* state);

/* The match shown in the row-th visible row (from 0) as the state would be
 * drawn now, or false if that row is empty. */
bool draw_row_match(const struct draw_ctx* dc,
                    const struct state* state,
                    size_t row,
                    size_t* match);

void draw(const struct state* state,
          struct draw_ctx* dc,
          cairo_t* cr,
          int w,
          int h);

#endif  // !LM_DRAW_H
