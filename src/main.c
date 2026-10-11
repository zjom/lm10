/*
 * lm10: a Wayland picker. Entries come from stdin (see entry_parse.h for the
 * formats); typing filters them and picking one prints, execs or detaches
 * it. The look and the keys come from lm10.ini (config.h) and flags (usage
 * below). See README.md for building and running it.
 *
 * main wires the parts together: input feeds the state, wayl turns key
 * presses into the actions below, and render draws the state on wayl's
 * surface. The picked entry runs once all of that has been torn down.
 */
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "action.h"
#include "config.h"
#include "fdm.h"
#include "input.h"
#include "kb.h"
#include "log.h"
#include "render.h"
#include "shell.h"
#include "spawn.h"
#include "state.h"
#include "util.h"
#include "wayl.h"

struct app {
  struct fdm fdm;
  struct procs procs;  // reload commands
  struct config config;
  struct kb_manager* kb;
  struct input input;  // feeds entries from stdin into the state
  struct wayl* wayl;
  struct state state;    // the state
  struct render render;  // presents the state
  bool running;
  /* The picked entry, run once the event loop has stopped: nothing may
   * change the state after it is set. Without one, lm10 was cancelled. */
  const struct state_entry* pending;
};

/* Exit statuses */
enum {
  EXIT_PICKED = 0,
  EXIT_ERROR = 1,
  EXIT_CANCELLED = 130,  // nothing was picked, as in fzf
};

/* A NULL-terminated argv pointing into entry. Free with free(). */
static char** entry_argv(const struct state_entry* entry) {
  char** argv = xcalloc(entry->argv.len + 1, sizeof *argv);
  for (size_t i = 0; i < entry->argv.len; i++)
    argv[i] = (char*)entry->argv.ptr[i].ptr;  // NUL-terminated
  return argv;
}

/* Prints the entry's argv to stdout as one shell-quoted line. Returns false
 * if writing failed. */
static bool print_entry(const struct state_entry* entry) {
  StrBuf line = STRBUF_INIT;
  size_t i = 0;
  ARR_FOREACH(const Str, s, &entry->argv) {
    if (i++ > 0)
      sb_append_char(&line, ' ');
    shell_quote(&line, *s);
  }
  sb_append_char(&line, '\n');
  bool ok = true;
  if (fwrite(line.ptr, 1, line.len, stdout) != line.len ||
      fflush(stdout) != 0) {
    LOG_ERROR("failed to write to stdout: %s", strerror(errno));
    ok = false;
  }
  sb_free(&line);
  return ok;
}

/* Picks entry: the loop stops, and main runs it. */
static void pick(struct app* app, const struct state_entry* entry) {
  app->pending = entry;
  app->running = false;
}

/* Runs the picked entry once everything else has been torn down, handing it
 * the activation token if there is one. Returns false on failure; doesn't
 * return if the entry execs in place of lm10. */
static bool run_entry(const struct state_entry* entry, const char* token) {
  if (entry->run == STATE_RUN_PRINT)
    return print_entry(entry);

  char** argv = entry_argv(entry);
  bool ok = false;
  if (entry->run == STATE_RUN_EXEC)
    spawn_exec(argv, token);
  else
    ok = spawn_detached(argv, token);
  free(argv);
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
  int want_h = draw_height(&app->render.dctx, &app->state);
  *w = want_w < out_w ? want_w : out_w;
  *h = want_h < out_h ? want_h : out_h;
}

static void on_configure(void* data, int w, int h) {
  struct app* app = data;
  render_resize(&app->render, w, h);
  /* configure must be answered with a commit, even if nothing changed */
  render_invalidate(&app->render);
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
  return input_init_command(&app->input, &app->fdm, &app->procs, cmd,
                            &app->state, on_entries, app) == 0;
}

/* Runs sh -c cmd and waits for it, with its stdout on stderr so it can't
 * disturb lm10's output. Keys pressed meanwhile are handled after. */
static void execute(const char* cmd) {
  LOG_DEBUG("execute: %s", cmd);
  pid_t pid = spawn_sh(cmd, STDERR_FILENO, false);
  while (pid != -1 && waitpid(pid, NULL, 0) == -1 && errno == EINTR)
    ;
}

/* Runs one action. Returns true if the bind's later actions must be
 * skipped. */
