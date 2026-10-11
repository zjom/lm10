/*
 * lm10: a Wayland picker. Lists entries read from stdin, one per line as
 * `[-n NAME] [-i ICON] [-x|-X] COMMAND [ARG...]` or as JSON objects (see
 * input.h); typing filters them,
 * C-n / C-p move the selection, Return picks it: -x execs it, -X runs it
 * detached, and otherwise its command line is printed to stdout.
 *
 * Build:
 * meson setup bld/debug && ninja -C bld/debug
 *
 * The look and the keys are set in ~/.config/lm10/lm10.ini (or -c PATH); see
 * config.h. A key runs a list of actions, e.g. Control+r =
 * change-prompt(> )+reload(ls ~) swaps the prompt and the entries.
 * Flags override the file for one run: --KEY VALUE for a [main] key,
 * --color NAME=VALUE,... and --bind KEY=ACTIONS, e.g. --color text=#ffffff.
 *
 * Run inside a Wayland session (GNOME, KDE, Sway, ...):
 *   printf '%s\n' '-n Ranger -i ranger.svg kitty ranger' '-X foot' |
 * ./bld/debug/lm10
 *   ./bld/debug/lm10 < examples/session.jsonc
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "action.h"
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
  /* A picked entry to print or exec in place of lm10 once it has shut down,
   * with the activation token to hand it. */
  const struct state_entry* pending;
  char* exec_token;
};

/* A NULL-terminated argv pointing into entry, or NULL on allocation failure.
 * Free with free(). */
static char** entry_argv(const struct state_entry* entry) {
  /* +1 for the NULL terminator execvp requires */
  char** argv = calloc(entry->argv.len + 1, sizeof(*argv));
  if (argv == NULL) {
    return NULL;
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
  return argv;
}

static void set_activation_env(const char* xdg_activation_token) {
  if (xdg_activation_token != NULL) {
    /* Prepare client for xdg activation startup */
    setenv("XDG_ACTIVATION_TOKEN", xdg_activation_token, 1);
    /* And X11 startup notifications */
    setenv("DESKTOP_STARTUP_ID", xdg_activation_token, 1);
  }
}

/* Prints the entry's argv to stdout as one shell-quoted line. Returns false
 * if writing failed. */
static bool print_entry(const struct state_entry* entry) {
  StrBuf line = STRBUF_INIT;
  size_t i = 0;
  ARR_FOREACH(const Str, s, &entry->argv) {
    if (i++ > 0)
      sb_append_char(&line, ' ');
    action_quote(&line, *s);
  }
  sb_append_char(&line, '\n');
  bool ok = !line.failed;
  if (!ok)
    LOG_ERROR("allocation failed");
  else if (fwrite(line.ptr, 1, line.len, stdout) != line.len ||
           fflush(stdout) != 0) {
    LOG_ERROR("failed to write to stdout: %s", strerror(errno));
    ok = false;
  }
  sb_free(&line);
  return ok;
}

/* Replaces lm10 with the entry's command, so it keeps lm10's terminal,
 * session and stdout/stderr. Only returns on failure. */
static void exec_inplace(const struct state_entry* entry,
                         const char* xdg_activation_token) {
  char** argv = entry_argv(entry);
  if (argv == NULL) {
    LOG_ERROR("argv allocation failed");
    return;
  }
  set_activation_env(xdg_activation_token);

  /* stdin carried the entry list; give the command the terminal instead */
  if (!isatty(STDIN_FILENO)) {
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd == -1)
      fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd != -1) {
      dup2(fd, STDIN_FILENO);
      close(fd);
    }
  }

  execvp(argv[0], argv);
  LOG_ERROR("failed to execute: %s", strerror(errno));
  free(argv);
}

static bool exec_entry(const struct state_entry* entry,
                       const char* xdg_activation_token) {
  bool ok = false;
  char** argv = entry_argv(entry);
  if (argv == NULL) {
    return false;
  }

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

    set_activation_env(xdg_activation_token);

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
  app->running = false;

  if (entry->run == STATE_RUN_PRINT) {
    /* main prints it after tearing everything else down */
    app->pending = entry;
    return true;
  }

  char* xdg_activation_token = NULL;
  get_xdg_activation_token(&app->wayl.seat, NULL, &xdg_activation_token);

  if (entry->run == STATE_RUN_EXEC) {
    /* main execs it after tearing everything else down */
    app->pending = entry;
    app->exec_token = xdg_activation_token;
    return true;
  }

  bool ok = exec_entry(entry, xdg_activation_token);
  if (!ok) {
    app->wayl.err = LM_ERR_EXEC;
  }
  free(xdg_activation_token);

  return ok;
}

