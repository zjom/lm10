#ifndef LM_WAYL_H
#define LM_WAYL_H

#include "fdm.h"
#include "kb.h"

#include <stdint.h>

#include <wayland-client.h>

/* The Wayland connection: a wlr-layer-shell overlay surface, centered on
 * the output, that takes the keyboard from every seat. */
struct wayl;

/* How wayl talks to the app. All callbacks run from inside event dispatch. */
struct wayl_listener {
  /* Once, before the first configure: picks the surface size given the
   * output's logical size. The compositor centers the surface. */
  void (*size)(void* data, int out_w, int out_h, int* w, int* h);
  /* After ack_configure; the app must commit a buffer in response. */
  void (*configure)(void* data, int w, int h);
  void (*close)(void* data);
  /* The compositor's preferred scale for the surface changed, in 120ths
   * (120 = 1x). Sizes passed to size/configure stay logical. */
  void (*scale)(void* data, uint32_t scale120);
  /* Key press, and again for each repeat, already resolved by the kb
   * manager. Ignored keys are not reported. */
  void (*key)(void* data, const struct kb_result* key);
};

/* Connects, binds the globals, creates the layer surface (namespace is its
 * layer-shell namespace) and registers the display fd with fdm. kb resolves
 * keys and must outlive the wayl. Returns NULL on failure (logged). */
struct wayl* wayl_new(struct fdm* fdm,
                      struct kb_manager* kb,
                      const struct wayl_listener* listener,
                      void* data,
                      const char* namespace);
void wayl_destroy(struct wayl* w);

struct wl_shm* wayl_shm(const struct wayl* w);
struct wl_surface* wayl_surface(const struct wayl* w);

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