static bool run_action(struct app* app, const struct action* a) {
  struct state* st = &app->state;

  switch (a->kind) {
    case ACTION_CANCEL:
      app->running = false;
      return true;
    case ACTION_DELETE_PREV:
      state_query_delete(st, STATE_MOTION_CHAR_LEFT);
      break;
    case ACTION_DELETE_PREV_WORD:
      state_query_delete(st, STATE_MOTION_WORD_LEFT);
      break;
    case ACTION_DELETE_NEXT:
      state_query_delete(st, STATE_MOTION_CHAR_RIGHT);
      break;
    case ACTION_DELETE_NEXT_WORD:
      state_query_delete(st, STATE_MOTION_WORD_RIGHT);
      break;
    case ACTION_DELETE_LINE_START:
      state_query_delete(st, STATE_MOTION_START);
      break;
    case ACTION_DELETE_LINE_END:
      state_query_delete(st, STATE_MOTION_END);
      break;
    case ACTION_DELETE_LINE:
      state_query_clear(st);
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
        return false;
      pick(app, entry);
      return true;
    }
    case ACTION_MATCH_NEXT:
      state_select_next(st);
      break;
    case ACTION_MATCH_PREV:
      state_select_prev(st);
      break;
    case ACTION_CHANGE_PROMPT:
      state_set_prompt(st, a->arg);
      break;
    case ACTION_CHANGE_QUERY:
      state_query_set(st, a->arg);
      break;
    case ACTION_CHANGE_HEADER: {
      StrBuf header = STRBUF_INIT;
      config_unescape(&header, a->arg);
      state_set_header(st, sb_str(&header));
      sb_free(&header);
      break;
    }
    case ACTION_CHANGE_BORDER_LABEL:
      state_set_border_label(st, a->arg);
      break;
    case ACTION_RELOAD:
    case ACTION_EXECUTE: {
      StrBuf cmd = STRBUF_INIT;
      bool stop = false;
      action_expand(&cmd, a->arg, st);
      if (a->kind == ACTION_EXECUTE)
        execute(sb_cstr(&cmd));
      else
        stop = !reload(app, sb_cstr(&cmd));
      sb_free(&cmd);
      return stop;
    }
  }
  return false;
}

static void run_actions(struct app* app, uint32_t bind) {
  const ActionList* actions = &app->config.binds.ptr[bind].actions;
  for (size_t i = 0; i < actions->len; i++) {
    if (run_action(app, &actions->ptr[i]))
      return;
  }
}

