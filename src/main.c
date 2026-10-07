/*
 * lm10: a Wayland picker. Lists its arguments; C-n / C-p move the selection.
 *
 * Build:
 * meson setup bld/debug && ninja -C bld/debug
 *
 * Run inside a Wayland session (GNOME, KDE, Sway, ...): ./bld/debug/lm10 a b c
 */
#include "common.h"
#include "dlist.h"
#include "ds.h"
#include "fdm.h"
#include "log.h"
#include "render.h"
#include "state.h"
#include "wayl.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct app {
  struct fdm fdm;
  struct wayl wayl;
  struct state state;   // the state
  struct render render; // presents the state
  bool running;
};

static void exec_selection(void *data) {
  struct app *app = data;
  String s = state_entry_join_argv(state_get_selection(&app->state), " ");
  LOG_INFO("user selected %s", s.data);
  string_deinit(&s);
  app->running = false;
}

/* ---- wayl callbacks ---- */

static void on_configure(void *data, int w, int h) {
  struct app *app = data;
  render_resize(&app->render, w, h);
  /* configure must be answered with a commit, even if nothing changed */
  render_request(&app->render);
}

static void on_close(void *data) {
  struct app *app = data;
  app->running = false;
}

static void on_key(void *data, xkb_keysym_t sym, uint32_t mods) {
  struct app *app = data;
  if (!(mods & WAYL_MOD_CTRL)) {
    if (sym == XKB_KEY_Return) {
      exec_selection(app);
    }
  }

  switch (sym) {
  case XKB_KEY_n:
    state_select_next(&app->state);
    break;
  case XKB_KEY_p:
    state_select_prev(&app->state);
    break;
  default:
    return;
  }
  render_request(&app->render);
}

static const struct wayl_listener wayl_listener = {
    .configure = on_configure,
    .close = on_close,
    .key = on_key,
};

/* ---- main ---- */
static void lm__string_dlist_dtor_fn(void *data) {
  String *s = data;
  string_deinit(s);
}

int main(int argc, char *argv[]) {
  int err = 0;
  bool fdm_ok = false, wayl_ok = false, render_ok = false;

  struct app app = {.running = true};
  state_init(&app.state);

  for (int i = 0; i < argc; i++) {
    struct state_entry entry = {0};

    DList l;
    dlist_init(&l, argc * sizeof(String), lm__string_dlist_dtor_fn);

    struct str str = str_of_cstring(argv[i]);
    str_trim(&str, isspace);

    while (str.len > 0) {
      struct str cut = str_split_left(&str, ' ');
      str_trim(&cut, isspace);
      if (cut.len > 0) {
        String cutted = str_to_owned(&cut);
        if (dlist_push_back(&l, &cutted) == NULL) {
          LOG_WARN("dlist push back null");
        }
      }
    }

    if (dlist_size(&l)) {
      entry.argv = l;
      entry.name = (String){.len = 0, .cap = 0, .data = NULL};
      state_push(&app.state, entry);
    }
  }

  if (fdm_init(&app.fdm) != 0) {
    LOG_FATAL("failed to init fdm");
    err = 1;
    goto cleanup;
  }
  fdm_ok = true;

  wayl_ok = true; /* wayl_destroy handles a partial init */
  if (wayl_init(&app.wayl, &app.fdm, &wayl_listener, &app, "lm10") != 0) {
    err = 1;
    goto cleanup;
  }

  if (render_init(&app.render, app.wayl.shm, app.wayl.surface, &app.state) !=
      0) {
    err = 1;
    goto cleanup;
  }
  render_ok = true;

  while (app.running) {
    err = wayl_prepare(&app.wayl);
    if (err != 0)
      goto cleanup;

    if (!app.running) { /* a dispatched event may have asked us to quit */
      wayl_finish(&app.wayl);
      break;
    }

    int wait_ret = fdm_wait(&app.fdm);
    int wait_errno = errno;
    wayl_finish(&app.wayl);

    if (wait_ret != 0) {
      LOG_ERROR("fdm error: %s", wait_errno ? strerror(wait_errno) : "");
      err = wait_errno ? wait_errno : 1;
      goto cleanup;
    }

    enum lm_err e = app.wayl.err != LM_OK ? app.wayl.err : app.render.err;
    if (e != LM_OK) {
      LOG_ERROR("app error: %s", lm_strerror(e));
      err = e;
      goto cleanup;
    }
  }

cleanup:
  if (render_ok)
    render_destroy(&app.render); /* before wayl: uses its surface and shm */
  if (wayl_ok)
    wayl_destroy(&app.wayl);
  if (fdm_ok)
    fdm_destroy(&app.fdm);
  state_destroy(&app.state);
  return err == 0 ? 0 : 1;
}
