#ifndef LM_WAYL_H
#define LM_WAYL_H

#include "fdm.h"
#include "kb.h"
#include "ztd_arr.h"

#include <stdbool.h>
#include <stdint.h>

#include <wayland-client.h>

/* The app (main.c) handles these; wayl calls them from inside event
 * dispatch. */
struct app;
/* Once, before the first configure: picks the surface size given the
 * output's logical size. The compositor centers the surface. */
void app_size(struct app* app, int out_w, int out_h, int* w, int* h);
/* After ack_configure; the app must commit a buffer in response. */
void app_configure(struct app* app, int w, int h);
void app_close(struct app* app);
/* The compositor's preferred scale for the surface changed, in 120ths
 * (120 = 1x). Sizes passed to app_size and app_configure stay logical. */
void app_scale(struct app* app, uint32_t scale120);
/* Key press, and again for each repeat, resolved by kb_resolve. Keys with
 * neither a keysym nor text are not reported. */
void app_key(struct app* app, const struct kb_result* key);

struct seat;
typedef LIST(struct seat*) SeatList;

/* The Wayland connection: a wlr-layer-shell overlay surface, centered on
 * the output, that takes the keyboard from every seat. */
struct wayl {
  struct wl_display* display;
  struct wl_registry* registry;
  struct wl_compositor* compositor;
  struct wl_shm* shm;
  struct wl_surface* surface;
  struct zwlr_layer_shell_v1* layer_shell;
  struct zwlr_layer_surface_v1* layer_surface;
  struct xkb_context* xkb_ctx;
  /* Activation tokens are fetched with a roundtrip on a queue of their own,
   * so it doesn't dispatch anything else. */
  struct wl_event_queue* xdg_activation_v1_queue;
  struct xdg_activation_v1* xdg_activation_v1;
  /* Optional: with both, the surface gets fractional scales; otherwise only
   * integer ones from wl_surface.preferred_buffer_scale (wl_compositor v6). */
  struct wp_viewporter* viewporter;
  struct wp_fractional_scale_manager_v1* fractional_scale_manager;
  struct wp_viewport* viewport;
  struct wp_fractional_scale_v1* fractional_scale;
  SeatList seats;           // heap-allocated, as listeners point at them
  struct seat* last_input;  // the seat of the last key press, or NULL
  struct fdm* fdm;
  int display_fd;  // -1 until registered with fdm
  struct app* app;

  int32_t width, height;  // last configured logical size
  uint32_t scale120;      // preferred scale in 120ths

  /* false until the first configure (full output, all edges anchored) has
   * been answered with our centered size. */
  bool sized;

  /* true between a successful wl_display_prepare_read() and its matching
   * wl_display_read_events() / wl_display_cancel_read(). */
  bool read_prepared;
};

/* Connects, binds the globals, creates the layer surface (namespace is its
 * layer-shell namespace) and registers the display fd with fdm. Returns
 * NULL on failure (logged). */
struct wayl* wayl_new(struct fdm* fdm, struct app* app, const char* namespace);
void wayl_destroy(struct wayl* w);

/* Main-loop halves around fdm_wait(): wayl_prepare dispatches queued events
 * until it can become a reader, then flushes; wayl_finish cancels the read if
 * the display fd didn't fire. A display error, or a keymap that doesn't
 * compile, is fatal. */
void wayl_prepare(struct wayl* w);
void wayl_finish(struct wayl* w);

/* Asks the compositor for an xdg-activation token for whatever the last key
 * press starts, so it may take focus. Takes a roundtrip, so it must not run
 * between wayl_prepare and wayl_finish. Returns the token (free() it), or
 * NULL if there is none. */
char* wayl_activation_token(struct wayl* w);

#endif  // !LM_WAYL_H
