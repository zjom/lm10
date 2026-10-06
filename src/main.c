/*
 * Minimal Wayland client: opens an xdg-shell window filled with a checkerboard.
 *
 * Build:
 * mkdir bld && meson setup bld && ninja -C bld
 *
 * Run inside a Wayland session (GNOME, KDE, Sway, ...): ./bld/lm10
 */
#define _GNU_SOURCE
#include "fdm.h"
#include "glib-object.h"
#include "log.h"
#include "pango/pango-font.h"
#include "pango/pango-layout.h"
#include "xdg-shell-client-protocol.h"
#include <assert.h>
#include <cairo.h>
#include <errno.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

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
static char *lm10_strerror(enum lm10error code) {
  switch (code) {
  case LM_ERR_UNKNOWN_KEYMAP_FORMAT:
    return "unknown keymap format\n";
  case LM_ERR_KEYMAP_COMPILE_FAILED:
    return "failed to compile keymap\n";
  case LM_ERR_KEY_EVENT_NO_KEYMAP:
    return "received key event when no keymap registered\n";
  case LM_ERR_FAILED_CREATE_STATE_FROM_KEYMAP:
    return "xkb failed to create state from keymap\n";
  case LM_ERR_MMAP:
    return "mmap error\n";
  case LM_ERR_SHM:
    return "shm error\n";
  default:
    return "unknown error\n";
  }
}

struct app;

struct repeat {
  int32_t rate;      /* repeats per second; 0 = disabled */
  int32_t delay;     /* ms before the first repeat */
  int fd;            /* timerfd, -1 if not created yet */
  xkb_keycode_t key; /* XKB keycode (evdev + 8) being repeated, 0 = none */
};

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
    struct repeat repeat;
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
  struct fdm fdm;
  int width, height;
  bool running;
  enum lm10error errcode;
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

static void seat_handle_keypress(struct seat *seat, xkb_keycode_t keycode) {
  if (xkb_state_mod_name_is_active(seat->kbd.xkb_state, XKB_MOD_NAME_CTRL,
                                   XKB_STATE_MODS_EFFECTIVE) > 0) {
    switch (xkb_state_key_get_one_sym(seat->kbd.xkb_state, keycode)) {
    case XKB_KEY_n:
      printf("C-n\n");
      break;
    case XKB_KEY_p:
      printf("C-p\n");
      break;
    default:
      break;
    }
  }
}

static struct timespec ns_to_timespec(int64_t ns) {
  return (struct timespec){.tv_sec = ns / 1000000000,
                           .tv_nsec = ns % 1000000000};
}

static void repeat_stop(struct seat *seat) {
  struct repeat *r = &seat->kbd.repeat;
  r->key = 0;
  if (r->fd < 0)
    return;

  struct itimerspec ts = {0}; /* all zero = disarm */
  if (timerfd_settime(r->fd, 0, &ts, NULL) == -1)
    LOG_WARN("failed to disarm repeat timer: %s", strerror(errno));
}

static int32_t handle_key_repeat_ready(struct fdm *fdm, void *data);

static bool repeat_ensure_fd(struct seat *seat) {
  struct repeat *r = &seat->kbd.repeat;
  if (r->fd >= 0)
    return true;

  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) {
    LOG_WARN("failed to create timerfd: %s", strerror(errno));
    return false;
  }
  if (fdm_add(&seat->app->fdm, fd, EPOLLIN, seat, handle_key_repeat_ready,
              FDM_AUTOCLOSE) != 0) {
    LOG_WARN("failed to add timerfd to fdm");
    close(fd);
    return false;
  }
  r->fd = fd;
  return true;
}

static void repeat_start(struct seat *seat, xkb_keycode_t keycode) {
  struct repeat *r = &seat->kbd.repeat;
  if (r->rate <= 0) /* compositor disabled key repeat */
    return;
  if (!repeat_ensure_fd(seat))
    return;

  int64_t delay_ns = (int64_t)r->delay * 1000000;
  if (delay_ns <= 0)
    delay_ns = 1; /* all-zero it_value would disarm the timer */

  struct itimerspec ts = {
      .it_value = ns_to_timespec(delay_ns),
      .it_interval = ns_to_timespec(1000000000 / r->rate),
  };
  if (timerfd_settime(r->fd, 0, &ts, NULL) == -1) {
    LOG_WARN("failed to arm repeat timer: %s", strerror(errno));
    return;
  }
  r->key = keycode;
}

