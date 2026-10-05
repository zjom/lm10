/*
 * Minimal Wayland client: opens an xdg-shell window filled with a checkerboard.
 *
 * Build:
 * mkdir bld && meson setup bld && ninja -C bld
 *
 * Run inside a Wayland session (GNOME, KDE, Sway, ...): ./bld/lm10
 */
#define _GNU_SOURCE
#include <stdlib.h>

#include "glib-object.h"
#include "pango/pango-font.h"
#include "pango/pango-layout.h"
#include "xdg-shell-client-protocol.h"
#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

struct strlist {
  size_t len;
  size_t cap;
  char **items;
};
void strlist_init(struct strlist *sl) {
  sl->cap = 0;
  sl->len = 0;
  sl->items = NULL;
}

int strlist_resize(struct strlist *sl) {
  size_t new_cap = sl->cap ? sl->cap * 2 : 8;
  char **tmp = realloc(sl->items, new_cap * sizeof(*tmp));
  if (!tmp)
    return -1;
  sl->items = tmp;
  sl->cap = new_cap;
  return 0;
}

int strlist_push(struct strlist *sl, const char *s) {
  if (sl->len == sl->cap) {
    if (strlist_resize(sl)) {
      return -1;
    }
  }
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (!copy)
    return -1;
  memcpy(copy, s, n);
  sl->items[sl->len++] = copy;
  return 0;
}

void strlist_free(struct strlist *sl) {
  for (size_t i = 0; i < sl->len; i++) {
    free(sl->items[i]);
  }
  free(sl->items);
  strlist_init(sl);
}

struct app {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct xdg_wm_base *wm_base;
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  int width, height;
  bool running;
  struct strlist *items;
};

/* ---- shared-memory buffer ---- */

static void buffer_release(void *data, struct wl_buffer *buffer) {
  (void)data;
  wl_buffer_destroy(buffer); /* compositor is done with it */
}
static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static struct wl_buffer *draw_frame(struct app *app) {
  int w = app->width, h = app->height, stride = w * 4, size = stride * h;

  int fd = memfd_create("lm10-shm", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) < 0) {
    perror("shm");
    return NULL;
  }

  uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) {
    perror("mmap");
    close(fd);
    return NULL;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, size);
  struct wl_buffer *buf =
      wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  cairo_surface_t *cs = cairo_image_surface_create_for_data(
      (unsigned char *)px, CAIRO_FORMAT_RGB24, w, h, stride);
  cairo_t *cr = cairo_create(cs);

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

  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font =
      pango_font_description_from_string("Sans Bold 28");
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);

  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < app->items->len; i++) {
    if (i)
      g_string_append_c(s, '\n');
    g_string_append(s, app->items->items[i]);
  }
  pango_layout_set_text(layout, s->str, -1);
  g_string_free(s, TRUE);

  int tw, th;
  pango_layout_get_pixel_size(layout, &tw, &th);
  double tx = (w - tw) / 2.0, ty = (h - th) / 2.0;
  cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
  cairo_rectangle(cr, tx - 16, ty - 8, tw + 32, th + 16);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  cairo_move_to(cr, tx, ty);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_flush(cs);
  cairo_surface_destroy(cs);

  munmap(px, size);
  wl_buffer_add_listener(buf, &buffer_listener, NULL);
  return buf;
}

/* ---- xdg-shell listeners ---- */

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial) {
  struct app *app = data;
  xdg_surface_ack_configure(xs, serial);

  struct wl_buffer *buf = draw_frame(app);
  if (!buf) {
    app->running = false;
    return;
  }
  wl_surface_attach(app->surface, buf, 0, 0);
  wl_surface_damage_buffer(app->surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(app->surface);
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t w,
                               int32_t h, struct wl_array *states) {
  (void)tl;
  (void)states;
  struct app *app = data;
  if (w > 0 && h > 0) {
    app->width = w;
    app->height = h;
  } /* 0 = our choice */
}
static void toplevel_close(void *data, struct xdg_toplevel *tl) {
  (void)tl;
  ((struct app *)data)->running = false;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

/* ---- registry ---- */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
  (void)version;
  struct app *app = data;
  if (strcmp(iface, wl_compositor_interface.name) == 0) {
    app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  } else if (strcmp(iface, wl_shm_interface.name) == 0) {
    app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
    app->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
  }
}
static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name) {
  (void)data;
  (void)reg;
  (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
  struct strlist items;
  strlist_init(&items);
  for (int i = 0; i < argc; i++) {
    strlist_push(&items, argv[i]);
  }

  struct app app = {
      .width = 640, .height = 480, .running = true, .items = &items};

  app.display = wl_display_connect(NULL);
  if (!app.display) {
    fprintf(stderr, "Can't connect to a Wayland display\n");
    return 1;
  }

  app.registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(app.registry, &registry_listener, &app);
  wl_display_roundtrip(app.display); /* receive the globals */

  if (!app.compositor || !app.shm || !app.wm_base) {
    fprintf(stderr, "Compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
    return 1;
  }

  app.surface = wl_compositor_create_surface(app.compositor);
  app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
  xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
  app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
  xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
  xdg_toplevel_set_title(app.toplevel, "Hello Wayland");
  wl_surface_commit(app.surface); /* empty commit -> triggers first configure */

  while (app.running && wl_display_dispatch(app.display) != -1)
    ;

  xdg_toplevel_destroy(app.toplevel);
  xdg_surface_destroy(app.xdg_surface);
  wl_surface_destroy(app.surface);
  xdg_wm_base_destroy(app.wm_base);
  wl_shm_destroy(app.shm);
  wl_compositor_destroy(app.compositor);
  wl_registry_destroy(app.registry);
  wl_display_disconnect(app.display);
  strlist_free(app.items);
  return 0;
}
