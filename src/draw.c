#include "draw.h"

#include <math.h>
#include <stdio.h>

#include <glib-object.h>
#include <pango/pangocairo.h>

/* Gap between the query text and the match counter. */
#define COUNTER_GAP 12
#define CURSOR_W 2
#define SCROLLBAR_W 3

int draw_ctx_init(struct draw_ctx* dc, const struct config_ui* ui) {
  *dc = (struct draw_ctx){.ui = ui};
  dc->font = pango_font_description_from_string(sb_cstr(&ui->font));
  if (dc->font == NULL)
    return -1;

  /* Row heights are fixed, so measure the font once up front; the surface
   * size depends on it before anything has been drawn. */
  PangoContext* ctx =
      pango_font_map_create_context(pango_cairo_font_map_get_default());
  PangoLayout* layout = pango_layout_new(ctx);
  pango_layout_set_font_description(layout, dc->font);
  pango_layout_set_text(layout, "Ag", -1);
  pango_layout_get_pixel_size(layout, NULL, &dc->line_h);
  g_object_unref(layout);
  g_object_unref(ctx);
  return 0;
}

void draw_ctx_destroy(struct draw_ctx* dc) {
  if (dc->font)
    pango_font_description_free(dc->font);
  *dc = (struct draw_ctx){0};
}

/* ---- geometry ---- */

/* Everything is laid out from the panel edges inwards:
 *
 *   inset | query row | gap | separator | gap | rows... | inset
 */
struct geom {
  double inset;  // border + padding
  double row_h;
  double query_y, sep_y, list_y;
};

static struct geom geometry(const struct draw_ctx* dc) {
  const struct config_ui* ui = dc->ui;
  struct geom g;
  g.inset = ui->border_width + ui->padding;
  g.row_h = dc->line_h + 2.0 * ui->row_padding;
  g.query_y = g.inset;
  g.sep_y = g.query_y + g.row_h + ui->padding / 2;
  g.list_y = g.query_y + g.row_h + ui->padding;
  return g;
}

int draw_height(const struct draw_ctx* dc) {
  struct geom g = geometry(dc);
  return (int)ceil(g.list_y + dc->ui->lines * g.row_h + g.inset);
}

/* ---- helpers ---- */

