#include "wayl.h"

#include <assert.h>
#include <errno.h>
#include <fractional-scale-v1-client-protocol.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <viewporter-client-protocol.h>
#include <wlr-layer-shell-unstable-v1-client-protocol.h>
#include <xdg-activation-v1-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "log.h"
#include "util.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct repeat {
  int32_t rate;       // repeats per second; 0 = disabled
  int32_t delay;      // ms before the first repeat
  int fd;             // timerfd, -1 if not created yet
  xkb_keycode_t key;  // XKB keycode (evdev + 8) being repeated, 0 = none
};

struct seat {
  struct wayl* wayl;
  uint32_t wl_name;  // registry name, to notice the seat going away
  struct wl_seat* wl_seat;
  struct wl_keyboard* wl_kbd;
  uint32_t serial;  // of the last key event, for activation tokens
  struct xkb_keymap* xkb_keymap;
  struct xkb_state* xkb_state;
  struct repeat repeat;
};

/* ---- activation tokens ---- */

static void token_done(void* data,
                       struct xdg_activation_token_v1* token,
                       const char* name) {
  (void)token;
  char** out = data;
  free(*out);
  *out = strdup(name);
}

static const struct xdg_activation_token_v1_listener token_listener = {
    .done = token_done,
};

char* wayl_activation_token(struct wayl* w) {
  assert(!w->read_prepared);
  if (w->xdg_activation_v1 == NULL) {
    LOG_DEBUG("no xdg_activation_v1; not handing out a token");
    return NULL;
  }
  struct xdg_activation_token_v1* token =
      xdg_activation_v1_get_activation_token(w->xdg_activation_v1);
  struct seat* seat = w->last_input;
  if (seat != NULL && seat->wl_seat != NULL)
    xdg_activation_token_v1_set_serial(token, seat->serial, seat->wl_seat);
  xdg_activation_token_v1_set_surface(token, w->surface);

  char* name = NULL;
  xdg_activation_token_v1_add_listener(token, &token_listener, &name);
  xdg_activation_token_v1_commit(token);
  if (wl_display_roundtrip_queue(w->display, w->xdg_activation_v1_queue) == -1)
    LOG_WARN("failed to get an activation token");
  xdg_activation_token_v1_destroy(token);
  return name;
}

/* ---- scale ---- */

static void set_scale(struct wayl* w, uint32_t scale120) {
  if (scale120 == 0 || scale120 == w->scale120)
    return;
  w->scale120 = scale120;
  app_scale(w->app, scale120);
}