static void on_key(void* data, const struct kb_result* key) {
  struct app* app = data;
  /* Once the loop is stopping, keys already queued (or repeating) must not
   * touch the state: a picked entry points into it. */
  if (!app->running)
    return;

  if (!key->bound) {
    state_query_insert(&app->state, str_from_cstr(key->text));
  } else {
    run_actions(app, key->bind);
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

/* getopt_long values past the chars: --KEY is OPT_MAIN + its index in
 * config_main_keys */
enum { OPT_COLOR = 0x100, OPT_BIND, OPT_MAIN };

/* A config flag as given, applied after the file. */
struct flag {
  int opt;
  const char* arg;
};

static void usage(FILE* f, const char* argv0) {
  fprintf(f,
          "usage: %s [-v] [-c CONFIG] [--KEY VALUE]... [--color "
          "NAME=VALUE,...]...\n"
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
          "Exits 0 once something was picked, 130 if nothing was, 1 on error.\n"
          "\n"
          "  -c, --config PATH  config file (default: "
          "$XDG_CONFIG_HOME/lm10/lm10.ini)\n"
          "  -v, --verbose      log more; twice for even more (or set\n"
          "                     LM10_LOG=error|warn|info|debug|trace)\n"
          "  -h, --help         show this help\n"
          "\n"
          "These override the config file, in order (see lm10.ini):\n",
          argv0);
  for (size_t i = 0; i < config_n_main_keys; i++)
    fprintf(f, "  --%s %s\n", config_main_keys[i].name,
            config_main_keys[i].arg);
  fputs(
      "  --color NAME=#rrggbb[aa][,NAME=...]  set [colors] keys\n"
      "  --bind KEY=ACTION[+ACTION...]        bind a key, or none\n"
      "e.g. --width 40% --bind Escape=cancel --color text=#ffffff\n"
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
      f);
}

/* Sets the log level from LM10_LOG, then raises it by verbose steps from
 * the default, warnings. */
static void set_log_level(int verbose) {
  static const struct {
    const char* name;
    int level;
  } levels[] = {
      {"trace", LOG_LEVEL_TRACE}, {"debug", LOG_LEVEL_DEBUG},
      {"info", LOG_LEVEL_INFO},   {"warn", LOG_LEVEL_WARN},
      {"error", LOG_LEVEL_ERROR}, {"off", LOG_LEVEL_OFF},
  };
  int level = LOG_LEVEL_WARN;
  const char* env = getenv("LM10_LOG");
  if (env != NULL && env[0] != '\0') {
    size_t i = 0;
    while (i < sizeof levels / sizeof levels[0] &&
           strcmp(env, levels[i].name) != 0)
      i++;
    if (i < sizeof levels / sizeof levels[0])
      level = levels[i].level;
    else
      fprintf(stderr, "lm10: ignoring LM10_LOG=%s\n", env);
  }
  if (verbose > 0)
    level = verbose == 1 ? LOG_LEVEL_DEBUG : LOG_LEVEL_TRACE;
  log_set_level(level);
}

/* Loads the built-in config, then the file, then the flags in order.
 * Returns false if the file couldn't be read. */
static bool load_config(struct config* cfg,
                        const char* path,
                        const struct flag* flags,
                        size_t n_flags) {
  config_load_defaults(cfg);

  StrBuf def = STRBUF_INIT;
  bool explicit_path = path != NULL;
  int ret = 1;  // nowhere to look; the defaults will do
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
    if (fl->opt == OPT_COLOR)
      config_set_colors(cfg, fl->arg);
    else if (fl->opt == OPT_BIND)
      config_set_bind(cfg, fl->arg);
    else
      config_set_main(cfg, config_main_keys[fl->opt - OPT_MAIN].name, fl->arg);
  }
  return true;
}

int main(int argc, char** argv) {
  int err = 0;
  char* token = NULL;  // activation token for the picked entry
  bool render_ok = false, input_ok = false;
  const char* config_path = NULL;
  /* config flags, in order; there are fewer than argc of them */
  struct flag* flags = xcalloc((size_t)argc, sizeof(*flags));
  size_t n_flags = 0;

  static const struct option fixed_opts[] = {
      {"config", required_argument, NULL, 'c'},
      {"verbose", no_argument, NULL, 'v'},
      {"help", no_argument, NULL, 'h'},
      {"color", required_argument, NULL, OPT_COLOR},
      {"bind", required_argument, NULL, OPT_BIND},
  };
  enum { N_FIXED = sizeof fixed_opts / sizeof fixed_opts[0] };
  struct option* opts = xcalloc(N_FIXED + config_n_main_keys + 1, sizeof *opts);
  memcpy(opts, fixed_opts, sizeof fixed_opts);
  for (size_t i = 0; i < config_n_main_keys; i++)
    opts[N_FIXED + i] = (struct option){
        config_main_keys[i].name, required_argument, NULL, OPT_MAIN + (int)i};
  int opt, verbose = 0;
  while ((opt = getopt_long(argc, argv, "c:vh", opts, NULL)) != -1) {
    switch (opt) {
      case 'c':
        config_path = optarg;
        break;
      case 'v':
        verbose++;
        break;
      case 'h':
        usage(stdout, argv[0]);
        free(opts);
        free(flags);
        return EXIT_SUCCESS;
      case '?':
        usage(stderr, argv[0]);
        free(opts);
        free(flags);
        return EXIT_ERROR;
      default:
        flags[n_flags++] = (struct flag){opt, optarg};
        break;
    }
  }
  free(opts);
  if (optind < argc) {
    usage(stderr, argv[0]);
    free(flags);
    return EXIT_ERROR;
  }
  set_log_level(verbose);

  struct app app = {.running = true};
  config_init(&app.config);
  state_init(&app.state);

  if (fdm_init(&app.fdm) != 0)
    die("epoll_create: %s", strerror(errno));
  procs_init(&app.procs, &app.fdm);

  bool config_ok = load_config(&app.config, config_path, flags, n_flags);
  free(flags);  // the flags were copied into the config
  if (!config_ok) {
    err = 1;
    goto cleanup;
  }
  const struct config_ui* ui = &app.config.ui;
  state_set_prompt(&app.state, sb_str(&ui->prompt));
  state_set_header(&app.state, sb_str(&ui->header));
  state_set_border_label(&app.state, sb_str(&ui->border_label));
  state_set_fuzzy(&app.state, ui->fuzzy);

  app.kb = kb_manager_new();
  if (app.kb == NULL || !load_binds(app.kb, &app.config)) {
    LOG_FATAL("failed to set up keybindings");
    err = 1;
    goto cleanup;
  }

  app.wayl = wayl_new(&app.fdm, app.kb, &wayl_listener, &app, "lm10");
  if (app.wayl == NULL) {
    err = 1;
    goto cleanup;
  }

  /* Before the first configure is dispatched: on_size measures with it. */
  if (render_init(&app.render, wayl_shm(app.wayl), wayl_surface(app.wayl),
                  &app.state, &app.config.ui) != 0) {
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
    wayl_prepare(app.wayl);
    if (app.running)  // a dispatched event may have asked us to quit
      fdm_wait(&app.fdm);
    wayl_finish(app.wayl);
  }

  /* Not from inside the loop: getting the token takes a roundtrip, which
   * must not run while the loop has a read prepared on the display. */
  if (app.pending != NULL && app.pending->run != STATE_RUN_PRINT)
    token = wayl_activation_token(app.wayl);

cleanup:
  if (input_ok)
    input_destroy(&app.input);
  if (render_ok)
    render_destroy(&app.render);  // before wayl: uses its surface and shm
  wayl_destroy(app.wayl);
  kb_manager_destroy(app.kb);  // after wayl: it looks keys up in it
  procs_destroy(&app.procs);  // after input: it terminates through procs
  fdm_destroy(&app.fdm);
  if (err == 0 && app.pending != NULL && !run_entry(app.pending, token))
    err = 1;  // the entry lives in state, which is still alive
  free(token);
  state_destroy(&app.state);
  config_destroy(&app.config);
  if (err != 0)
    return EXIT_ERROR;
  return app.pending != NULL ? EXIT_PICKED : EXIT_CANCELLED;
}