static bool load_binds(struct kb_manager* kb, const struct config* cfg) {
  for (size_t i = 0; i < cfg->binds.len; i++) {
    const struct config_bind* b = &cfg->binds.ptr[i];
    if (b->actions.len == 0)
      kb_unload_bind(kb, b->key);
    else if (!kb_load_bind(kb, b->key, (uint32_t)i))
      return false;
  }
  return true;
}

static void on_entries(void* data) {
  struct app* app = data;
  render_request(&app->render);
}

/* ---- wayl callbacks ---- */

static void on_size(void* data, int out_w, int out_h, int* w, int* h) {
  struct app* app = data;
  const struct config_ui* ui = &app->config.ui;
  int want_w = ui->width_percent ? out_w * ui->width / 100 : ui->width;
  int want_h = draw_height(&app->render.dctx);
  *w = want_w < out_w ? want_w : out_w;
  *h = want_h < out_h ? want_h : out_h;
}

static void on_configure(void* data, int w, int h) {
  struct app* app = data;
  render_resize(&app->render, w, h);
  /* configure must be answered with a commit, even if nothing changed */
  render_request(&app->render);
}

static void on_scale(void* data, uint32_t scale120) {
  struct app* app = data;
  render_set_scale(&app->render, scale120);
  render_request(&app->render);
}

static void on_close(void* data) {
  struct app* app = data;
  app->running = false;
}

/* Replaces the entries with the output of the action's command. */
static bool reload(struct app* app, const char* cmd) {
  input_destroy(&app->input);
  state_clear_entries(&app->state);
  return input_init_command(&app->input, &app->fdm, cmd, &app->state,
                            on_entries, app) == 0;
}

/* Runs one action. Returns false if the ones after it shouldn't run. */
static bool run_action(struct app* app, const struct action* a) {
  struct state* st = &app->state;
  bool ok = true;

  switch (a->kind) {
    case ACTION_CANCEL:
      app->running = false;
      return false;
    case ACTION_DELETE_PREV:
      ok = state_query_delete(st, STATE_MOTION_CHAR_LEFT);
      break;
    case ACTION_DELETE_PREV_WORD:
      ok = state_query_delete(st, STATE_MOTION_WORD_LEFT);
      break;
    case ACTION_DELETE_NEXT:
      ok = state_query_delete(st, STATE_MOTION_CHAR_RIGHT);
      break;
    case ACTION_DELETE_NEXT_WORD:
      ok = state_query_delete(st, STATE_MOTION_WORD_RIGHT);
      break;
    case ACTION_DELETE_LINE_START:
      ok = state_query_delete(st, STATE_MOTION_START);
      break;
    case ACTION_DELETE_LINE_END:
      ok = state_query_delete(st, STATE_MOTION_END);
      break;
    case ACTION_DELETE_LINE:
      ok = state_query_clear(st);
      break;
    case ACTION_CURSOR_LEFT:
      state_cursor_move(st, STATE_MOTION_CHAR_LEFT);
      break;
    case ACTION_CURSOR_RIGHT:
      state_cursor_move(st, STATE_MOTION_CHAR_RIGHT);
      break;
    case ACTION_CURSOR_WORD_LEFT:
      state_cursor_move(st, STATE_MOTION_WORD_LEFT);
      break;
    case ACTION_CURSOR_WORD_RIGHT:
      state_cursor_move(st, STATE_MOTION_WORD_RIGHT);
      break;
    case ACTION_CURSOR_START:
      state_cursor_move(st, STATE_MOTION_START);
      break;
    case ACTION_CURSOR_END:
      state_cursor_move(st, STATE_MOTION_END);
      break;
    case ACTION_MATCH_EXEC: {
      const struct state_entry* entry = state_get_selection(st);
      if (entry == NULL)
        return true;
      exec_selection(app, entry);
      return false;
    }
    case ACTION_MATCH_NEXT:
      state_select_next(st);
      break;
    case ACTION_MATCH_PREV:
      state_select_prev(st);
      break;
    case ACTION_CHANGE_PROMPT:
      sb_clear(&app->config.ui.prompt);
      ok = sb_append(&app->config.ui.prompt, a->arg);
      break;
    case ACTION_CHANGE_QUERY:
      ok = state_query_set(st, a->arg);
      break;
    case ACTION_CHANGE_HEADER:
      ok = config_set_header(&app->config.ui.header, a->arg);
      break;
    case ACTION_CHANGE_BORDER_LABEL:
      sb_clear(&app->config.ui.border_label);
      ok = sb_append(&app->config.ui.border_label, a->arg);
      break;
    case ACTION_RELOAD:
    case ACTION_EXECUTE: {
      StrBuf cmd = STRBUF_INIT;
      if (!action_expand(&cmd, a->arg, st)) {
        LOG_WARN("command allocation failed");
        sb_free(&cmd);
        return false;
      }
      if (a->kind == ACTION_EXECUTE)
        action_execute(sb_cstr(&cmd));
      else if (!reload(app, sb_cstr(&cmd)))
        ok = false;
      sb_free(&cmd);
      return ok;
    }
  }
  if (!ok)
    LOG_WARN("query allocation failed");
  return true;
}

