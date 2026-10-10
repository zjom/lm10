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
#include <xdg-activation-v1-client-protocol.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon-names.h>

#include "log.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static void wm_base_ping(void* data, struct xdg_wm_base* wm, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

/* ---- xdg activation token listeners ---- */

static void token_done(void* data,
                       struct xdg_activation_token_v1* token,
                       const char* name) {
  (void)token;
  char** xdg_activation_token = data;
  *xdg_activation_token = strdup(name);
}

static const struct xdg_activation_token_v1_listener token_listener = {
    .done = token_done};

void get_xdg_activation_token(struct seat* seat,
                              const char* app_id,
                              char** xdg_activation_token) {
  struct wayl* wayl = seat->wayl;
  if (wayl->xdg_activation_v1 == NULL) {
    LOG_WARN("no xdg_activation_v1 manager");
    return;
  }
  struct xdg_activation_token_v1* token =
      xdg_activation_v1_get_activation_token(wayl->xdg_activation_v1);

  xdg_activation_token_v1_set_serial(token, seat->kbd.serial, seat->wl_seat);
  xdg_activation_token_v1_set_surface(token, wayl->surface);

  if (app_id != NULL)
    xdg_activation_token_v1_set_app_id(token, app_id);

  xdg_activation_token_v1_add_listener(token, &token_listener,
                                       xdg_activation_token);
  xdg_activation_token_v1_commit(token);

  if (wl_display_roundtrip_queue(wayl->display,
                                 wayl->xdg_activation_v1_queue) == -1) {
    LOG_WARN("failed to get xdg activation token");
  }

  xdg_activation_token_v1_destroy(token);
}

/* ---- scale ---- */

static void set_scale(struct wayl* w, uint32_t scale120) {
  if (scale120 == 0 || scale120 == w->scale120)
    return;
  w->scale120 = scale120;
  w->listener->scale(w->data, scale120);
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
      wayl->listener->size(wayl->data, (int)w, (int)h, &sw, &sh);
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
  wayl->listener->configure(wayl->data, wayl->width, wayl->height);
}