static void fractional_scale_preferred(void* data,
                                       struct wp_fractional_scale_v1* fs,
                                       uint32_t scale) {
  (void)fs;
  set_scale(data, scale);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener =
    {
        .preferred_scale = fractional_scale_preferred,
};

static void surface_enter(void* data,
                          struct wl_surface* surface,
                          struct wl_output* output) {
  (void)data;
  (void)surface;
  (void)output;
}

static void surface_leave(void* data,
                          struct wl_surface* surface,
                          struct wl_output* output) {
  (void)data;
  (void)surface;
  (void)output;
}

static void surface_preferred_buffer_scale(void* data,
                                           struct wl_surface* surface,
                                           int32_t factor) {
  (void)surface;
  struct wayl* w = data;
  /* The fractional scale is more precise; this one is rounded up. */
  if (w->fractional_scale == NULL && factor > 0)
    set_scale(w, (uint32_t)factor * 120);
}

static void surface_preferred_buffer_transform(void* data,
                                               struct wl_surface* surface,
                                               uint32_t transform) {
  (void)data;
  (void)surface;
  (void)transform;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_buffer_scale,
    .preferred_buffer_transform = surface_preferred_buffer_transform,
};

/* ---- layer surface listeners ---- */

static void layer_surface_configure(void* data,
                                    struct zwlr_layer_surface_v1* surface,
                                    uint32_t serial,
                                    uint32_t w,
                                    uint32_t h) {
  struct wayl* wayl = data;

  if (!wayl->sized) {
    /* Anchored to all edges, so w x h is the output's logical size. Let the
     * app pick its size; with no anchors the compositor centers us. Commit
     * without a buffer and wait for the configure that carries our size. */
    if (w > 0 && h > 0) {
      int sw = wayl->width, sh = wayl->height;
      app_size(wayl->app, (int)w, (int)h, &sw, &sh);
      wayl->width = sw;
      wayl->height = sh;
    }
    wayl->sized = true;
    zwlr_layer_surface_v1_set_anchor(surface, 0);
    zwlr_layer_surface_v1_set_size(surface, wayl->width, wayl->height);
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    wl_surface_commit(wayl->surface);
    return;
  }

  if (w > 0 && h > 0) {
    wayl->height = h;
    wayl->width = w;
  }
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  /* Double-buffered: lands with the buffer the app commits in response. */
  if (wayl->viewport)
    wp_viewport_set_destination(wayl->viewport, wayl->width, wayl->height);
  app_configure(wayl->app, wayl->width, wayl->height);
}

static void layer_surface_closed(void* data,
                                 struct zwlr_layer_surface_v1* surface) {
  struct wayl* wayl = data;
  (void)surface;
  app_close(wayl->app);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* ---- keyboard ---- */

static void seat_emit_key(struct seat* seat, xkb_keycode_t keycode) {
  struct wayl* w = seat->wayl;
  struct kb_result r = kb_resolve(seat->xkb_state, keycode);
  if (r.n_keys > 0 || r.text[0] != '\0')
    app_key(w->app, &r);
}

static struct timespec ns_to_timespec(int64_t ns) {
  return (struct timespec){.tv_sec = ns / 1000000000,
                           .tv_nsec = ns % 1000000000};
}

static void repeat_stop(struct seat* seat) {
  struct repeat* r = &seat->repeat;
  r->key = 0;
  if (r->fd < 0)
    return;

  struct itimerspec ts = {0};  // all zero = disarm
  if (timerfd_settime(r->fd, 0, &ts, NULL) == -1)
    LOG_WARN("failed to disarm repeat timer: %s", strerror(errno));
}

static void handle_key_repeat_ready(struct fdm* fdm, void* data) {
  (void)fdm;
  struct seat* seat = data;
  uint64_t expcount = 0;

  ssize_t n = read(seat->repeat.fd, &expcount, sizeof expcount);
  if (n != sizeof expcount) {
    if (n == -1 && errno != EAGAIN)
      LOG_WARN("failed to read repeat timer: %s", strerror(errno));
    return;
  }

  if (seat->repeat.key == 0 || seat->xkb_state == NULL)
    return;

  /* One repeat however many ticks were missed: replaying them after the
   * loop was busy would fire a burst of keys, possibly past the release
   * that is already queued. */
  seat_emit_key(seat, seat->repeat.key);
}

static bool repeat_ensure_fd(struct seat* seat) {
  struct repeat* r = &seat->repeat;
  if (r->fd >= 0)
    return true;

  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) {
    LOG_WARN("failed to create timerfd: %s", strerror(errno));
    return false;
  }
  if (fdm_add(seat->wayl->fdm, fd, EPOLLIN, handle_key_repeat_ready, seat,
              FDM_AUTOCLOSE) != 0) {
    LOG_WARN("failed to add timerfd to fdm");
    close(fd);
    return false;
  }
  r->fd = fd;
  return true;
}

static void repeat_start(struct seat* seat, xkb_keycode_t keycode) {
  struct repeat* r = &seat->repeat;
  if (r->rate <= 0)  // compositor disabled key repeat
    return;
  if (!repeat_ensure_fd(seat))
    return;

  int64_t delay_ns = (int64_t)r->delay * 1000000;
  if (delay_ns <= 0)
    delay_ns = 1;  // all-zero it_value would disarm the timer

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

static void keyboard_handle_repeat(void* data,
                                   struct wl_keyboard* wl_keyboard,
                                   int32_t rate,
                                   int32_t delay) {
  (void)wl_keyboard;
  struct seat* seat = data;

  seat->repeat.rate = rate;
  seat->repeat.delay = delay;
  if (rate <= 0)
    repeat_stop(seat);
}

static void keyboard_handle_key(void* data,
                                struct wl_keyboard* wl_keyboard,
                                uint32_t serial,
                                uint32_t time,
                                uint32_t key,
                                uint32_t state) {
  (void)wl_keyboard;
  (void)time;
  struct seat* seat = data;

  /* A keyboard may come without a keymap (or with one we can't use); its
   * keys can't be resolved, so they are ignored. */
  if (seat->xkb_state == NULL)
    return;

  xkb_keycode_t keycode = key + 8;  // evdev -> XKB

  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    seat->serial = serial;
    seat->wayl->last_input = seat;
    seat_emit_key(seat, keycode);
    /* Modifiers etc. don't repeat and must not hijack an active repeat. */
    if (xkb_keymap_key_repeats(seat->xkb_keymap, keycode))
      repeat_start(seat, keycode);
  } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    if (keycode == seat->repeat.key)
      repeat_stop(seat);
  }
}

