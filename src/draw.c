#include "draw.h"

#include <math.h>
#include <stdio.h>

#include <glib-object.h>
#include <pango/pangocairo.h>

/* Gap between the query text and the match counter. */
#define COUNTER_GAP 12
#define CURSOR_W 2
#define SCROLLBAR_W 3
/* Gap between a row's icon and its name. */
#define ICON_GAP 8

int draw_ctx_init(struct draw_ctx* dc, const struct config_ui* ui) {
  *dc = (struct draw_ctx){.ui = ui, .spans = LIST_INIT};
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

  if (ui->icons) {
    dc->icons = icon_cache_new(sb_cstr(&ui->icon_theme));
    if (dc->icons == NULL) {
      draw_ctx_destroy(dc);
      return -1;
    }
  }
  return 0;
}

void draw_ctx_destroy(struct draw_ctx* dc) {
  if (dc->font)
    pango_font_description_free(dc->font);
  icon_cache_destroy(dc->icons);
  LIST_FREE(&dc->spans);
  *dc = (struct draw_ctx){0};
}

/* ---- geometry ---- */

/* Everything is laid out from the panel edges inwards:
 *
 *   inset | query row | gap | separator | gap | header | rows... | inset
 *
 * A border label sits centered on the top or bottom border line, so the panel
 * is moved in from that surface edge far enough for it to fit.
 */
struct geom {
  double inset;  // border + padding
  double row_h;
  double top, bottom;  // space between the panel and the surface edges
  double query_y, sep_y, header_y, list_y;
};

/* Lines in the header, 0 if there is none. */
static size_t header_lines(const struct state* state) {
  if (state->header.len == 0)
    return 0;
  size_t n = 1;
  for (size_t i = 0; i < state->header.len; i++)
    n += state->header.ptr[i] == '\n';
  return n;
}

static struct geom geometry(const struct draw_ctx* dc,
                            const struct state* state) {
  const struct config_ui* ui = dc->ui;
  struct geom g = {0};
  if (state->border_label.len > 0) {
    double out = ceil(dc->line_h / 2.0 - ui->border_width / 2.0);
    if (out > 0)
      *(ui->border_label_bottom ? &g.bottom : &g.top) = out;
  }
  size_t lines = header_lines(state);
  g.inset = ui->border_width + ui->padding;
  g.row_h = dc->line_h + 2.0 * ui->row_padding;
  g.query_y = g.top + g.inset;
  g.sep_y = g.query_y + g.row_h + ui->padding / 2;
  g.header_y = g.query_y + g.row_h + ui->padding;
  g.list_y = g.header_y;
  if (lines > 0)
    g.list_y += (double)lines * dc->line_h + 2.0 * ui->row_padding;
  return g;
}