static void layer_surface_closed(void* data,
                                 struct zwlr_layer_surface_v1* surface) {
  struct wayl* wayl = data;
  (void)surface;
  wayl->listener->close(wayl->data);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* ---- keyboard ---- */

static void seat_emit_key(struct seat* seat, xkb_keycode_t keycode) {
  static const struct {
    const char* name;
    uint32_t mod;
  } mod_names[] = {
      {XKB_MOD_NAME_CTRL, KB_MOD_CTRL},
      {XKB_MOD_NAME_SHIFT, KB_MOD_SHIFT},
      {XKB_MOD_NAME_ALT, KB_MOD_ALT},
      {XKB_MOD_NAME_LOGO, KB_MOD_SUPER},
  };

  struct xkb_state* st = seat->kbd.xkb_state;
  uint32_t mods = 0;
  for (size_t i = 0; i < sizeof mod_names / sizeof mod_names[0]; i++) {
    if (xkb_state_mod_name_is_active(st, mod_names[i].name,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
      mods |= mod_names[i].mod;
  }

  struct wayl* w = seat->wayl;
  struct kb_result r =
      kb_get_action(w->kb, mods, xkb_state_key_get_one_sym(st, keycode));
  if (r.action == KB_ACTION_NONE && r.utf8[0] == '\0')
    return;
  w->listener->key(w->data, &r);
}

static struct timespec ns_to_timespec(int64_t ns) {
  return (struct timespec){.tv_sec = ns / 1000000000,
                           .tv_nsec = ns % 1000000000};
}

static void repeat_stop(struct seat* seat) {
  struct repeat* r = &seat->kbd.repeat;
  r->key = 0;
  if (r->fd < 0)
    return;

  struct itimerspec ts = {0}; /* all zero = disarm */
  if (timerfd_settime(r->fd, 0, &ts, NULL) == -1)
    LOG_WARN("failed to disarm repeat timer: %s", strerror(errno));
}

static int32_t handle_key_repeat_ready(struct fdm* fdm, void* data) {
  (void)fdm;
  struct seat* seat = data;
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
    seat_emit_key(seat, seat->kbd.repeat.key);
  return 0;
}

static bool repeat_ensure_fd(struct seat* seat) {
  struct repeat* r = &seat->kbd.repeat;
  if (r->fd >= 0)
    return true;

  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) {
    LOG_WARN("failed to create timerfd: %s", strerror(errno));
    return false;
  }
  if (fdm_add(seat->wayl->fdm, fd, EPOLLIN, seat, handle_key_repeat_ready,
              FDM_AUTOCLOSE) != 0) {
    LOG_WARN("failed to add timerfd to fdm");
    close(fd);
    return false;
  }
  r->fd = fd;
  return true;
}

static void repeat_start(struct seat* seat, xkb_keycode_t keycode) {
  struct repeat* r = &seat->kbd.repeat;
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

static void keyboard_handle_repeat(void* data,
                                   struct wl_keyboard* wl_keyboard,
                                   int32_t rate,
                                   int32_t delay) {
  (void)wl_keyboard;
  struct seat* seat = data;

  seat->kbd.repeat.rate = rate;
  seat->kbd.repeat.delay = delay;
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
  seat->kbd.serial = serial;

  if (seat->kbd.xkb_state == NULL) {
    seat->wayl->err = LM_ERR_KEY_EVENT_NO_KEYMAP;
    return;
  }

  xkb_keycode_t keycode = key + 8; /* evdev -> XKB */

  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    seat_emit_key(seat, keycode);
    /* Modifiers etc. don't repeat and must not hijack an active repeat. */
    if (xkb_keymap_key_repeats(seat->kbd.xkb_keymap, keycode))
      repeat_start(seat, keycode);
  } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    if (keycode == seat->kbd.repeat.key)
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

  xkb_keymap_unref(seat->kbd.xkb_keymap);
  seat->kbd.xkb_keymap = NULL;
  xkb_state_unref(seat->kbd.xkb_state);
  seat->kbd.xkb_state = NULL;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    seat->wayl->err = LM_ERR_UNKNOWN_KEYMAP_FORMAT;
    close(fd);
    return;
  }

  char* buf = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (buf == MAP_FAILED) {
    seat->wayl->err = LM_ERR_MMAP;
    close(fd);
    return;
  }
  struct xkb_keymap* keymap = xkb_keymap_new_from_string(
      seat->wayl->xkb_ctx, buf, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(buf, size);
  close(fd);

  if (keymap == NULL) {
    seat->wayl->err = LM_ERR_KEYMAP_COMPILE_FAILED;
    return;
  }
  struct xkb_state* xkb_state = xkb_state_new(keymap);
  if (xkb_state == NULL) {
    xkb_keymap_unref(keymap);
    seat->wayl->err = LM_ERR_FAILED_CREATE_STATE_FROM_KEYMAP;
    return;
  }
  seat->kbd.xkb_keymap = keymap;
  seat->kbd.xkb_state = xkb_state;
}

static void keyboard_handle_enter(void* data,
                                  struct wl_keyboard* wl_keyboard,
                                  uint32_t serial,
                                  struct wl_surface* surface,
                                  struct wl_array* keys) {
  struct seat* seat = data;
  seat->kbd.serial = serial;
  (void)wl_keyboard;
  (void)surface;
  (void)keys;
}

static void keyboard_handle_leave(void* data,
                                  struct wl_keyboard* wl_keyboard,
                                  uint32_t serial,
                                  struct wl_surface* surface) {
  struct seat* seat = data;
  seat->kbd.serial = 0;
  repeat_stop(seat);
  (void)wl_keyboard;
  (void)serial;
  (void)surface;
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
  if (seat->kbd.xkb_state == NULL) {
    seat->wayl->err = LM_ERR_KEY_EVENT_NO_KEYMAP;
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

/* ---- seat ---- */

static void seat_init(struct seat* s, struct wayl* w) {
  *s = (struct seat){
      .wayl = w,
      .kbd.repeat = {.fd = -1},
  };
}

/* Tear down the wl_keyboard and its xkb state. The repeat timerfd is kept
 * (just disarmed) so it can be reused if a keyboard comes back. */
static void seat_keyboard_destroy(struct seat* s) {
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

static void seat_destroy(struct seat* s) {
  seat_keyboard_destroy(s);

  if (s->kbd.repeat.fd >= 0)
    fdm_remove(s->wayl->fdm, s->kbd.repeat.fd); /* FDM_AUTOCLOSE closes it */

  if (s->wl_seat != NULL) {
    if (wl_seat_get_version(s->wl_seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
      wl_seat_release(s->wl_seat);
    else
      wl_seat_destroy(s->wl_seat);
  }

  free(s->name);
  seat_init(s, s->wayl);
}

static void seat_handle_name(void* data,
                             struct wl_seat* wl_seat,
                             const char* name) {
  (void)wl_seat;
  struct seat* seat = data;
  free(seat->name);
  seat->name = strdup(name);
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
  } else {
    if (seat->wl_kbd != NULL) {
      seat_keyboard_destroy(seat);
    }
  }
}

static const struct wl_seat_listener seat_listener = {
    .name = seat_handle_name,
    .capabilities = seat_handle_capabilities};

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
  }

  else if (strcmp(iface, wl_shm_interface.name) == 0) {
    w->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  }

  else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
    w->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(w->wm_base, &wm_base_listener, w);
  }

  else if (strcmp(iface, wl_seat_interface.name) == 0) {
    seat_destroy(&w->seat);
    w->seat.wl_seat =
        wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5));
    w->seat.wl_name = name;
    wl_seat_add_listener(w->seat.wl_seat, &seat_listener, &w->seat);
  }

  else if (strcmp(iface, xdg_activation_v1_interface.name) == 0) {
    w->xdg_activation_v1 =
        wl_registry_bind(reg, name, &xdg_activation_v1_interface, 1);
    wl_proxy_set_queue((struct wl_proxy*)w->xdg_activation_v1,
                       w->xdg_activation_v1_queue);
  }

  else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
    w->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
  }

  else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
    w->fractional_scale_manager = wl_registry_bind(
        reg, name, &wp_fractional_scale_manager_v1_interface, 1);
  }

  else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
    w->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
                                      MIN(version, 1));
  }
}

