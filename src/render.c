#include "render.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

/* ---- frames ---- */

static void buffer_release(void* data, struct wl_buffer* buffer) {
  (void)buffer;
  struct frame* f = data;
  f->busy = false;
  render_request(f->r);  // covers the case where every buffer was busy
}
static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

/* Sets up f as a w x h buffer. f must stay at this address while the buffer
 * exists. Dies if it can't. */
static void frame_init(struct frame* f,
                       struct render* r,
                       int w,
                       int h) {
  int stride = w * 4;
  size_t size = (size_t)stride * h;

  int fd = memfd_create("lm10-shm", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) < 0)
    die("shm buffer %dx%d: %s", w, h, strerror(errno));

  uint32_t* px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED)
    die("shm buffer mmap: %s", strerror(errno));

  cairo_surface_t* cs = cairo_image_surface_create_for_data(
      (unsigned char*)px, CAIRO_FORMAT_ARGB32, w, h, stride);
  cairo_status_t status = cairo_surface_status(cs);
  if (status != CAIRO_STATUS_SUCCESS)
    die("cairo surface %dx%d: %s", w, h, cairo_status_to_string(status));

  struct wl_shm_pool* pool = wl_shm_create_pool(r->shm, fd, size);
  struct wl_buffer* buf =
      wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  *f = (struct frame){
      .r = r,
      .buffer = buf,
      .data = px,
      .size = size,
      .width = w,
      .height = h,
      .cs = cs,
  };
  wl_buffer_add_listener(buf, &buffer_listener, f);
}

static void frame_destroy(struct frame* f) {
  if (f->buffer)
    wl_buffer_destroy(f->buffer);
  if (f->cs)
    cairo_surface_destroy(f->cs);
  if (f->data)
    munmap(f->data, f->size);
  *f = (struct frame){0};
}

/* ---- presenting ---- */

static void frame_done(void* data, struct wl_callback* cb, uint32_t time) {
  (void)time;
  struct render* r = data;
  wl_callback_destroy(cb);
  r->frame_cb = NULL;
  render_request(r);
}
static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

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
    frame_init(f, r, bw, bh);
    return f;
  }
  return NULL;  // all busy; a release will retry
}

void render_request(struct render* r) {
  bool stale = r->dirty || r->state->gen != r->drawn_gen;
  if (!stale || r->frame_cb != NULL || r->width <= 0 || r->height <= 0)
    return;

  struct frame* f = acquire_frame(r);
  if (f == NULL)
    return;

  /* Draw in logical units; cairo and pango render at buffer resolution. */
  cairo_t* cr = cairo_create(f->cs);
  cairo_scale(cr, r->scale120 / 120.0, r->scale120 / 120.0);
  state_sort(r->state);
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
}

int render_init(struct render* r,
                struct wl_shm* shm,
                struct wl_surface* surface,
                struct state* state,
                const struct config_ui* ui) {
  *r = (struct render){
      .surface = surface,
      .shm = shm,
      .state = state,
      .scale120 = 120,
      .dirty = true,  // nothing drawn yet
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