int draw_height(const struct draw_ctx* dc, const struct state* state) {
  struct geom g = geometry(dc, state);
  return (int)ceil(g.list_y + dc->ui->lines * g.row_h + g.inset + g.bottom);
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

/* Attributes drawing the spans in c, bold. */
static PangoAttrList* highlight(const SpanList* spans, struct config_color c) {
  PangoAttrList* attrs = pango_attr_list_new();
  ARR_FOREACH(const struct state_span, sp, spans) {
    PangoAttribute* a[] = {
        pango_attr_foreground_new((guint16)(c.r * 65535),
                                  (guint16)(c.g * 65535),
                                  (guint16)(c.b * 65535)),
        pango_attr_foreground_alpha_new((guint16)(c.a * 65535)),
        pango_attr_weight_new(PANGO_WEIGHT_BOLD),
    };
    for (size_t i = 0; i < sizeof a / sizeof a[0]; i++) {
      a[i]->start_index = (guint)sp->start;
      a[i]->end_index = (guint)(sp->start + sp->len);
      pango_attr_list_insert(attrs, a[i]);
    }
  }
  return attrs;
}

/* The entry's icon rendered at the text height in buffer pixels, or NULL.
 * Guessed icons that don't exist are expected, so they don't warn. */
static cairo_surface_t* entry_icon(struct draw_ctx* dc,
                                   cairo_t* cr,
                                   const struct state_entry* e) {
  double px = dc->line_h, unused = 0;
  cairo_user_to_device_distance(cr, &px, &unused);
  return icon_cache_get(dc->icons, e->icon, (int)lround(px), !e->icon_guessed);
}

/* Paints an icon with its top left at x, y, snapped to buffer pixels so it
 * stays sharp at any scale. Symbolic icons are monochrome shapes, so they
 * take the text color like they do in GTK. */
static void draw_icon(cairo_t* cr,
                      cairo_surface_t* icon,
                      Str name,
                      struct config_color text,
                      double x,
                      double y) {
  cairo_user_to_device(cr, &x, &y);
  cairo_save(cr);
  cairo_identity_matrix(cr);
  if (str_ends_with(name, STR("-symbolic"))) {
    set_color(cr, text);
    cairo_mask_surface(cr, icon, round(x), round(y));
  } else {
    cairo_set_source_surface(cr, icon, round(x), round(y));
    cairo_paint(cr);
  }
  cairo_restore(cr);
}

/* ---- parts ---- */

/* Where the border label goes, in a gap cut out of the border. */
struct label_box {
  double x, y, w, h;
};

/* Lays the border label out and places it, or returns false if there is
 * none. The label stays clear of the rounded corners, ellipsized if it
 * doesn't fit between them. */
static bool place_label(const struct config_ui* ui,
                        const struct state* state,
                        const struct geom* g,
                        PangoLayout* layout,
                        int w,
                        int h,
                        struct label_box* box) {
  if (state->border_label.len == 0)
    return false;
  double bw = ui->border_width;
  double margin = ui->border_radius > bw ? ui->border_radius : bw;
  double room = w - 2 * margin;
  if (room <= 0)
    return false;

  set_text(layout, sb_str(&state->border_label));
  pango_layout_set_width(layout, (int)(room * PANGO_SCALE));
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  int tw, th;
  pango_layout_get_pixel_size(layout, &tw, &th);

  int pos = ui->border_label_pos;
  double x = pos == 0 ? (w - tw) / 2.0 : pos > 0 ? pos : w + pos - tw;
  if (x > w - margin - tw)
    x = w - margin - tw;
  if (x < margin)
    x = margin;
  double line_y =
      ui->border_label_bottom ? h - g->bottom - bw / 2 : g->top + bw / 2;
  *box = (struct label_box){x, line_y - th / 2.0, tw, th};
  return true;
}

static void draw_panel(cairo_t* cr,
                       const struct config_ui* ui,
                       const struct geom* g,
                       const struct label_box* label,
                       int w,
                       int h) {
  double bw = ui->border_width;
  double ph = h - g->top - g->bottom;
  rounded_rect(cr, bw / 2, g->top + bw / 2, w - bw, ph - bw,
               ui->border_radius - bw / 2);
  set_color(cr, ui->colors.background);
  if (bw <= 0) {
    cairo_fill(cr);
    return;
  }
  cairo_fill_preserve(cr);

  cairo_save(cr);
  if (label != NULL) {
    /* Everything but the label's box. */
    cairo_new_path(cr);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_rectangle(cr, label->x, label->y, label->w, label->h);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
    rounded_rect(cr, bw / 2, g->top + bw / 2, w - bw, ph - bw,
                 ui->border_radius - bw / 2);
  }
  set_color(cr, ui->colors.border);
  cairo_set_line_width(cr, bw);
  cairo_stroke(cr);
  cairo_restore(cr);
}

/* Shows the laid out label in its box. */
static void draw_label(cairo_t* cr,
                       const struct config_ui* ui,
                       PangoLayout* layout,
                       const struct label_box* label) {
  set_color(cr, ui->colors.border_label);
  cairo_move_to(cr, label->x, label->y);
  pango_cairo_show_layout(cr, layout);
}

static void draw_header(const struct draw_ctx* dc,
                        const struct state* state,
                        const struct geom* g,
                        cairo_t* cr,
                        PangoLayout* layout,
                        int w) {
  const struct config_ui* ui = dc->ui;
  double text_w = w - 2 * g->inset - 2 * ui->row_padding;
  if (state->header.len == 0 || text_w <= 0)
    return;
  /* Each line is ellipsized on its own. */
  set_text(layout, sb_str(&state->header));
  pango_layout_set_width(layout, (int)(text_w * PANGO_SCALE));
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  set_color(cr, ui->colors.header);
  cairo_move_to(cr, g->inset + ui->row_padding, g->header_y + ui->row_padding);
  pango_cairo_show_layout(cr, layout);
}

static void draw_query(const struct state* state,
                       const struct draw_ctx* dc,
                       const struct geom* g,
                       cairo_t* cr,
                       PangoLayout* layout,
                       int w) {
  const struct config_ui* ui = dc->ui;
  double x = g->inset + ui->row_padding, y = g->query_y;
  double right = w - g->inset - ui->row_padding;

  set_text(layout, sb_str(&state->prompt));
  set_color(cr, ui->colors.prompt);
  show(cr, layout, g, x, y);
  x += text_width(layout);

  if (ui->show_counter && state->entries.len > 0) {
    char buf[48];
    int n = snprintf(buf, sizeof buf, "%zu/%zu", state->matches.len,
                     state->entries.len);
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
  if (state->query.len == 0) {
    set_text(layout, sb_str(&ui->placeholder));
    set_color(cr, ui->colors.placeholder);
    show(cr, layout, g, x, y);
  } else {
    set_text(layout, sb_str(&state->query));
    PangoRectangle pos;
    pango_layout_index_to_pos(layout, (int)state->cursor, &pos);
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
static void scroll_to_selection(const struct state* state,
                                struct draw_ctx* dc,
                                size_t visible) {
  size_t n = state->matches.len;
  if (state->selected < dc->first)
    dc->first = state->selected;
  else if (state->selected >= dc->first + visible)
    dc->first = state->selected + 1 - visible;
  if (dc->first + visible > n)
    dc->first = n > visible ? n - visible : 0;
}

static void draw_rows(const struct state* state,
                      struct draw_ctx* dc,
                      const struct geom* g,
                      cairo_t* cr,
                      PangoLayout* layout,
                      int w,
                      int h) {
  const struct config_ui* ui = dc->ui;
  const struct config_colors* c = &ui->colors;
  double x = g->inset, cw = w - 2 * g->inset;
  double list_h = h - g->bottom - g->inset - g->list_y;
  size_t n = state->matches.len;
  size_t visible = list_h > 0 ? (size_t)(list_h / g->row_h) : 0;
  if (visible == 0)
    return;

  if (n == 0) {
    dc->first = 0;
    if (state->entries.len > 0) {
      set_text(layout, STR("No matches"));
      set_color(cr, c->placeholder);
      show(cr, layout, g, x + ui->row_padding, g->list_y);
    }
    return;
  }
  scroll_to_selection(state, dc, visible);

  /* Leave room for a scrollbar in the right padding when it's needed. */
  bool scrollbar = n > visible && ui->padding >= SCROLLBAR_W + 2;
  /* Every row gets the icon column once a visible icon has loaded, so names
   * line up, and it stays so they don't jump around while filtering. */
  for (size_t i = dc->first;
       dc->icons && !dc->icon_column && i < n && i < dc->first + visible; i++)
    dc->icon_column = entry_icon(dc, cr, state_match(state, i)) != NULL;
  double icon_w = dc->icon_column ? dc->line_h + ICON_GAP : 0;
  double text_x = x + ui->row_padding + icon_w;

  for (size_t i = dc->first; i < n && i < dc->first + visible; i++) {
    double y = g->list_y + (double)(i - dc->first) * g->row_h;
    bool sel = i == state->selected;
    if (sel) {
      rounded_rect(cr, x, y, cw, g->row_h, ui->selection_radius);
      set_color(cr, c->selection);
      cairo_fill(cr);
    }

    const struct state_entry* e = state_match(state, i);
    cairo_surface_t* icon = dc->icon_column ? entry_icon(dc, cr, e) : NULL;
    if (icon != NULL)
      draw_icon(cr, icon, e->icon, sel ? c->selection_text : c->text,
                x + ui->row_padding, y + ui->row_padding);

    set_text(layout, e->name);
    pango_layout_set_width(
        layout, (int)((cw - 2 * ui->row_padding - icon_w) * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    if (state_match_spans(state, i, &dc->spans) && dc->spans.len > 0) {
      PangoAttrList* attrs =
          highlight(&dc->spans, sel ? c->selection_match : c->match);
      pango_layout_set_attributes(layout, attrs);
      pango_attr_list_unref(attrs);
    }
    set_color(cr, sel ? c->selection_text : c->text);
    show(cr, layout, g, text_x, y);
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

void draw(const struct state* state,
          struct draw_ctx* dc,
          cairo_t* cr,
          int w,
          int h) {
  const struct config_ui* ui = dc->ui;
  struct geom g = geometry(dc, state);

  /* Buffers are reused, so clear to transparent first. */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  PangoLayout* layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, dc->font);

  struct label_box label;
  bool has_label = place_label(ui, state, &g, layout, w, h, &label);
  draw_panel(cr, ui, &g, has_label ? &label : NULL, w, h);
  if (has_label)
    draw_label(cr, ui, layout, &label);

  draw_query(state, dc, &g, cr, layout, w);

  if (ui->colors.separator.a > 0 && ui->padding > 0) {
    set_color(cr, ui->colors.separator);
    cairo_rectangle(cr, g.inset, g.sep_y, w - 2 * g.inset, 1);
    cairo_fill(cr);
  }

  draw_header(dc, state, &g, cr, layout, w);
  draw_rows(state, dc, &g, cr, layout, w, h);

  g_object_unref(layout);
}