static void on_key(void* data, const struct kb_result* key) {
  struct app* app = data;

  if (!key->bound) {
    if (!state_query_insert(&app->state, str_from_cstr(key->utf8)))
      LOG_WARN("query allocation failed");
  } else {
    const ActionList* actions = &app->config.binds.ptr[key->bind].actions;
    ARR_FOREACH(const struct action, a, actions) {
      if (!run_action(app, a))
        break;
    }
  }
  if (app->running)
    render_request(&app->render);
}

static const struct wayl_listener wayl_listener = {
    .size = on_size,
    .configure = on_configure,
    .close = on_close,
    .scale = on_scale,
    .key = on_key,
};

/* ---- main ---- */

/* The [main] keys, each settable with --KEY VALUE. */
static const char* const main_keys[] = {
    "font",
    "prompt",
    "placeholder",
    "header",
    "border-label",
    "border-label-pos",
    "width",
    "lines",
    "padding",
    "row-padding",
    "border-width",
    "border-radius",
    "selection-radius",
    "show-counter",
    "icons",
    "icon-theme",
};
#define N_MAIN_KEYS (sizeof main_keys / sizeof main_keys[0])

/* getopt_long values past the chars: --KEY is OPT_MAIN + its index */
enum { OPT_COLOR = 0x100, OPT_BIND, OPT_MAIN };

/* A config flag as given, applied after the file. */
struct flag {
  int opt;
  const char* arg;
};

static void usage(FILE* f, const char* argv0) {
  fprintf(
      f,
      "usage: %s [-c CONFIG] [--KEY VALUE]... [--color NAME=VALUE,...]...\n"
      "       [--bind KEY=ACTIONS]...\n"
      "\n"
      "Reads entries from stdin, one per line:\n"
      "  [-n NAME] [-i ICON] [-x|-X] COMMAND [ARG...]\n"
      "or, if stdin starts with '{', '[' or a comment, as JSON(C) values,\n"
      "each an entry object or an array of them:\n"
      "  {\"name\": NAME, \"icon\": ICON, \"run\": \"print\"|\"exec\"|"
      "\"detach\",\n"
      "   \"command\": \"COMMAND ARG...\" | [COMMAND, ARG...]}\n"
      "\n"
      "Picking one execs it (-x, \"exec\"), runs it detached (-X,\n"
      "\"detach\"), or else prints its command line to stdout.\n"
      "\n"
      "  -c, --config PATH  config file (default: "
      "$XDG_CONFIG_HOME/lm10/lm10.ini)\n"
      "  -h, --help         show this help\n"
      "\n"
      "These override the config file, in order (see lm10.ini):\n"
      "  --font FONT        --prompt TEXT       --placeholder TEXT\n"
      "  --header TEXT      --border-label TEXT --border-label-pos N[:SIDE]\n"
      "  --width N|N%%       --lines N           --padding N\n"
      "  --row-padding N    --border-width N    --border-radius N\n"
      "  --selection-radius N                   --show-counter yes|no\n"
      "  --icons yes|no     --icon-theme NAME\n"
      "  --color NAME=#rrggbb[aa][,NAME=...]  set [colors] keys\n"
      "  --bind KEY=ACTION[+ACTION...]        bind a key, or none\n"
      "e.g. --width 40%% --bind Escape=cancel --color text=#ffffff\n"
      "     --header 'Pick one\\nor not' --border-label ' lm10 '\n"
      "     --border-label-pos 10:bottom\n"
      "\n"
      "A key runs actions joined by '+', some taking an argument:\n"
      "  --bind 'Control+r=change-prompt(> )+reload(ls ~)'\n"
      "change-prompt(T), change-query(T), change-header(T),\n"
      "change-border-label(T), reload(CMD) and execute(CMD) join the\n"
      "built-in ones (see lm10.ini). In CMD, {} {n} {q} {N}\n"
      "{N..M} become the selection's command, name, the query and argv\n"
      "words, shell-quoted.\n",
      argv0);
}

