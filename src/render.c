#include "render.h"
#include "log.h"

#include <stdint.h>

static int try_present(struct render *r);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  (void)time;
  struct render *r = data;
  wl_callback_destroy(cb);
  r->frame_cb = NULL;
  try_present(r);
}
static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void on_frame_release(void *data, struct frame *f) {
  (void)f;
  try_present(data); /* covers the case where every buffer was busy */
}

static struct frame *acquire_frame(struct render *r) {
  for (size_t i = 0; i < sizeof r->frames / sizeof r->frames[0]; i++) {
    struct frame *f = &r->frames[i];
    if (f->busy)
      continue;
    if (f->buffer && f->width == r->width && f->height == r->height)
      return f;

    /* Free but missing or the wrong size: safe to replace, since the
     * compositor doesn't hold it. */
    frame_destroy(f);
    enum lm_err e =
        frame_init(f, r->shm, r->width, r->height, on_frame_release, r);
    if (e != LM_OK) {
      r->err = e;
      return NULL;
    }
    return f;
  }
  return NULL; /* all busy; a release will retry */
}

static int try_present(struct render *r) {
  if (r->err != LM_OK)
    return -1;
  if (!r->dirty || r->frame_cb != NULL || r->width <= 0 || r->height <= 0)
    return 0;

  struct frame *f = acquire_frame(r);
  if (f == NULL)
    return r->err != LM_OK ? -1 : 0;

  cairo_t *cr = cairo_create(f->cs);
  draw(r->state, &r->dctx, cr, f->width, f->height);
  cairo_destroy(cr);
  cairo_surface_flush(f->cs);

  wl_surface_attach(r->surface, f->buffer, 0, 0);
  wl_surface_damage_buffer(r->surface, 0, 0, INT32_MAX, INT32_MAX);
  r->frame_cb = wl_surface_frame(r->surface);
  wl_callback_add_listener(r->frame_cb, &frame_listener, r);
  f->busy = true;
  wl_surface_commit(r->surface);

  r->dirty = false;
  return 0;
}

int render_init(struct render *r, struct wl_shm *shm,
                struct wl_surface *surface, const struct state *state) {
  *r = (struct render){.surface = surface, .shm = shm, .state = state};
  if (draw_ctx_init(&r->dctx) != 0) {
    LOG_ERROR("failed to create font description");
    return -1;
  }
  return 0;
}

void render_destroy(struct render *r) {
  if (r->frame_cb)
    wl_callback_destroy(r->frame_cb);
  for (size_t i = 0; i < sizeof r->frames / sizeof r->frames[0]; i++)
    frame_destroy(&r->frames[i]);
  draw_ctx_destroy(&r->dctx);
  *r = (struct render){0};
}

void render_resize(struct render *r, int w, int h) {
  if (w == r->width && h == r->height)
    return;
  r->width = w;
  r->height = h;
  r->dirty = true;
}

int render_request(struct render *r) {
  r->dirty = true;
  return try_present(r);
}
