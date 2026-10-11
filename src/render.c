#include "render.h"

#include <stdint.h>

#include "log.h"

static int try_present(struct render* r);

static void frame_done(void* data, struct wl_callback* cb, uint32_t time) {
  (void)time;
  struct render* r = data;
  wl_callback_destroy(cb);
  r->frame_cb = NULL;
  try_present(r);
}
static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void on_frame_release(void* data, struct frame* f) {
  (void)f;
  try_present(data); /* covers the case where every buffer was busy */
}

/* round(logical * scale), as the fractional-scale protocol prescribes. */
static int to_buffer(int v, uint32_t scale120) {
  return (int)(((int64_t)v * scale120 + 60) / 120);
}

static struct frame* acquire_frame(struct render* r) {
  int bw = to_buffer(r->width, r->scale120);
  int bh = to_buffer(r->height, r->scale120);
  for (size_t i = 0; i < sizeof r->frames / sizeof r->frames[0]; i++) {
    struct frame* f = &r->frames[i];
    if (f->busy)
      continue;
    if (f->buffer && f->width == bw && f->height == bh)
      return f;

    /* Free but missing or the wrong size: safe to replace, since the
     * compositor doesn't hold it. */
    frame_destroy(f);
    enum lm_err e = frame_init(f, r->shm, bw, bh, on_frame_release, r);
    if (e != LM_OK) {
      r->err = e;
      return NULL;
    }
    return f;
  }
  return NULL; /* all busy; a release will retry */
}

static int try_present(struct render* r) {
  if (r->err != LM_OK)
    return -1;
  bool stale = r->dirty || r->state->gen != r->drawn_gen;
  if (!stale || r->frame_cb != NULL || r->width <= 0 || r->height <= 0)
    return 0;

  struct frame* f = acquire_frame(r);
  if (f == NULL)
    return r->err != LM_OK ? -1 : 0;

  /* Draw in logical units; cairo and pango render at buffer resolution. */
  cairo_t* cr = cairo_create(f->cs);
  cairo_scale(cr, r->scale120 / 120.0, r->scale120 / 120.0);
  draw(r->state, &r->dctx, cr, r->width, r->height);
  cairo_destroy(cr);
  cairo_surface_flush(f->cs);

  /* Integer scales go through buffer_scale. Fractional ones rely on the
   * viewport destination, so the buffer is taken at 1:1 in buffer pixels. */
  int32_t buffer_scale =
      r->scale120 % 120 == 0 ? (int32_t)r->scale120 / 120 : 1;
  wl_surface_set_buffer_scale(r->surface, buffer_scale);
  wl_surface_attach(r->surface, f->buffer, 0, 0);
  wl_surface_damage_buffer(r->surface, 0, 0, INT32_MAX, INT32_MAX);
  r->frame_cb = wl_surface_frame(r->surface);
  wl_callback_add_listener(r->frame_cb, &frame_listener, r);
  f->busy = true;
  wl_surface_commit(r->surface);

  r->dirty = false;
  r->drawn_gen = r->state->gen;
  return 0;
}

int render_init(struct render* r,
                struct wl_shm* shm,
                struct wl_surface* surface,
                const struct state* state,
                const struct config_ui* ui) {
  *r = (struct render){
      .surface = surface,
      .shm = shm,
      .state = state,
      .scale120 = 120,
      .dirty = true, /* nothing drawn yet */
  };
  if (draw_ctx_init(&r->dctx, ui) != 0) {
    LOG_ERROR("failed to set up drawing (bad font or out of memory)");
    return -1;
  }
  return 0;
}

void render_destroy(struct render* r) {
  if (r->frame_cb)
    wl_callback_destroy(r->frame_cb);
  for (size_t i = 0; i < sizeof r->frames / sizeof r->frames[0]; i++)
    frame_destroy(&r->frames[i]);
  draw_ctx_destroy(&r->dctx);
  *r = (struct render){0};
}

void render_resize(struct render* r, int w, int h) {
  if (w == r->width && h == r->height)
    return;
  r->width = w;
  r->height = h;
  r->dirty = true;
}

void render_set_scale(struct render* r, uint32_t scale120) {
  if (scale120 == 0 || scale120 == r->scale120)
    return;
  r->scale120 = scale120;
  r->dirty = true;
}

void render_invalidate(struct render* r) {
  r->dirty = true;
}

int render_request(struct render* r) {
  return try_present(r);
}