static void keyboard_handle_keymap(void* data,
                                   struct wl_keyboard* wl_keyboard,
                                   uint32_t format,
                                   int32_t fd,
                                   uint32_t size) {
  (void)wl_keyboard;
  struct seat* seat = data;

  xkb_keymap_unref(seat->xkb_keymap);
  seat->xkb_keymap = NULL;
  xkb_state_unref(seat->xkb_state);
  seat->xkb_state = NULL;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    LOG_WARN("keyboard has no xkb keymap (format %u); ignoring its keys",
             format);
    close(fd);
    return;
  }

  char* buf = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (buf == MAP_FAILED)
    die("keymap mmap: %s", strerror(errno));
  struct xkb_keymap* keymap = xkb_keymap_new_from_string(
      seat->wayl->xkb_ctx, buf, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(buf, size);
  close(fd);

  if (keymap == NULL)
    die("failed to compile the keymap");
  struct xkb_state* xkb_state = xkb_state_new(keymap);
  if (xkb_state == NULL)
    die("failed to create an xkb state for the keymap");
  seat->xkb_keymap = keymap;
  seat->xkb_state = xkb_state;
}

static void keyboard_handle_enter(void* data,
                                  struct wl_keyboard* wl_keyboard,
                                  uint32_t serial,
                                  struct wl_surface* surface,
                                  struct wl_array* keys) {
  (void)data;
  (void)wl_keyboard;
  (void)serial;
  (void)surface;
  (void)keys;
}

static void keyboard_handle_leave(void* data,
                                  struct wl_keyboard* wl_keyboard,
                                  uint32_t serial,
                                  struct wl_surface* surface) {
  (void)wl_keyboard;
  (void)serial;
  (void)surface;
  repeat_stop(data);
}

static void keyboard_handle_modifiers(void* data,
                                      struct wl_keyboard* wl_keyboard,
                                      uint32_t serial,
                                      uint32_t mods_depressed,
                                      uint32_t mods_latched,
                                      uint32_t mods_locked,
                                      uint32_t group) {
  (void)serial;
  (void)wl_keyboard;
  struct seat* seat = data;
  if (seat->xkb_state == NULL)
    return;  // no keymap: see keyboard_handle_key
  xkb_state_update_mask(seat->xkb_state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .repeat_info = keyboard_handle_repeat,
    .key = keyboard_handle_key,
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .modifiers = keyboard_handle_modifiers,
};

/* ---- seats ---- */

/* Tears down the seat's wl_keyboard and its xkb state. The repeat timerfd
 * is kept (just disarmed) so it can be reused if a keyboard comes back. */
static void seat_keyboard_destroy(struct seat* s) {
  repeat_stop(s);

  if (s->wl_kbd != NULL) {
    if (wl_keyboard_get_version(s->wl_kbd) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
      wl_keyboard_release(s->wl_kbd);
    else
      wl_keyboard_destroy(s->wl_kbd);
    s->wl_kbd = NULL;
  }

  xkb_state_unref(s->xkb_state);
  s->xkb_state = NULL;
  xkb_keymap_unref(s->xkb_keymap);
  s->xkb_keymap = NULL;
}

/* Releases everything the seat holds and frees it; it must already be out
 * of w->seats. */
static void seat_destroy(struct seat* s) {
  seat_keyboard_destroy(s);
  if (s->repeat.fd >= 0)
    fdm_remove(s->wayl->fdm, s->repeat.fd);  // FDM_AUTOCLOSE closes it
  if (s->wl_seat != NULL) {
    if (wl_seat_get_version(s->wl_seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
      wl_seat_release(s->wl_seat);
    else
      wl_seat_destroy(s->wl_seat);
  }
  if (s->wayl->last_input == s)
    s->wayl->last_input = NULL;
  free(s);
}

static void seat_handle_name(void* data,
                             struct wl_seat* wl_seat,
                             const char* name) {
  (void)data;
  (void)wl_seat;
  (void)name;
}

static void seat_handle_capabilities(void* data,
                                     struct wl_seat* wl_seat,
                                     enum wl_seat_capability caps) {
  struct seat* seat = data;
  assert(seat->wl_seat == wl_seat);

  if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
    if (seat->wl_kbd == NULL) {
      seat->wl_kbd = wl_seat_get_keyboard(wl_seat);
      wl_keyboard_add_listener(seat->wl_kbd, &keyboard_listener, seat);
    }
  } else if (seat->wl_kbd != NULL) {
    seat_keyboard_destroy(seat);
  }
}

static const struct wl_seat_listener seat_listener = {
    .name = seat_handle_name,
    .capabilities = seat_handle_capabilities,
};

static void seat_add(struct wayl* w,
                     struct wl_registry* reg,
                     uint32_t name,
                     uint32_t version) {
  struct seat* s = xmalloc(sizeof *s);
  LIST_PUSH(&w->seats, s);
  *s = (struct seat){.wayl = w, .wl_name = name, .repeat = {.fd = -1}};
  s->wl_seat = wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5));
  wl_seat_add_listener(s->wl_seat, &seat_listener, s);
}

/* ---- registry ---- */

static void registry_global(void* data,
                            struct wl_registry* reg,
                            uint32_t name,
                            const char* iface,
                            uint32_t version) {
  struct wayl* w = data;
  if (strcmp(iface, wl_compositor_interface.name) == 0) {
    /* v6 for wl_surface.preferred_buffer_scale */
    w->compositor =
        wl_registry_bind(reg, name, &wl_compositor_interface, MIN(version, 6));
  } else if (strcmp(iface, wl_shm_interface.name) == 0) {
    w->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  } else if (strcmp(iface, wl_seat_interface.name) == 0) {
    seat_add(w, reg, name, version);
  } else if (strcmp(iface, xdg_activation_v1_interface.name) == 0) {
    w->xdg_activation_v1 =
        wl_registry_bind(reg, name, &xdg_activation_v1_interface, 1);
    wl_proxy_set_queue((struct wl_proxy*)w->xdg_activation_v1,
                       w->xdg_activation_v1_queue);
  } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
    w->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
  } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) ==
             0) {
    w->fractional_scale_manager = wl_registry_bind(
        reg, name, &wp_fractional_scale_manager_v1_interface, 1);
  } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
    w->layer_shell =
        wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
  }
}

