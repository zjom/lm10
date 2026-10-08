#include "draw.h"

#include <glib-object.h>
#include <pango/pangocairo.h>

int draw_ctx_init(struct draw_ctx* dc) {
  *dc = (struct draw_ctx){.measured_gen = UINT64_MAX};
  dc->font = pango_font_description_from_string("Sans Bold 28");
  return dc->font ? 0 : -1;
}

void draw_ctx_destroy(struct draw_ctx* dc) {
  if (dc->font)
    pango_font_description_free(dc->font);
  *dc = (struct draw_ctx){0};
}

static void measure(const struct state* v,
                    struct draw_ctx* dc,
                    PangoLayout* layout) {
  size_t n = v->entries.len;
  int max_w = 0, line_h = 0;
  for (size_t i = 0; i < n; i++) {
    int lw, lh;
    pango_layout_set_text(layout, v->entries.ptr[i].name.ptr,
                          (int)v->entries.ptr[i].name.len);
    pango_layout_get_pixel_size(layout, &lw, &lh);
    if (lw > max_w)
      max_w = lw;
    if (lh > line_h)
      line_h = lh;
  }
  dc->max_w = max_w;
  dc->line_h = line_h;
  dc->measured_gen = v->gen;
}

void draw(const struct state* v,
          struct draw_ctx* dc,
          cairo_t* cr,
          int w,
          int h) {
  /* Buffers are reused, so clear first (black, as a fresh memfd was). */
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_paint(cr);

  int step = 20;
  cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
  cairo_set_line_width(cr, 1.0);
  for (int x = 0; x <= w; x += step) {
    cairo_move_to(cr, x + 0.5, 0);
    cairo_line_to(cr, x + 0.5, h);
  }
  for (int y = 0; y <= h; y += step) {
    cairo_move_to(cr, 0, y + 0.5);
    cairo_line_to(cr, w, y + 0.5);
  }
  cairo_stroke(cr);

  PangoLayout* layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, dc->font);

  if (dc->measured_gen != v->gen)
    measure(v, dc, layout);

  const double pad_x = 16, pad_y = 8;  // panel padding
  const double row_pad = 6;            // vertical padding inside each row
  size_t n = v->entries.len;

  double row_h = dc->line_h + 2 * row_pad;
  double tw = dc->max_w, th = row_h * n;
  double tx = (w - tw) / 2.0, ty = (h - th) / 2.0;

  /* Panel background */
  cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
  cairo_rectangle(cr, tx - pad_x, ty - pad_y, tw + 2 * pad_x, th + 2 * pad_y);
  cairo_fill(cr);

  /* Rows */
  for (size_t i = 0; i < n; i++) {
    double ry = ty + i * row_h;

    if (i == v->selected) {
      cairo_set_source_rgb(cr, 0.20, 0.45, 0.90);  // highlight bg
      cairo_rectangle(cr, tx - pad_x, ry, tw + 2 * pad_x, row_h);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 1, 1, 1);  // selected text
    } else {
      cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);  // normal text
    }

    pango_layout_set_text(layout, v->entries.ptr[i].name.ptr,
                          (int)v->entries.ptr[i].name.len);
    cairo_move_to(cr, tx, ry + row_pad);
    pango_cairo_show_layout(cr, layout);
  }

  g_object_unref(layout);
}