/* Loads the built-in config, then the file, then the flags in order.
 * Returns false on a fatal error. */
static bool load_config(struct config* cfg,
                        const char* path,
                        const struct flag* flags,
                        size_t n_flags) {
  if (!config_load_defaults(cfg)) {
    LOG_FATAL("failed to load default config");
    return false;
  }

  StrBuf def = STRBUF_INIT;
  bool explicit_path = path != NULL;
  int ret = 1; /* nowhere to look; the defaults will do */
  if (path == NULL && config_default_path(&def))
    path = sb_cstr(&def);
  if (path != NULL)
    ret = config_load_file(cfg, path);
  if (ret == 1 && explicit_path)
    LOG_ERROR("%s: no such file", path);
  sb_free(&def);
  if (!(ret == 0 || (ret == 1 && !explicit_path)))
    return false;

  for (size_t i = 0; i < n_flags; i++) {
    const struct flag* fl = &flags[i];
    bool ok;
    if (fl->opt == OPT_COLOR)
      ok = config_set_colors(cfg, fl->arg);
    else if (fl->opt == OPT_BIND)
      ok = config_set_bind(cfg, fl->arg);
    else
      ok = config_set_main(cfg, main_keys[fl->opt - OPT_MAIN], fl->arg);
    if (!ok) {
      LOG_FATAL("flags: allocation failed");
      return false;
    }
  }
  return true;
}

int main(int argc, char** argv) {
  int err = 0;
  bool fdm_ok = false, wayl_ok = false, render_ok = false, input_ok = false;
  const char* config_path = NULL;
  /* config flags, in order; there are fewer than argc of them */
  struct flag* flags = calloc((size_t)argc, sizeof(*flags));
  size_t n_flags = 0;
  if (flags == NULL) {
    LOG_FATAL("allocation failed");
    return 1;
  }

  struct option opts[4 + N_MAIN_KEYS + 1] = {
      {"config", required_argument, NULL, 'c'},
      {"help", no_argument, NULL, 'h'},
      {"color", required_argument, NULL, OPT_COLOR},
      {"bind", required_argument, NULL, OPT_BIND},
  };
  for (size_t i = 0; i < N_MAIN_KEYS; i++)
    opts[4 + i] = (struct option){main_keys[i], required_argument, NULL,
                                  OPT_MAIN + (int)i};
  int opt;
  while ((opt = getopt_long(argc, argv, "c:h", opts, NULL)) != -1) {
    switch (opt) {
      case 'c':
        config_path = optarg;
        break;
      case 'h':
        usage(stdout, argv[0]);
        free(flags);
        return 0;
      case '?':
        usage(stderr, argv[0]);
        free(flags);
        return 1;
      default:
        flags[n_flags++] = (struct flag){opt, optarg};
        break;
    }
  }
  if (optind < argc) {
    usage(stderr, argv[0]);
    free(flags);
    return 1;
  }

  struct app app = {.running = true};
  config_init(&app.config);
  state_init(&app.state);

  if (fdm_init(&app.fdm) != 0) {
    LOG_FATAL("failed to init fdm");
    err = 1;
    goto cleanup;
  }
  fdm_ok = true;

  bool config_ok = load_config(&app.config, config_path, flags, n_flags);
  free(flags); /* the flags were copied into the config */
  if (!config_ok) {
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

  /* Before the first configure is dispatched: on_size measures with it. */
  if (render_init(&app.render, app.wayl.shm, app.wayl.surface, &app.state,
                  &app.config.ui) != 0) {
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
    if (wayl_prepare(&app.wayl) != LM_OK) {
      err = 1;
      goto cleanup;
    }

    if (!app.running) { /* a dispatched event may have asked us to quit */
      wayl_finish(&app.wayl);
      break;
    }

    enum lm_err wait_err = fdm_wait(&app.fdm);
    wayl_finish(&app.wayl);
    if (wait_err != LM_OK) { /* already logged */
      err = 1;
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
  if (err == 0 && app.pending != NULL) { /* entry is in state */
    if (app.pending->run == STATE_RUN_PRINT) {
      if (!print_entry(app.pending))
        err = 1;
    } else {
      exec_inplace(app.pending, app.exec_token);
      err = 1;
    }
  }
  free(app.exec_token);
  state_destroy(&app.state);
  config_destroy(&app.config);
  return err == 0 ? 0 : 1;
}
