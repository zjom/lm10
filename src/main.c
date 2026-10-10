/*
 * lm10: a Wayland picker. Lists entries read from stdin, one per line as
 * `[-n NAME] [-i ICON] COMMAND [ARG...]`; typing filters them, C-n / C-p move
 * the selection, Return runs it.
 *
 * Build:
 * meson setup bld/debug && ninja -C bld/debug
 *
 * Run inside a Wayland session (GNOME, KDE, Sway, ...):
 *   printf '%s\n' '-n Ranger -i ranger.svg kitty ranger' foot |
 * ./bld/debug/lm10
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "fdm.h"
#include "input.h"
#include "kb.h"
#include "log.h"
#include "render.h"
#include "state.h"
#include "wayl.h"

struct app {
  struct fdm fdm;
  struct config config;
  struct kb_manager* kb;
  struct input input;  // feeds entries from stdin into the state
  struct wayl wayl;
  struct state state;    // the state
  struct render render;  // presents the state
  bool running;
};

static bool exec_entry(const struct state_entry* entry,
                       const char* xdg_activation_token) {
  bool ok = false;
  size_t argc = entry->argv.len;
  /* +1 for the NULL terminator execvp requires */
  char** argv = calloc(argc + 1, sizeof(*argv));
  if (argv == NULL) {
    return false;
  }

  size_t i = 0;
  ARR_FOREACH(const Str, s, &entry->argv) {
    argv[i] = (char*)s->ptr;
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

    if (xdg_activation_token != NULL) {
      /* Prepare client for xdg activation startup */
      setenv("XDG_ACTIVATION_TOKEN", xdg_activation_token, 1);
      /* And X11 startup notifications */
      setenv("DESKTOP_STARTUP_ID", xdg_activation_token, 1);
    }

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
    ok = true;
  } else {
    if (ret == -1) {
      LOG_ERROR("failed to read from pipe: %s", strerror(errno));
    } else if (ret == sizeof(child_errno)) {
      LOG_ERROR("failed to execute: %s", strerror(child_errno));
    } else {
      LOG_ERROR("short read from pipe");
    }
  }

out_free_argv:
  free(argv);
  return ok;
}

static bool exec_selection(struct app* app, const struct state_entry* entry) {
  char* xdg_activation_token = NULL;
  get_xdg_activation_token(&app->wayl.seat, NULL, &xdg_activation_token);
  bool ok = exec_entry(entry, xdg_activation_token);
  if (!ok) {
    app->wayl.err = LM_ERR_EXEC;
  }
  free(xdg_activation_token);
  app->running = false;

  return ok;
}

static bool load_binds(struct kb_manager* kb, const struct config* cfg) {
  ARR_FOREACH(const struct config_bind, b, &cfg->binds) {
    if (!kb_load_bind(kb, b->key, b->action))
      return false;
  }
  return true;
}

static void on_entries(void* data) {
  struct app* app = data;
  render_request(&app->render);
}

/* ---- wayl callbacks ---- */

static void on_configure(void* data, int w, int h) {
  struct app* app = data;
  render_resize(&app->render, w, h);
  /* configure must be answered with a commit, even if nothing changed */
  render_request(&app->render);
}

static void on_close(void* data) {
  struct app* app = data;
  app->running = false;
}

