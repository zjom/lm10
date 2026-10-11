#ifndef LM_DRAW_H
#define LM_DRAW_H

#include "config.h"
#include "icon.h"
#include "state.h"

#include <cairo.h>
#include <pango/pango-font.h>

#include <stddef.h>

/* Cached drawing resources and view state */
struct draw_ctx {
  const struct config_ui* ui;  // not owned, must outlive the ctx
  PangoFontDescription* font;
  int line_h;  // text height in the configured font; also the icon size
  struct icon_cache* icons;  // NULL if icons are off
  bool icon_column;          // rows leave room for icons; set once one loads
  size_t first;  // first visible match, kept so the selection stays in view
};

int draw_ctx_init(struct draw_ctx* dc, const struct config_ui* ui);
void draw_ctx_destroy(struct draw_ctx* dc);

/* The surface height that fits the query row and ui->lines rows. */
int draw_height(const struct draw_ctx* dc);

void draw(const struct state* v,
          struct draw_ctx* dc,
          cairo_t* cr,
          int w,
          int h);

#endif  // !LM_DRAW_H