static void seat_init(struct seat *s, struct app *app) {
  *s = (struct seat){
      .app = app,
      .kbd.repeat = {.fd = -1},
  };
}

/* Tear down the wl_keyboard and its xkb state. The repeat timerfd is kept
 * (just disarmed) so it can be reused if a keyboard comes back. */
static void seat_keyboard_destroy(struct seat *s) {
  repeat_stop(s);

  if (s->wl_kbd != NULL) {
    if (wl_keyboard_get_version(s->wl_kbd) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
      wl_keyboard_release(s->wl_kbd);
    else
      wl_keyboard_destroy(s->wl_kbd);
    s->wl_kbd = NULL;
  }

  xkb_state_unref(s->kbd.xkb_state);
  s->kbd.xkb_state = NULL;
  xkb_keymap_unref(s->kbd.xkb_keymap);
  s->kbd.xkb_keymap = NULL;
  s->kbd.serial = 0;
}

static void seat_destroy(struct seat *s) {
  seat_keyboard_destroy(s);

  if (s->kbd.repeat.fd >= 0)
    fdm_remove(&s->app->fdm, s->kbd.repeat.fd); /* FDM_AUTOCLOSE closes it */

  if (s->wl_seat != NULL) {
    if (wl_seat_get_version(s->wl_seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
      wl_seat_release(s->wl_seat);
    else
      wl_seat_destroy(s->wl_seat);
  }

  free(s->name);
}

static int32_t handle_key_repeat_ready(struct fdm *fdm, void *data) {
  (void)fdm;
  struct seat *seat = data;
  uint64_t expcount = 0;

  ssize_t n = read(seat->kbd.repeat.fd, &expcount, sizeof expcount);
  if (n != sizeof expcount) {
    if (n == -1 && errno != EAGAIN)
      LOG_WARN("failed to read repeat timer: %s", strerror(errno));
    return 0;
  }

  if (seat->kbd.repeat.key == 0 || seat->kbd.xkb_state == NULL)
    return 0;

  for (uint64_t i = 0; i < expcount; i++)
    seat_handle_keypress(seat, seat->kbd.repeat.key);
  return 0;
}

static void keyboard_handle_repeat(void *data, struct wl_keyboard *wl_keyboard,
                                   int32_t rate, int32_t delay) {
  (void)wl_keyboard;
  struct seat *seat = data;

  seat->kbd.repeat.rate = rate;
  seat->kbd.repeat.delay = delay;
  if (rate <= 0)
    repeat_stop(seat);
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state) {
  (void)wl_keyboard;
  (void)time;
  struct seat *seat = data;
  seat->kbd.serial = serial;

  if (seat->kbd.xkb_state == NULL) {
    seat->app->errcode = LM_ERR_KEY_EVENT_NO_KEYMAP;
    return;
  }

  xkb_keycode_t keycode = key + 8; /* evdev -> XKB */

  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    seat_handle_keypress(seat, keycode);
    /* Modifiers etc. don't repeat and must not hijack an active repeat. */
    if (xkb_keymap_key_repeats(seat->kbd.xkb_keymap, keycode))
      repeat_start(seat, keycode);
  } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    if (keycode == seat->kbd.repeat.key)
      repeat_stop(seat);
  }
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
  repeat_stop(seat);
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
      seat_keyboard_destroy(seat);
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
    seat_destroy(&app->seat);
    seat_init(&app->seat, app);
    app->seat.wl_seat =
        wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5));
    app->seat.wl_name = name;
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

/* true between a successful wl_display_prepare_read() and its matching
 * wl_display_read_events() / wl_display_cancel_read(). */
static bool display_read_prepared = false;

static int report_display_error(struct wl_display *display) {
  int err = wl_display_get_error(display);

  if (err == EPROTO) {
    const struct wl_interface *interface = NULL;
    uint32_t id = 0;
    uint32_t code = wl_display_get_protocol_error(display, &interface, &id);
    LOG_FATAL("Fatal Protocol Error: Interface %s, Object ID %u, Error Code %u",
              interface != NULL ? interface->name : "unknown", id, code);
  } else {
    LOG_FATAL("Fatal Socket Error: %s", strerror(err ? err : errno));
  }
  return err ? err : 1;
}