static void on_key(void* data, const struct kb_result* key) {
  struct app* app = data;
  struct state* st = &app->state;
  bool ok = true;

  switch (key->action) {
    case KB_ACTION_NONE:
      ok = state_query_insert(st, str_from_cstr(key->utf8));
      break;
    case KB_ACTION_CANCEL:
      app->running = false;
      return;
    case KB_ACTION_DELETE_PREV:
      ok = state_query_delete(st, STATE_MOTION_CHAR_LEFT);
      break;
    case KB_ACTION_DELETE_PREV_WORD:
      ok = state_query_delete(st, STATE_MOTION_WORD_LEFT);
      break;
    case KB_ACTION_DELETE_NEXT:
      ok = state_query_delete(st, STATE_MOTION_CHAR_RIGHT);
      break;
    case KB_ACTION_DELETE_NEXT_WORD:
      ok = state_query_delete(st, STATE_MOTION_WORD_RIGHT);
      break;
    case KB_ACTION_DELETE_LINE_START:
      ok = state_query_delete(st, STATE_MOTION_START);
      break;
    case KB_ACTION_DELETE_LINE_END:
      ok = state_query_delete(st, STATE_MOTION_END);
      break;
    case KB_ACTION_DELETE_LINE:
      ok = state_query_clear(st);
      break;
    case KB_ACTION_CURSOR_LEFT:
      state_cursor_move(st, STATE_MOTION_CHAR_LEFT);
      break;
    case KB_ACTION_CURSOR_RIGHT:
      state_cursor_move(st, STATE_MOTION_CHAR_RIGHT);
      break;
    case KB_ACTION_CURSOR_WORD_LEFT:
      state_cursor_move(st, STATE_MOTION_WORD_LEFT);
      break;
    case KB_ACTION_CURSOR_WORD_RIGHT:
      state_cursor_move(st, STATE_MOTION_WORD_RIGHT);
      break;
    case KB_ACTION_CURSOR_START:
      state_cursor_move(st, STATE_MOTION_START);
      break;
    case KB_ACTION_CURSOR_END:
      state_cursor_move(st, STATE_MOTION_END);
      break;
    case KB_ACTION_MATCH_EXEC: {
      const struct state_entry* entry = state_get_selection(st);
      if (entry != NULL)
        exec_selection(app, entry);
      return;
    }
    case KB_ACTION_MATCH_NEXT:
      state_select_next(st);
      break;
    case KB_ACTION_MATCH_PREV:
      state_select_prev(st);
      break;
  }
  if (!ok)
    LOG_WARN("query allocation failed");
  render_request(&app->render);
}

static const struct wayl_listener wayl_listener = {
    .configure = on_configure,
    .close = on_close,
    .key = on_key,
};

/* ---- main ---- */
int main(void) {
  int err = 0;
  bool fdm_ok = false, wayl_ok = false, render_ok = false, input_ok = false;

  struct app app = {.running = true};
  config_init(&app.config);
  state_init(&app.state);

  if (fdm_init(&app.fdm) != 0) {
    LOG_FATAL("failed to init fdm");
    err = 1;
    goto cleanup;
  }
  fdm_ok = true;

  if (!config_load_defaults(&app.config)) {
    LOG_FATAL("failed to load config");
    err = 1;
    goto cleanup;
  }

  app.kb = kb_manager_new();
  if (app.kb == NULL || !load_binds(app.kb, &app.config)) {
    LOG_FATAL("failed to set up keybindings");
    err = 1;
    goto cleanup;
  }

  wayl_ok = true; /* wayl_destroy handles a partial init */
  if (wayl_init(&app.wayl, &app.fdm, app.kb, &wayl_listener, &app, "lm10") !=
      0) {
    err = 1;
    goto cleanup;
  }

  if (render_init(&app.render, app.wayl.shm, app.wayl.surface, &app.state) !=
      0) {
    err = 1;
    goto cleanup;
  }
  render_ok = true;

  /* after render: new entries request a redraw */
  if (input_init(&app.input, &app.fdm, STDIN_FILENO, &app.state, on_entries,
                 &app) != 0) {
    err = 1;
    goto cleanup;
  }
  input_ok = true;

  // we may have inherited XDG_ACTIVATION_TOKEN
  // from launching parent process.
  unsetenv("XDG_ACTIVATION_TOKEN");
  unsetenv("DESKTOP_STARTUP_ID");

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
  if (input_ok)
    input_destroy(&app.input);
  if (render_ok)
    render_destroy(&app.render); /* before wayl: uses its surface and shm */
  if (wayl_ok)
    wayl_destroy(&app.wayl);
  kb_manager_destroy(app.kb); /* after wayl: it looks keys up in it */
  if (fdm_ok)
    fdm_destroy(&app.fdm);
  state_destroy(&app.state);
  config_destroy(&app.config);
  return err == 0 ? 0 : 1;
}
