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
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct app {
  struct fdm fdm;
  struct wayl wayl;
  struct state state;   // the state
  struct render render; // presents the state
  bool running;
};

static bool exec_selection(void *data) {
  struct app *app = data;
  const struct state_entry *entry = state_get_selection(&app->state);
  if (entry == NULL) {
    return false;
  }

  bool ok = false;
  size_t argc = dlist_size(&entry->argv);
  /* +1 for the NULL terminator execvp requires */
  char **argv = calloc(argc + 1, sizeof(*argv));
  if (argv == NULL) {
    return false;
  }

  size_t i = 0;
  DLIST_FOREACH(&entry->argv, n) {
    argv[i] = DLIST_AT(n, String)->data;
    assert(argv[i] != NULL);
    i++;
  }

  LOG_DEBUG("argv:");
  for (size_t j = 0; argv[j] != NULL; j++)
    LOG_DEBUG("  %zu: \"%s\"", j, argv[j]);

  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
    LOG_ERROR("failed to create pipe %s", strerror(errno));
    goto out_free_argv;
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    LOG_ERROR("failed to fork %s", strerror(errno));
    goto out_free_argv;
  }

  if (pid == 0) {
    /* Intermediate child */
    close(pipe_fds[0]);
    if (setsid() == -1)
      goto child_err;

    pid_t gpid = fork();
    if (gpid == -1)
      goto child_err;
    if (gpid > 0)
      _exit(0); /* Grandchild gets reparented to init */

    int devnull_r = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull_r == -1)
      goto child_err;

    if (dup2(devnull_r, STDIN_FILENO) == -1)
      goto child_err;

    execvp(argv[0], argv);

  child_err: {
    /* Signal error back to parent process */
    int err = errno;
    (void)!write(pipe_fds[1], &err, sizeof(err));
    _exit(1);
  }
  }

  /* Parent */
  close(pipe_fds[1]);

  int child_errno;
  static_assert(sizeof(child_errno) == sizeof(errno), "errno size mismatch");

  ssize_t ret;
  do {
    ret = read(pipe_fds[0], &child_errno, sizeof(child_errno));
  } while (ret == -1 && errno == EINTR);
  close(pipe_fds[0]);

  /* Reap the intermediate child */
  while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
    ;
  if (ret == 0) {
    LOG_DEBUG("fork+exec succeeded");
    app->running = false;
    ok = true;
  } else {
    if (ret == -1) {
      LOG_ERROR("failed to read from pipe: %s", strerror(errno));
    } else if (ret == sizeof(child_errno)) {
      LOG_ERROR("failed to execute: %s", strerror(child_errno));
    } else {
      LOG_ERROR("short read from pipe");
    }
    app->wayl.err = LM_ERR_EXEC;
  }

out_free_argv:
  free(argv);
  return ok;
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
      if (!exec_selection(app)) {
        // TODO: handle exec failure
      }
      return;
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

  for (int i = 1; i < argc; i++) {
    struct state_entry entry = {0};

    DList l;
    dlist_init(&l, sizeof(String), lm__string_dlist_dtor_fn);

    struct str str = str_of_cstring(argv[i]);
    str_trim(&str, isspace);

    while (str.len > 0) {
      struct str cut = str_split_left(&str, ' ');
      str_trim(&cut, isspace);
      if (cut.len > 0) {
        String cutted = str_to_owned(&cut);
        if (cutted.len == 0) {
          LOG_WARN("cutted allocation failed");
          string_deinit(&cutted);
          continue;
        }
        if (dlist_push_back(&l, &cutted) == NULL) {
          LOG_WARN("dlist push back null");
          string_deinit(&cutted);
          continue;
        }
      }
    }

    if (dlist_size(&l)) {
      if (state_entry_init(&entry, NULL, l) == NULL) {
        LOG_WARN("entry allocation failed");
        continue;
      }
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