static int32_t handle_display_events(struct fdm *fdm, void *data) {
  (void)fdm;
  struct app *app = data;

  if (!display_read_prepared)
    return 0; /* not a reader right now; main loop will prepare again */

  display_read_prepared = false; /* consumed by read_events below */
  if (wl_display_read_events(app->display) == -1)
    return report_display_error(app->display);

  /* read_events only queues events; they still need dispatching. */
  if (wl_display_dispatch_pending(app->display) == -1)
    return report_display_error(app->display);

  return 0;
}

int main(int argc, char *argv[]) {
  int err = 0;

  struct strlist items;
  strlist_init(&items);
  struct app app = {
      .width = 640,
      .height = 480,
      .running = true,
      .items = &items,
  };
  seat_init(&app.seat, &app);

  for (int i = 0; i < argc; i++) {
    strlist_push(&items, argv[i]);
  }

  app.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (app.xkb_ctx == NULL) {
    LOG_FATAL("failed to create xkb context");
    err = 1;
    goto cleanup;
  }

  if (fdm_init(&app.fdm) != 0) {
    LOG_FATAL("failed to init fdm");
    err = 1;
    goto cleanup;
  }

  app.display = wl_display_connect(NULL);
  if (!app.display) {
    LOG_FATAL("Can't connect to a Wayland display");
    err = 1;
    goto cleanup;
  }

  app.registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(app.registry, &registry_listener, &app);
  wl_display_roundtrip(app.display); /* receive the globals */

  if (!app.compositor || !app.shm || !app.wm_base) {
    LOG_FATAL("Compositor lacks wl_compositor, wl_shm or xdg_wm_base");
    err = 1;
    goto cleanup;
  }

  app.surface = wl_compositor_create_surface(app.compositor);
  app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
  xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
  app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
  xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
  xdg_toplevel_set_title(app.toplevel, "Hello Wayland");
  wl_surface_commit(app.surface); /* empty commit -> triggers first configure */

  int32_t display_fd = wl_display_get_fd(app.display);
  if (fdm_add(&app.fdm, display_fd,
              EPOLLIN | EPOLLRDHUP | EPOLLPRI | EPOLLERR | EPOLLHUP, &app,
              handle_display_events, 0) != 0) {
    LOG_ERROR("failed to add display fd to fdm: %s",
              errno ? strerror(errno) : "");
    err = errno ? errno : 1;
    goto cleanup;
  }

  while (app.running) {
    /* Drain already-queued events until we can become a reader.
     * A failed prepare_read must NOT be paired with read/cancel. */
    while (wl_display_prepare_read(app.display) != 0) {
      if (wl_display_dispatch_pending(app.display) == -1) {
        err = report_display_error(app.display);
        goto cleanup;
      }
    }
    display_read_prepared = true;

    if (!app.running) { /* a dispatched event may have asked us to quit */
      wl_display_cancel_read(app.display);
      display_read_prepared = false;
      break;
    }

    if (wl_display_flush(app.display) == -1 && errno != EAGAIN) {
      wl_display_cancel_read(app.display);
      display_read_prepared = false;
      err = report_display_error(app.display);
      goto cleanup;
    }

    int wait_ret = fdm_wait(&app.fdm);
    int wait_errno = errno;

    /* If the display fd didn't fire, we are still a prepared reader.
     * Cancel so the next iteration's prepare_read stays balanced. */
    if (display_read_prepared) {
      wl_display_cancel_read(app.display);
      display_read_prepared = false;
    }

    if (wait_ret != 0) {
      LOG_ERROR("fdm error: %s", wait_errno ? strerror(wait_errno) : "");
      err = wait_errno ? wait_errno : 1;
      goto cleanup;
    }

    if (app.errcode != 0) {
      LOG_ERROR("app error: %s", lm10_strerror(app.errcode));
      err = app.errcode;
      goto cleanup;
    }
  }

cleanup:
  seat_destroy(&app.seat);
  xkb_context_unref(app.xkb_ctx);

  xdg_toplevel_destroy(app.toplevel);
  xdg_surface_destroy(app.xdg_surface);
  wl_surface_destroy(app.surface);
  xdg_wm_base_destroy(app.wm_base);
  wl_shm_destroy(app.shm);
  wl_compositor_destroy(app.compositor);
  wl_registry_destroy(app.registry);

  wl_display_disconnect(app.display);
  strlist_free(app.items);
  fdm_destroy(&app.fdm);
  return err == 0 ? 0 : 1;
}