static void registry_global_remove(void* data,
                                   struct wl_registry* reg,
                                   uint32_t name) {
  (void)reg;
  struct wayl* w = data;
  /* Only seats come and go; the other globals we use are for good. */
  for (size_t i = 0; i < w->seats.len; i++) {
    struct seat* s = w->seats.ptr[i];
    if (s->wl_name == name) {
      LIST_SWAP_REMOVE(&w->seats, i);
      seat_destroy(s);
      return;
    }
  }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- display fd ---- */

/* Dies saying why the display failed. */
_Noreturn static void display_error(struct wl_display* display) {
  int err = wl_display_get_error(display);

  if (err == EPROTO) {
    const struct wl_interface* interface = NULL;
    uint32_t id = 0;
    uint32_t code = wl_display_get_protocol_error(display, &interface, &id);
    die("protocol error: interface %s, object %u, code %u",
        interface != NULL ? interface->name : "unknown", id, code);
  }
  die("lost the display connection: %s", strerror(err ? err : errno));
}

static void handle_display_events(struct fdm* fdm, void* data) {
  (void)fdm;
  struct wayl* w = data;

  if (!w->read_prepared)
    return;  // not a reader right now; main loop will prepare again

  w->read_prepared = false;  // consumed: wayl_finish has nothing to cancel
  if (wl_display_read_events(w->display) == -1 ||
      wl_display_dispatch_pending(w->display) == -1)
    display_error(w->display);
}

/* ---- public ---- */

/* Fails wayl_new: logs why, tears down what was set up. */
static struct wayl* fail(struct wayl* w, const char* why) {
  LOG_FATAL("%s", why);
  wayl_destroy(w);
  return NULL;
}

struct wayl* wayl_new(struct fdm* fdm, struct app* app, const char* namespace) {
  struct wayl* w = xmalloc(sizeof *w);
  *w = (struct wayl){
      .seats = LIST_INIT,
      .fdm = fdm,
      .display_fd = -1,
      .width = 640,
      .height = 480,
      .scale120 = 120,
      .app = app,
  };

  w->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (w->xkb_ctx == NULL)
    return fail(w, "failed to create an xkb context");

  w->display = wl_display_connect(NULL);
  if (w->display == NULL)
    return fail(w, "can't connect to a Wayland display");

  w->xdg_activation_v1_queue = wl_display_create_queue(w->display);
  if (w->xdg_activation_v1_queue == NULL)
    return fail(w, "failed to create an event queue");

  w->registry = wl_display_get_registry(w->display);
  wl_registry_add_listener(w->registry, &registry_listener, w);
  if (wl_display_roundtrip(w->display) == -1)  // receive the globals
    display_error(w->display);

  if (w->compositor == NULL)
    return fail(w, "the compositor lacks wl_compositor");
  if (w->shm == NULL)
    return fail(w, "the compositor lacks wl_shm");
  if (w->layer_shell == NULL)
    return fail(w,
                "the compositor lacks zwlr_layer_shell_v1: lm10 needs one "
                "with wlr-layer-shell, such as sway, Hyprland, KDE Plasma, "
                "niri or river");

  w->surface = wl_compositor_create_surface(w->compositor);
  if (w->surface == NULL)
    return fail(w, "failed to create a surface");
  wl_surface_add_listener(w->surface, &surface_listener, w);

  if (w->viewporter && w->fractional_scale_manager) {
    w->viewport = wp_viewporter_get_viewport(w->viewporter, w->surface);
    w->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
        w->fractional_scale_manager, w->surface);
    wp_fractional_scale_v1_add_listener(w->fractional_scale,
                                        &fractional_scale_listener, w);
  }

  w->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      w->layer_shell, w->surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      namespace);
  if (w->layer_surface == NULL)
    return fail(w, "failed to create a layer surface");

  zwlr_layer_surface_v1_add_listener(w->layer_surface, &layer_surface_listener,
                                     w);
  /* Fill the output (ignoring panels) so the first configure reports its
   * logical size; layer_surface_configure then shrinks and centers us. */
  zwlr_layer_surface_v1_set_anchor(w->layer_surface,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(w->layer_surface, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(w->layer_surface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      w->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
  wl_surface_commit(w->surface);  // empty commit -> triggers first configure

  int fd = wl_display_get_fd(w->display);
  if (fdm_add(fdm, fd, EPOLLIN | EPOLLRDHUP | EPOLLPRI | EPOLLERR | EPOLLHUP,
              handle_display_events, w, 0) != 0)
    return fail(w, "failed to watch the display fd");
  w->display_fd = fd;
  return w;
}

void wayl_destroy(struct wayl* w) {
  if (w == NULL)
    return;
  if (w->read_prepared)
    wl_display_cancel_read(w->display);

  while (w->seats.len > 0) {
    struct seat* s = w->seats.ptr[w->seats.len - 1];
    LIST_POP(&w->seats);
    seat_destroy(s);
  }
  LIST_FREE(&w->seats);
  xkb_context_unref(w->xkb_ctx);

  if (w->display_fd >= 0)
    fdm_remove(w->fdm, w->display_fd);  // no AUTOCLOSE: libwayland owns it
  if (w->xdg_activation_v1)
    xdg_activation_v1_destroy(w->xdg_activation_v1);
  if (w->xdg_activation_v1_queue)
    wl_event_queue_destroy(w->xdg_activation_v1_queue);
  if (w->layer_surface)
    zwlr_layer_surface_v1_destroy(w->layer_surface);
  if (w->layer_shell)
    zwlr_layer_shell_v1_destroy(w->layer_shell);
  if (w->fractional_scale)
    wp_fractional_scale_v1_destroy(w->fractional_scale);
  if (w->viewport)
    wp_viewport_destroy(w->viewport);
  if (w->fractional_scale_manager)
    wp_fractional_scale_manager_v1_destroy(w->fractional_scale_manager);
  if (w->viewporter)
    wp_viewporter_destroy(w->viewporter);
  if (w->surface)
    wl_surface_destroy(w->surface);
  if (w->shm)
    wl_shm_destroy(w->shm);
  if (w->compositor)
    wl_compositor_destroy(w->compositor);
  if (w->registry)
    wl_registry_destroy(w->registry);
  if (w->display)
    wl_display_disconnect(w->display);
  free(w);
}

void wayl_prepare(struct wayl* w) {
  /* Drain already-queued events until we can become a reader.
   * A failed prepare_read must NOT be paired with read/cancel. */
  while (wl_display_prepare_read(w->display) != 0) {
    if (wl_display_dispatch_pending(w->display) == -1)
      display_error(w->display);
  }
  w->read_prepared = true;

  if (wl_display_flush(w->display) == -1 && errno != EAGAIN)
    display_error(w->display);
}

void wayl_finish(struct wayl* w) {
  /* If the display fd didn't fire, we are still a prepared reader.
   * Cancel so the next prepare_read stays balanced. */
  if (w->read_prepared) {
    wl_display_cancel_read(w->display);
    w->read_prepared = false;
  }
}