static void set_color(cairo_t* cr, struct config_color c) {
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

static void rounded_rect(cairo_t* cr,
                         double x,
                         double y,
                         double w,
                         double h,
                         double r) {
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  if (r <= 0) {
    cairo_rectangle(cr, x, y, w, h);
    return;
  }
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

/* Resets the layout to plain, unbounded text. */
static void set_text(PangoLayout* layout, Str text) {
  pango_layout_set_attributes(layout, NULL);
  pango_layout_set_width(layout, -1);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
  pango_layout_set_text(layout, text.ptr, (int)text.len);
}

static int text_width(PangoLayout* layout) {
  int w;
  pango_layout_get_pixel_size(layout, &w, NULL);
  return w;
}

/* Shows the layout at x, vertically centered in the row at y. */
static void show(cairo_t* cr,
                 PangoLayout* layout,
                 const struct geom* g,
                 double x,
                 double y) {
  int lh;
  pango_layout_get_pixel_size(layout, NULL, &lh);
  cairo_move_to(cr, x, y + (g->row_h - lh) / 2);
  pango_cairo_show_layout(cr, layout);
}

static PangoAttrList* highlight(size_t start,
                                size_t end,
                                struct config_color c) {
  PangoAttrList* attrs = pango_attr_list_new();
  PangoAttribute* a[] = {
      pango_attr_foreground_new((guint16)(c.r * 65535), (guint16)(c.g * 65535),
                                (guint16)(c.b * 65535)),
      pango_attr_foreground_alpha_new((guint16)(c.a * 65535)),
      pango_attr_weight_new(PANGO_WEIGHT_BOLD),
  };
  for (size_t i = 0; i < sizeof a / sizeof a[0]; i++) {
    a[i]->start_index = (guint)start;
    a[i]->end_index = (guint)end;
    pango_attr_list_insert(attrs, a[i]);
  }
  return attrs;
}

/* ---- parts ---- */

static void draw_panel(cairo_t* cr, const struct config_ui* ui, int w, int h) {
  double bw = ui->border_width;
  rounded_rect(cr, bw / 2, bw / 2, w - bw, h - bw, ui->border_radius - bw / 2);
  set_color(cr, ui->colors.background);
  if (bw > 0) {
    cairo_fill_preserve(cr);
    set_color(cr, ui->colors.border);
    cairo_set_line_width(cr, bw);
    cairo_stroke(cr);
  } else {
    cairo_fill(cr);
  }
}

static void draw_query(const struct state* v,
                       const struct draw_ctx* dc,
                       const struct geom* g,
                       cairo_t* cr,
                       PangoLayout* layout,
                       int w) {
  const struct config_ui* ui = dc->ui;
  double x = g->inset + ui->row_padding, y = g->query_y;
  double right = w - g->inset - ui->row_padding;

  set_text(layout, sb_str(&ui->prompt));
  set_color(cr, ui->colors.prompt);
  show(cr, layout, g, x, y);
  x += text_width(layout);

  if (ui->show_counter && v->entries.len > 0) {
    char buf[48];
    int n =
        snprintf(buf, sizeof buf, "%zu/%zu", v->matches.len, v->entries.len);
    set_text(layout, str_from_parts(buf, (size_t)n));
    right -= text_width(layout);
    set_color(cr, ui->colors.counter);
    show(cr, layout, g, right, y);
    right -= COUNTER_GAP;
  }

  /* The query gets whatever is left, scrolled to keep the cursor in it. */
  double avail = right - x - CURSOR_W;
  if (avail <= 0)
    return;
  cairo_save(cr);
  cairo_rectangle(cr, x, y, avail + CURSOR_W, g->row_h);
  cairo_clip(cr);

  double cur_x = 0;
  if (v->query.len == 0) {
    set_text(layout, sb_str(&ui->placeholder));
    set_color(cr, ui->colors.placeholder);
    show(cr, layout, g, x, y);
  } else {
    set_text(layout, sb_str(&v->query));
    PangoRectangle pos;
    pango_layout_index_to_pos(layout, (int)v->cursor, &pos);
    cur_x = (double)pos.x / PANGO_SCALE;
    double scroll = cur_x > avail ? cur_x - avail : 0;
    set_color(cr, ui->colors.input);
    show(cr, layout, g, x - scroll, y);
    cur_x -= scroll;
  }

  set_color(cr, ui->colors.cursor);
  cairo_rectangle(cr, x + cur_x, y + ui->row_padding, CURSOR_W, dc->line_h);
  cairo_fill(cr);
  cairo_restore(cr);
}

/* Moves dc->first so the selection is one of the visible rows. */
static void scroll_to_selection(const struct state* v,
                                struct draw_ctx* dc,
                                size_t visible) {
  size_t n = v->matches.len;
  if (v->selected < dc->first)
    dc->first = v->selected;
  else if (v->selected >= dc->first + visible)
    dc->first = v->selected + 1 - visible;
  if (dc->first + visible > n)
    dc->first = n > visible ? n - visible : 0;
}

static void draw_rows(const struct state* v,
                      struct draw_ctx* dc,
                      const struct geom* g,
                      cairo_t* cr,
                      PangoLayout* layout,
                      int w,
                      int h) {
  const struct config_ui* ui = dc->ui;
  const struct config_colors* c = &ui->colors;
  double x = g->inset, cw = w - 2 * g->inset;
  double list_h = h - g->inset - g->list_y;
  size_t n = v->matches.len;
  size_t visible = list_h > 0 ? (size_t)(list_h / g->row_h) : 0;
  if (visible == 0)
    return;

  if (n == 0) {
    dc->first = 0;
    if (v->entries.len > 0) {
      set_text(layout, STR("No matches"));
      set_color(cr, c->placeholder);
      show(cr, layout, g, x + ui->row_padding, g->list_y);
    }
    return;
  }
  scroll_to_selection(v, dc, visible);

  /* Leave room for a scrollbar in the right padding when it's needed. */
  bool scrollbar = n > visible && ui->padding >= SCROLLBAR_W + 2;

  for (size_t i = dc->first; i < n && i < dc->first + visible; i++) {
    double y = g->list_y + (double)(i - dc->first) * g->row_h;
    bool sel = i == v->selected;
    if (sel) {
      rounded_rect(cr, x, y, cw, g->row_h, ui->selection_radius);
      set_color(cr, c->selection);
      cairo_fill(cr);
    }

    const struct state_entry* e = state_match(v, i);
    set_text(layout, e->name);
    pango_layout_set_width(layout,
                           (int)((cw - 2 * ui->row_padding) * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    size_t off = state_match_offset(v, i);
    if (v->query.len > 0 && off != STR_NPOS) {
      PangoAttrList* attrs = highlight(off, off + v->query.len,
                                       sel ? c->selection_match : c->match);
      pango_layout_set_attributes(layout, attrs);
      pango_attr_list_unref(attrs);
    }
    set_color(cr, sel ? c->selection_text : c->text);
    show(cr, layout, g, x + ui->row_padding, y);
  }

  if (scrollbar) {
    double track = visible * g->row_h;
    double thumb = track * (double)visible / (double)n;
    double ty = g->list_y + track * (double)dc->first / (double)n;
    double tx = w - g->inset + (ui->padding - SCROLLBAR_W) / 2.0;
    rounded_rect(cr, tx, ty, SCROLLBAR_W, thumb, SCROLLBAR_W / 2.0);
    set_color(cr, c->counter);
    cairo_fill(cr);
  }
}

void draw(const struct state* v,
          struct draw_ctx* dc,
          cairo_t* cr,
          int w,
          int h) {
  const struct config_ui* ui = dc->ui;
  struct geom g = geometry(dc);

  /* Buffers are reused, so clear to transparent first. */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  draw_panel(cr, ui, w, h);

  PangoLayout* layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, dc->font);

  draw_query(v, dc, &g, cr, layout, w);

  if (ui->colors.separator.a > 0 && ui->padding > 0) {
    set_color(cr, ui->colors.separator);
    cairo_rectangle(cr, g.inset, g.sep_y, w - 2 * g.inset, 1);
    cairo_fill(cr);
  }

  draw_rows(v, dc, &g, cr, layout, w, h);

  g_object_unref(layout);
}
