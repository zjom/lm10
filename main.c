/*
 * Minimal Wayland client: opens an xdg-shell window filled with a checkerboard.
 *
 * Build:
 * mkdir bld && meson setup bld && ninja -C bld
 *
 * Run inside a Wayland session (GNOME, KDE, Sway, ...): ./bld/lm10
 */
#define _GNU_SOURCE

#include "glib-object.h"
#include "pango/pango-font.h"
#include "pango/pango-layout.h"
#include "xdg-shell-client-protocol.h"
#include <assert.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

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

enum lm10error {
  LM_ERR_UNKNOWN_KEYMAP_FORMAT = -1,
  LM_ERR_KEYMAP_COMPILE_FAILED = -2,
  LM_ERR_KEY_EVENT_NO_KEYMAP = -3,
  LM_ERR_FAILED_CREATE_STATE_FROM_KEYMAP = -4,
  LM_ERR_MMAP = -5,
  LM_ERR_SHM = -6,
};
static void lm10_perror(enum lm10error code) {
  switch (code) {
  case LM_ERR_UNKNOWN_KEYMAP_FORMAT:
    fprintf(stderr, "unknown keymap format\n");
    break;
  case LM_ERR_KEYMAP_COMPILE_FAILED:
    fprintf(stderr, "failed to compile keymap\n");
    break;
  case LM_ERR_KEY_EVENT_NO_KEYMAP:
    fprintf(stderr, "received key event when no keymap registered\n");
    break;
  case LM_ERR_FAILED_CREATE_STATE_FROM_KEYMAP:
    fprintf(stderr, "xkb failed to create state from keymap\n");
    break;
  case LM_ERR_MMAP:
    fprintf(stderr, "mmap error\n");
    break;
  case LM_ERR_SHM:
    fprintf(stderr, "shm error\n");
    break;
  default:
    fprintf(stderr, "unknown error\n");
    break;
  }
}

struct app;

struct seat {
  char *name;
  uint32_t wl_name;
  struct wl_seat *wl_seat;

  struct app *app;

  struct wl_keyboard *wl_kbd;
  struct {
    uint32_t serial;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
  } kbd;
};

struct app {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct xdg_wm_base *wm_base;
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  struct xkb_context *xkb_ctx;
  struct seat seat;
  int width, height;
  bool running;
  enum lm10error errcode;
  struct strlist *items;
};

static void seat_free(struct seat *s) {
  free(s->name);
  s->name = NULL;
  s->wl_name = 0;

  s->app = NULL;

  if (s->wl_kbd != NULL) {
    wl_keyboard_release(s->wl_kbd);
    s->wl_kbd = NULL;
    s->kbd.serial = 0;
    xkb_keymap_unref(s->kbd.xkb_keymap);
    s->kbd.xkb_keymap = NULL;
    xkb_state_unref(s->kbd.xkb_state);
    s->kbd.xkb_state = NULL;
  }

  if (s->wl_seat != NULL) {
    wl_seat_get_version(s->wl_seat) >= 5 ? wl_seat_release(s->wl_seat)
                                         : wl_seat_destroy(s->wl_seat);
    s->wl_seat = NULL;
  }
}

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
    app->errcode = LM_ERR_SHM;
    if (fd >= 0) {
      close(fd);
    }
    return NULL;
  }

  uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) {
    app->errcode = LM_ERR_MMAP;
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

/* ---- seat listener ---- */

static void keyboard_handle_repeat(void *data, struct wl_keyboard *wl_keyboard,
                                   int32_t rate, int32_t delay) {
  (void)data;
  (void)wl_keyboard;
  (void)rate;
  (void)delay;
}

static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
                                   uint32_t format, int32_t fd, uint32_t size) {
  (void)wl_keyboard;
  struct seat *seat = data;

  xkb_keymap_unref(seat->kbd.xkb_keymap);
  seat->kbd.xkb_keymap = NULL;
  xkb_state_unref(seat->kbd.xkb_state);
  seat->kbd.xkb_state = NULL;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    seat->app->errcode = LM_ERR_UNKNOWN_KEYMAP_FORMAT;
    close(fd);
    return;
  }

  char *buf = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (buf == MAP_FAILED) {
    seat->app->errcode = LM_ERR_MMAP;
    close(fd);
    return;
  }
  struct xkb_keymap *keymap = xkb_keymap_new_from_string(
      seat->app->xkb_ctx, buf, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(buf, size);
  close(fd);

  if (keymap == NULL) {
    seat->app->errcode = LM_ERR_KEYMAP_COMPILE_FAILED;
    return;
  }
  struct xkb_state *xkb_state = xkb_state_new(keymap);
  if (xkb_state == NULL) {
    xkb_keymap_unref(keymap);
    seat->app->errcode = LM_ERR_FAILED_CREATE_STATE_FROM_KEYMAP;
    return;
  }
  seat->kbd.xkb_keymap = keymap;
  seat->kbd.xkb_state = xkb_state;
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state) {
  struct seat *seat = data;
  (void)wl_keyboard;
  (void)serial;
  (void)time;

  if (seat->kbd.xkb_state == NULL) {
    seat->app->errcode = LM_ERR_KEY_EVENT_NO_KEYMAP;
    return;
  }
  bool ctrl_active = xkb_state_mod_name_is_active(
      seat->kbd.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED && ctrl_active) {
    xkb_keysym_t keysym =
        xkb_state_key_get_one_sym(seat->kbd.xkb_state, key + 8);

    switch (keysym) {
    case XKB_KEY_n:
      printf("C-n\n");
      break;
    case XKB_KEY_p:
      printf("C-p\n");
      break;
    default:
      return;
    }
  }
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys) {
  struct seat *seat = data;
  seat->kbd.serial = serial;
  (void)wl_keyboard;
  (void)surface;
  (void)keys;
}
static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, struct wl_surface *surface) {
  struct seat *seat = data;
  seat->kbd.serial = 0;
  (void)wl_keyboard;
  (void)serial;
  (void)surface;
}

