#ifndef LM_WAYL_H
#define LM_WAYL_H

#include "common.h"
#include "fdm.h"

#include <stdbool.h>
#include <stdint.h>

#include <wayland-client.h>
#include <wlr-layer-shell-unstable-v1-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

struct xdg_activation_v1;

struct wayl;

enum wayl_mod {
  WAYL_MOD_CTRL = 1 << 0,
  WAYL_MOD_SHIFT = 1 << 1,
  WAYL_MOD_ALT = 1 << 2,
};

/* How wayl talks to the app. All callbacks run from inside event dispatch. */
struct wayl_listener {
  /* After ack_configure; the app must commit a buffer in response. */
  void (*configure)(void *data, int w, int h);
  void (*close)(void *data);
  /* Key press, and again for each repeat. mods is a mask of wayl_mod. */
  void (*key)(void *data, xkb_keysym_t sym, uint32_t mods);
};

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

  struct wayl *wayl;

  struct wl_keyboard *wl_kbd;
  struct {
    uint32_t serial;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct repeat repeat;
  } kbd;
};

struct wayl {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct xdg_wm_base *wm_base;
  struct wl_surface *surface;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct xkb_context *xkb_ctx;
  struct wl_event_queue *xdg_activation_v1_queue;
  struct xdg_activation_v1 *xdg_activation_v1;
  struct seat seat;
  struct fdm *fdm;
  int display_fd; // -1 until registered with fdm

  int width, height; // last configured logical size

  /* true between a successful wl_display_prepare_read() and its matching
   * wl_display_read_events() / wl_display_cancel_read(). */
  bool read_prepared;

  const struct wayl_listener *listener;
  void *data;

  enum lm_err err; // sticky; checked by the main loop
};

/* Connects, binds globals, creates the toplevel and registers the display fd
 * with fdm. On failure, returns -1; call wayl_destroy either way. */
int wayl_init(struct wayl *w, struct fdm *fdm,
              const struct wayl_listener *listener, void *data,
              const char *title);
void wayl_destroy(struct wayl *w);

/* Main-loop halves around fdm_wait(): wayl_prepare dispatches queued events
 * until it can become a reader, then flushes; wayl_finish cancels the read if
 * the display fd didn't fire. wayl_prepare returns 0, or non-zero on a fatal
 * display error (already logged). */
int wayl_prepare(struct wayl *w);
void wayl_finish(struct wayl *w);

/* Synchronously grab a token from the compositor.
 * xdg_activation_token return parameter will not be modified if:
 * - there's no manager OR
 * - the roundtrip fails,
 *
 * if strdup fails, xdg_activation_token will become NULL
 */
void get_xdg_activation_token(struct seat *seat, const char *app_id,
                              char **xdg_activation_token);

#endif // !LM_WAYL_H