static void registry_global_remove(void* data,
                                   struct wl_registry* reg,
                                   uint32_t name) {
  (void)data;
  (void)reg;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- display fd ---- */

static int report_display_error(struct wl_display* display) {
  int err = wl_display_get_error(display);

  if (err == EPROTO) {
    const struct wl_interface* interface = NULL;
    uint32_t id = 0;
    uint32_t code = wl_display_get_protocol_error(display, &interface, &id);
    LOG_FATAL("Fatal Protocol Error: Interface %s, Object ID %u, Error Code %u",
              interface != NULL ? interface->name : "unknown", id, code);
  } else {
    LOG_FATAL("Fatal Socket Error: %s", strerror(err ? err : errno));
  }
  return err ? err : 1;
}

static int32_t handle_display_events(struct fdm* fdm, void* data) {
  (void)fdm;
  struct wayl* w = data;

  if (!w->read_prepared)
    return 0; /* not a reader right now; main loop will prepare again */

  w->read_prepared = false; /* consumed by wayl_finish */
  if (wl_display_read_events(w->display) == -1)
    return report_display_error(w->display);

  if (wl_display_dispatch_pending(w->display) == -1)
    return report_display_error(w->display);

  return 0;
}

/* ---- public ---- */

int wayl_init(struct wayl* w,
              struct fdm* fdm,
              struct kb_manager* kb,
              const struct wayl_listener* listener,
              void* data,
              const char* title) {
  *w = (struct wayl){
      .fdm = fdm,
      .kb = kb,
      .display_fd = -1,
      .width = 640,
      .height = 480,
      .scale120 = 120,
      .listener = listener,
      .data = data,
  };
  seat_init(&w->seat, w);

  w->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (w->xkb_ctx == NULL) {
    LOG_FATAL("failed to create xkb context");
    return -1;
  }

  w->display = wl_display_connect(NULL);
  if (!w->display) {
    LOG_FATAL("Can't connect to a Wayland display");
    return -1;
  }

  w->xdg_activation_v1_queue = wl_display_create_queue(w->display);
  if (w->xdg_activation_v1_queue == NULL) {
    LOG_ERROR("failed to create wl xdg_activation_v1 queue");
    return -1;
  }

  w->registry = wl_display_get_registry(w->display);
  wl_registry_add_listener(w->registry, &registry_listener, w);
  wl_display_roundtrip(w->display); /* receive the globals */

  if (!w->compositor) {
    LOG_FATAL("Compositor lacks wl_compositor");
    return -1;
  }

  if (!w->shm) {
    LOG_FATAL("Compositor lacks wl_shm");
    return -1;
  }

  if (!w->wm_base) {
    LOG_FATAL("Compositor lacks xdg_wm_base");
    return -1;
  }

  if (!w->layer_shell) {
    LOG_FATAL("Compositor zwlr_layer_shell");
    return -1;
  }

  w->surface = wl_compositor_create_surface(w->compositor);
  if (!w->surface) {
    LOG_FATAL("failed to create surface");
    return -1;
  }
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
      title);
  if (!w->layer_surface) {
    LOG_FATAL("failed to create layer surface");
    return -1;
  }

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
  wl_surface_commit(w->surface); /* empty commit -> triggers first configure */

  int fd = wl_display_get_fd(w->display);
  if (fdm_add(fdm, fd, EPOLLIN | EPOLLRDHUP | EPOLLPRI | EPOLLERR | EPOLLHUP, w,
              handle_display_events, 0) != 0) {
    LOG_FATAL("failed to add display fd to fdm: %s",
              errno ? strerror(errno) : "");
    return -1;
  }
  w->display_fd = fd;

  return 0;
}