static void keyboard_handle_modifiers(void *data,
                                      struct wl_keyboard *wl_keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched,
                                      uint32_t mods_locked, uint32_t group) {
  (void)serial;
  (void)wl_keyboard;
  struct seat *seat = data;
  if (seat->kbd.xkb_state == NULL) {
    seat->app->errcode = LM_ERR_KEY_EVENT_NO_KEYMAP;
    return;
  }
  xkb_state_update_mask(seat->kbd.xkb_state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .repeat_info = keyboard_handle_repeat,
    .key = keyboard_handle_key,
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .modifiers = keyboard_handle_modifiers};

static void seat_handle_name(void *data, struct wl_seat *wl_seat,
                             const char *name) {
  (void)wl_seat;
  struct seat *seat = data;
  free(seat->name);
  seat->name = strdup(name);
}

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
                                     enum wl_seat_capability caps) {
  struct seat *seat = data;
  assert(seat->wl_seat == wl_seat);

  if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
    if (seat->wl_kbd == NULL) {
      seat->wl_kbd = wl_seat_get_keyboard(wl_seat);
      wl_keyboard_add_listener(seat->wl_kbd, &keyboard_listener, seat);
    }
  } else {
    if (seat->wl_kbd != NULL) {
      wl_keyboard_get_version(seat->wl_kbd) >= 3
          ? wl_keyboard_release(seat->wl_kbd)
          : wl_keyboard_destroy(seat->wl_kbd);
      seat->wl_kbd = NULL;
      xkb_state_unref(seat->kbd.xkb_state);
      seat->kbd.xkb_state = NULL;
      xkb_keymap_unref(seat->kbd.xkb_keymap);
      seat->kbd.xkb_keymap = NULL;

      seat->kbd.serial = 0;
    }
  }
}

static const struct wl_seat_listener seat_listener = {
    .name = seat_handle_name, .capabilities = seat_handle_capabilities};

/* ---- registry ---- */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
  struct app *app = data;
  if (strcmp(iface, wl_compositor_interface.name) == 0) {
    app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  } else if (strcmp(iface, wl_shm_interface.name) == 0) {
    app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
    app->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
  } else if (strcmp(iface, wl_seat_interface.name) == 0) {
    seat_free(&app->seat);
    app->seat.wl_seat =
        wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5));
    app->seat.wl_name = name;
    app->seat.app = app;

    wl_seat_add_listener(app->seat.wl_seat, &seat_listener, &app->seat);
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
  struct xkb_context *xkb_ctx = xkb_context_new(0);
  if (xkb_ctx == NULL) {
    fprintf(stderr, "failed to create xkb context\n");
    return 1;
  }

  struct app app = {.width = 640,
                    .height = 480,
                    .running = true,
                    .items = &items,
                    .xkb_ctx = xkb_ctx};

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
  int err = 0;
  while (app.running) {
    if (wl_display_dispatch(app.display) == -1) {
      err = wl_display_get_error(app.display);

      if (err == EPROTO) {
        // It was a Wayland protocol violation
        const struct wl_interface *interface;
        uint32_t id;
        uint32_t code =
            wl_display_get_protocol_error(app.display, &interface, &id);

        fprintf(
            stderr,
            "Fatal Protocol Error: Interface %s, Object ID %u, Error Code %u\n",
            interface != NULL ? interface->name : "unknown", id, code);
      } else {
        // It was a system/socket error (e.g., EPIPE, ECONNRESET)
        fprintf(stderr, "Fatal Socket Error: %s\n", strerror(err));
      }

      break;
    }

    if (app.errcode != 0) {
      lm10_perror(app.errcode);
      err = app.errcode;
      break;
    }
  };

  xkb_context_unref(app.xkb_ctx);
  seat_free(&app.seat);

  xdg_toplevel_destroy(app.toplevel);
  xdg_surface_destroy(app.xdg_surface);
  wl_surface_destroy(app.surface);
  xdg_wm_base_destroy(app.wm_base);
  wl_shm_destroy(app.shm);
  wl_compositor_destroy(app.compositor);
  wl_registry_destroy(app.registry);

  wl_display_disconnect(app.display);
  strlist_free(app.items);
  return err == 0 ? 0 : 1;
}
