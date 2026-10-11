/* Checks examples/lm10.ini (the path is argv[1]) against the code: it must
 * list every key and action, and its values must be the built-in defaults,
 * loading without a warning. */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "test.h"

static bool read_text(const char* path, StrBuf* out) {
  FILE* f = fopen(path, "r");
  if (f == NULL)
    return false;
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    sb_append(out, str_from_parts(chunk, n));
  fclose(f);
  return !out->failed;
}

/* Whether section has a "key =" line for name. */
static bool has_key(Str text, const char* section, const char* name) {
  Str line, current = {0};
  while (str_next_line(&text, &line)) {
    line = str_trim(line);
    if (line.len > 0 && line.ptr[0] == '[') {
      current = line;
      continue;
    }
    Str key, val;
    if (str_eq_cstr(current, section) && line.len > 0 && line.ptr[0] != '#' &&
        str_cut(line, STR("="), &key, &val) && str_eq_cstr(str_trim(key), name))
      return true;
  }
  return false;
}

/* The [main] and [colors] keys in the file must be the ones in the code. */
static void test_keys(Str text) {
  for (size_t i = 0; i < config_n_main_keys; i++) {
    if (!has_key(text, "[main]", config_main_keys[i].name)) {
      fprintf(stderr, "  [main] %s is missing\n", config_main_keys[i].name);
      test_failures++;
    }
  }
  for (size_t i = 0; i < config_n_color_keys; i++) {
    if (!has_key(text, "[colors]", config_color_keys[i].name)) {
      fprintf(stderr, "  [colors] %s is missing\n", config_color_keys[i].name);
      test_failures++;
    }
  }
  for (size_t i = 0; i < ACTION_COUNT; i++) {
    const char* name = action_name((enum action_kind)i);
    if (str_find(text, str_from_cstr(name)) == STR_NPOS) {
      fprintf(stderr, "  action %s isn't mentioned\n", name);
      test_failures++;
    }
  }
}

/* The actions the last bind for key runs, or NULL if it has none. */
static const ActionList* bound(const struct config* cfg,
                               struct kb_keypress key) {
  const ActionList* found = NULL;
  ARR_FOREACH(const struct config_bind, b, &cfg->binds) {
    if (b->key.mods == key.mods && b->key.sym == key.sym)
      found = b->actions.len > 0 ? &b->actions : NULL;
  }
  return found;
}

static bool same_actions(const ActionList* a, const ActionList* b) {
  if (a == NULL || b == NULL)
    return a == b;
  if (a->len != b->len)
    return false;
  for (size_t i = 0; i < a->len; i++) {
    if (a->ptr[i].kind != b->ptr[i].kind ||
        !str_eq(a->ptr[i].arg, b->ptr[i].arg))
      return false;
  }
  return true;
}

/* Every key either config binds must do the same in the other. */
static void check_binds(const struct config* x, const struct config* y) {
  const struct config* both[] = {x, y};
  for (size_t c = 0; c < 2; c++) {
    ARR_FOREACH(const struct config_bind, b, &both[c]->binds) {
      if (!same_actions(bound(x, b->key), bound(y, b->key))) {
        fprintf(stderr, "  key %#x+%#x is bound differently\n", b->key.mods,
                b->key.sym);
        test_failures++;
      }
    }
  }
}

static void test_defaults(const char* path) {
  struct config defaults, example;
  config_init(&defaults);
  config_init(&example);
  CHECK(config_load_defaults(&defaults));
  CHECK(config_load_defaults(&example));

  /* Loading it must not warn. */
  FILE* log = tmpfile();
  log_set_stream(log);
  CHECK(config_load_file(&example, path) == 0);
  log_set_stream(NULL);
  CHECK(ftell(log) == 0);
  fclose(log);

  const struct config_ui *d = &defaults.ui, *e = &example.ui;
  for (size_t i = 0; i < config_n_main_keys; i++) {
    const struct config_key* k = &config_main_keys[i];
    const void* dv = (const char*)d + k->off;
    const void* ev = (const char*)e + k->off;
    bool same = false;
    switch (k->kind) {
      case CONFIG_STRING:
      case CONFIG_TEXT:
        same = str_eq(sb_str(dv), sb_str(ev));
        break;
      case CONFIG_INT:
        same = *(const int*)dv == *(const int*)ev;
        break;
      case CONFIG_BOOL:
        same = *(const bool*)dv == *(const bool*)ev;
        break;
      case CONFIG_WIDTH:
        same = d->width == e->width && d->width_percent == e->width_percent;
        break;
      case CONFIG_LABEL_POS:
        same = d->border_label_pos == e->border_label_pos &&
               d->border_label_bottom == e->border_label_bottom;
        break;
    }
    if (!same) {
      fprintf(stderr, "  [main] %s isn't the default\n", k->name);
      test_failures++;
    }
  }
  for (size_t i = 0; i < config_n_color_keys; i++) {
    const struct config_color* dc =
        (const void*)((const char*)&d->colors + config_color_keys[i].off);
    const struct config_color* ec =
        (const void*)((const char*)&e->colors + config_color_keys[i].off);
    if (memcmp(dc, ec, sizeof *dc) != 0) {
      fprintf(stderr, "  [colors] %s isn't the default\n",
              config_color_keys[i].name);
      test_failures++;
    }
  }
  check_binds(&defaults, &example);

  config_destroy(&defaults);
  config_destroy(&example);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s LM10_INI\n", argv[0]);
    return 1;
  }
  StrBuf text = STRBUF_INIT;
  CHECK(read_text(argv[1], &text));
  test_keys(sb_str(&text));
  test_defaults(argv[1]);
  sb_free(&text);
  return TEST_RESULT;
}
