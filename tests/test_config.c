#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "test.h"

/* The last bind for key, or NULL. */
static const struct config_bind* find_bind(const struct config* cfg,
                                           const char* key) {
  struct kb_keypress k;
  if (!kb_parse_keypress(str_from_cstr(key), &k))
    return NULL;
  const struct config_bind* found = NULL;
  ARR_FOREACH(const struct config_bind, b, &cfg->binds) {
    if (b->key.mods == k.mods && b->key.sym == k.sym)
      found = b;
  }
  return found;
}

/* Writes text to a temporary file and loads it into cfg. */
static int load_text(struct config* cfg, const char* text) {
  char path[] = "/tmp/lm10-test-XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd != -1);
  if (fd == -1)
    return -1;
  FILE* f = fdopen(fd, "w");
  fputs(text, f);
  fclose(f);
  int ret = config_load_file(cfg, path);
  unlink(path);
  return ret;
}

static void test_defaults(void) {
  struct config cfg;
  config_init(&cfg);
  CHECK(config_load_defaults(&cfg));
  CHECK_STR(sb_str(&cfg.ui.font), "Sans 13");
  CHECK(cfg.ui.width == 600 && !cfg.ui.width_percent);
  const struct config_bind* b = find_bind(&cfg, "Return");
  CHECK(b != NULL && b->actions.len == 1 &&
        b->actions.ptr[0].kind == ACTION_MATCH_EXEC);
  config_destroy(&cfg);
}

static void test_file(void) {
  struct config cfg;
  config_init(&cfg);
  CHECK(config_load_defaults(&cfg));
  CHECK(load_text(&cfg,
                  "# comment\n"
                  "; also a comment\n"
                  "[main]\n"
                  "font = Mono 10\n"
                  "prompt = \"> \"\n"
                  "width = 40%\n"
                  "lines = 5\n"
                  "padding = -1\n"  // out of range: kept
                  "header = one\\ntwo\n"
                  "border-label-pos = -10:bottom\n"
                  "show-counter = off\n"
                  "bogus = 1\n"
                  "[colors]\n"
                  "text = #ff000080\n"
                  "border = #00ff00\n"
                  "match = red\n"  // bad: kept
                  "[nope]\n"
                  "font = ignored\n"
                  "[keys]\n"
                  "Control+r = change-prompt(> )+reload[ls {}]\n"
                  "Return = none\n"
                  "Control+x = no-such-action\n") == 0);

  CHECK_STR(sb_str(&cfg.ui.font), "Mono 10");
  CHECK_STR(sb_str(&cfg.ui.prompt), "> ");
  CHECK(cfg.ui.width == 40 && cfg.ui.width_percent);
  CHECK(cfg.ui.lines == 5);
  CHECK(cfg.ui.padding == 14);
  CHECK_STR(sb_str(&cfg.ui.header), "one\ntwo");
  CHECK(cfg.ui.border_label_pos == -10 && cfg.ui.border_label_bottom);
  CHECK(!cfg.ui.show_counter);
  CHECK(cfg.ui.colors.text.r == 1.0 && cfg.ui.colors.text.g == 0.0);
  CHECK(cfg.ui.colors.text.a > 0.5 && cfg.ui.colors.text.a < 0.51);
  CHECK(cfg.ui.colors.border.g == 1.0 && cfg.ui.colors.border.a == 1.0);
  CHECK(cfg.ui.colors.match.r > 0.9 && cfg.ui.colors.match.b < 0.6);

  const struct config_bind* b = find_bind(&cfg, "Control+r");
  CHECK(b != NULL && b->actions.len == 2);
  if (b != NULL && b->actions.len == 2) {
    CHECK(b->actions.ptr[0].kind == ACTION_CHANGE_PROMPT);
    CHECK_STR(b->actions.ptr[0].arg, "> ");
    CHECK(b->actions.ptr[1].kind == ACTION_RELOAD);
    CHECK_STR(b->actions.ptr[1].arg, "ls {}");
  }
  b = find_bind(&cfg, "Return");
  CHECK(b != NULL && b->actions.len == 0);
  b = find_bind(&cfg, "Control+x");
  CHECK(b == NULL);
  config_destroy(&cfg);
}

static void test_missing_file(void) {
  struct config cfg;
  config_init(&cfg);
  CHECK(config_load_file(&cfg, "/nonexistent/lm10.ini") == 1);
  config_destroy(&cfg);
}

static void test_bad_actions(void) {
  static const char* const bad[] = {
      "Escape=",               // empty
      "Escape=cancel(x)",      // takes no argument
      "Escape=reload",         // needs an argument
      "Escape=reload(ls",      // unterminated
      "Escape=cancel cancel",  // missing +
      "Escape=cancel+",        // trailing +
      "NotAKey=cancel",        // bad key
      "no equals sign",
  };
  struct config cfg;
  config_init(&cfg);
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    CHECK(config_set_bind(&cfg, bad[i]));
  CHECK(cfg.binds.len == 0);
  config_destroy(&cfg);
}

static void test_flags(void) {
  struct config cfg;
  config_init(&cfg);
  CHECK(config_load_defaults(&cfg));
  /* Flag values are taken as is: not trimmed or unquoted. */
  CHECK(config_set_main(&cfg, "prompt", " \"x\" "));
  CHECK_STR(sb_str(&cfg.ui.prompt), " \"x\" ");
  CHECK(config_set_colors(&cfg, "text=#000000, border = #ffffff,bad"));
  CHECK(cfg.ui.colors.text.r == 0.0);
  CHECK(cfg.ui.colors.border.r == 1.0);
  CHECK(config_set_bind(&cfg, "Control+q=cancel"));
  CHECK(find_bind(&cfg, "Control+q") != NULL);
  config_destroy(&cfg);
}

int main(void) {
  RUN(test_defaults);
  RUN(test_file);
  RUN(test_missing_file);
  RUN(test_bad_actions);
  RUN(test_flags);
  return TEST_RESULT;
}