void wayl_destroy(struct wayl* w) {
  if (w->read_prepared) {
    wl_display_cancel_read(w->display);
    w->read_prepared = false;
  }

  seat_destroy(&w->seat);
  xkb_context_unref(w->xkb_ctx);

  if (w->display_fd >= 0)
    fdm_remove(w->fdm, w->display_fd); /* no AUTOCLOSE: libwayland owns it */
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
  if (w->wm_base)
    xdg_wm_base_destroy(w->wm_base);
  if (w->shm)
    wl_shm_destroy(w->shm);
  if (w->compositor)
    wl_compositor_destroy(w->compositor);
  if (w->registry)
    wl_registry_destroy(w->registry);
  if (w->display)
    wl_display_disconnect(w->display);

  *w = (struct wayl){.display_fd = -1};
}

int wayl_prepare(struct wayl* w) {
  /* Drain already-queued events until we can become a reader.
   * A failed prepare_read must NOT be paired with read/cancel. */
  while (wl_display_prepare_read(w->display) != 0) {
    if (wl_display_dispatch_pending(w->display) == -1)
      return report_display_error(w->display);
  }
  w->read_prepared = true;

  if (wl_display_flush(w->display) == -1 && errno != EAGAIN) {
    wayl_finish(w);
    return report_display_error(w->display);
  }
  return 0;
}

void wayl_finish(struct wayl* w) {
  /* If the display fd didn't fire, we are still a prepared reader.
   * Cancel so the next prepare_read stays balanced. */
  if (w->read_prepared) {
    wl_display_cancel_read(w->display);
    w->read_prepared = false;
  }
}
