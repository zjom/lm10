#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

void config_init(struct config* cfg) {
  *cfg = (struct config){
      .arena = ARENA_INIT,
      .binds = LIST_INIT,
      .ui = {.font = STRBUF_INIT,
             .prompt = STRBUF_INIT,
             .placeholder = STRBUF_INIT,
             .header = STRBUF_INIT,
             .border_label = STRBUF_INIT},
  };
}

void config_destroy(struct config* cfg) {
  LIST_FREE(&cfg->binds);
  arena_free(&cfg->arena);
  sb_free(&cfg->ui.font);
  sb_free(&cfg->ui.prompt);
  sb_free(&cfg->ui.placeholder);
  sb_free(&cfg->ui.header);
  sb_free(&cfg->ui.border_label);
}

static void set_str(StrBuf* dst, Str s) {
  sb_clear(dst);
  sb_append(dst, s);
}

void config_unescape(StrBuf* dst, Str s) {
  sb_clear(dst);
  Str before, after;
  while (str_cut(s, STR("\\n"), &before, &after)) {
    sb_append(dst, before);
    sb_append_char(dst, '\n');
    s = after;
  }
  sb_append(dst, s);
}

#define RGBA(hex)                                      \
  ((struct config_color){((hex) >> 24 & 0xff) / 255.0, \
                         ((hex) >> 16 & 0xff) / 255.0, \
                         ((hex) >> 8 & 0xff) / 255.0, ((hex) & 0xff) / 255.0})

static void load_default_ui(struct config_ui* ui) {
  ui->width = 600;
  ui->width_percent = false;
  ui->lines = 8;
  ui->padding = 14;
  ui->row_padding = 8;
  ui->border_width = 2;
  ui->border_radius = 12;
  ui->selection_radius = 8;
  ui->fuzzy = false;
  ui->show_counter = true;
  ui->border_label_pos = 0;
  ui->border_label_bottom = false;
  ui->colors = (struct config_colors){
      .background = RGBA(0x141415f2),
      .border = RGBA(0x878787ff),
      .text = RGBA(0xcdcdcdff),
      .match = RGBA(0xe8b589ff),
      .selection = RGBA(0x333738ff),
      .selection_text = RGBA(0xcdcdcdff),
      .selection_match = RGBA(0xe8b589ff),
      .prompt = RGBA(0x6e94b2ff),
      .input = RGBA(0xcdcdcdff),
      .placeholder = RGBA(0x606079ff),
      .cursor = RGBA(0xcdcdcdff),
      .counter = RGBA(0x606079ff),
      .separator = RGBA(0x252530ff),
      .header = RGBA(0x878787ff),
      .border_label = RGBA(0xcdcdcdff),
  };
  set_str(&ui->font, STR("Sans 13"));
  set_str(&ui->prompt, STR("❯ "));
  set_str(&ui->placeholder, STR("Search…"));
  set_str(&ui->header, STR(""));
  set_str(&ui->border_label, STR(""));
}

bool config_default_path(StrBuf* out) {
  const char* xdg = getenv("XDG_CONFIG_HOME");
  const char* home = getenv("HOME");
  sb_clear(out);
  if (xdg != NULL && xdg[0] != '\0')
    sb_appendf(out, "%s/lm10/lm10.ini", xdg);
  else if (home != NULL && home[0] != '\0')
    sb_appendf(out, "%s/.config/lm10/lm10.ini", home);
  else
    return false;
  return true;
}

/* ---- file parsing ---- */

/* Where a parse error is reported. */
struct loc {
  const char* path;
  size_t line;
};

/* line 0 is a command-line flag, which has none */
#define PARSE_WARN(loc, fmt, ...)                                      \
  do {                                                                 \
    if ((loc)->line != 0)                                              \
      LOG_WARN("%s:%zu: " fmt, (loc)->path, (loc)->line, __VA_ARGS__); \
    else                                                               \
      LOG_WARN("%s: " fmt, (loc)->path, __VA_ARGS__);                  \
  } while (0)

static bool parse_int(Str v, int min, int max, int* out) {
  int64_t n;
  if (!str_to_i64(v, 10, &n) || n < min || n > max)
    return false;
  *out = (int)n;
  return true;
}

static bool parse_bool(Str v, bool* out) {
  static const char* const yes[] = {"yes", "true", "on", "1"};
  static const char* const no[] = {"no", "false", "off", "0"};
  for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++) {
    if (str_eq_nocase(v, str_from_cstr(yes[i]))) {
      *out = true;
      return true;
    }
    if (str_eq_nocase(v, str_from_cstr(no[i]))) {
      *out = false;
      return true;
    }
  }
  return false;
}

/* #rrggbb or #rrggbbaa */
static bool parse_color(Str v, struct config_color* out) {
  if (!str_chop_prefix(&v, STR("#")) || (v.len != 6 && v.len != 8))
    return false;
  for (size_t i = 0; i < v.len; i++) {
    if (!isxdigit((unsigned char)v.ptr[i]))
      return false;
  }
  uint64_t hex;
  if (!str_to_u64(v, 16, &hex))
    return false;
  if (v.len == 6)
    hex = hex << 8 | 0xff;
  *out = RGBA(hex);
  return true;
}

/* N (pixels) or N% (of the output) */
static bool parse_width(Str v, int* out, bool* percent) {
  bool pct = str_chop_suffix(&v, STR("%"));
  if (!parse_int(str_trim_right(v), 1, pct ? 100 : 100000, out))
    return false;
  *percent = pct;
  return true;
}

/* N[:top|:bottom], N as for border-label-pos */
static bool parse_label_pos(Str v, int* out, bool* bottom) {
  Str n, side;
  str_cut(v, STR(":"), &n, &side);  // no side leaves it empty
  side = str_trim(side);
  bool b;
  if (side.len == 0 || str_eq_cstr(side, "top"))
    b = false;
  else if (str_eq_cstr(side, "bottom"))
    b = true;
  else
    return false;
  if (!parse_int(str_trim(n), -100000, 100000, out))
    return false;
  *bottom = b;
  return true;
}

#define UI(field) offsetof(struct config_ui, field)

const struct config_key config_main_keys[] = {
    {"font", "FONT", CONFIG_STRING, UI(font), 0},
    {"prompt", "TEXT", CONFIG_STRING, UI(prompt), 0},
    {"placeholder", "TEXT", CONFIG_STRING, UI(placeholder), 0},
    {"header", "TEXT", CONFIG_TEXT, UI(header), 0},
    {"border-label", "TEXT", CONFIG_STRING, UI(border_label), 0},
    {"border-label-pos", "N[:top|:bottom]", CONFIG_LABEL_POS,
     UI(border_label_pos), 0},
    {"width", "N|N%", CONFIG_WIDTH, UI(width), 0},
    {"match", "substring|fuzzy", CONFIG_MATCH, UI(fuzzy), 0},
    {"lines", "N", CONFIG_INT, UI(lines), 1},
    {"padding", "N", CONFIG_INT, UI(padding), 0},
    {"row-padding", "N", CONFIG_INT, UI(row_padding), 0},
    {"border-width", "N", CONFIG_INT, UI(border_width), 0},
    {"border-radius", "N", CONFIG_INT, UI(border_radius), 0},
    {"selection-radius", "N", CONFIG_INT, UI(selection_radius), 0},
    {"show-counter", "yes|no", CONFIG_BOOL, UI(show_counter), 0},
};
const size_t config_n_main_keys =
    sizeof config_main_keys / sizeof config_main_keys[0];

#define COLOR(name, field) {name, offsetof(struct config_colors, field)}
const struct config_color_key config_color_keys[] = {
    COLOR("background", background),
    COLOR("border", border),
    COLOR("text", text),
    COLOR("match", match),
    COLOR("selection", selection),
    COLOR("selection-text", selection_text),
    COLOR("selection-match", selection_match),
    COLOR("prompt", prompt),
    COLOR("input", input),
    COLOR("placeholder", placeholder),
    COLOR("cursor", cursor),
    COLOR("counter", counter),
    COLOR("separator", separator),
    COLOR("header", header),
    COLOR("border-label", border_label),
};
#undef COLOR
const size_t config_n_color_keys =
    sizeof config_color_keys / sizeof config_color_keys[0];

static void set_main(struct config_ui* ui,
                     const struct loc* loc,
                     Str key,
                     Str val) {
  const struct config_key* k = NULL;
  for (size_t i = 0; i < config_n_main_keys && k == NULL; i++) {
    if (str_eq_cstr(key, config_main_keys[i].name))
      k = &config_main_keys[i];
  }
  if (k == NULL) {
    PARSE_WARN(loc, "unknown key \"" STR_FMT "\" in [main]", STR_ARG(key));
    return;
  }

  void* field = (char*)ui + k->off;
  switch (k->kind) {
    case CONFIG_STRING:
      set_str(field, val);
      break;
    case CONFIG_TEXT:
      config_unescape(field, val);
      break;
    case CONFIG_INT:
      if (!parse_int(val, k->min, 1000, field))
        PARSE_WARN(loc, "%s must be a number from %d to 1000", k->name, k->min);
      break;
    case CONFIG_BOOL:
      if (!parse_bool(val, field))
        PARSE_WARN(loc, "%s must be yes or no", k->name);
      break;
    case CONFIG_WIDTH:
      if (!parse_width(val, &ui->width, &ui->width_percent))
        PARSE_WARN(loc, "%s",
                   "width must be pixels (600) or a percentage (40%)");
      break;
    case CONFIG_MATCH:
      if (str_eq_cstr(val, "substring"))
        ui->fuzzy = false;
      else if (str_eq_cstr(val, "fuzzy"))
        ui->fuzzy = true;
      else
        PARSE_WARN(loc, "%s", "match must be substring or fuzzy");
      break;
    case CONFIG_LABEL_POS:
      if (!parse_label_pos(val, &ui->border_label_pos,
                           &ui->border_label_bottom))
        PARSE_WARN(loc, "%s",
                   "border-label-pos must be N, N:top or N:bottom (0 centers)");
      break;
  }
}

static void set_color(struct config_colors* c,
                      const struct loc* loc,
                      Str key,
                      Str val) {
  for (size_t i = 0; i < config_n_color_keys; i++) {
    const struct config_color_key* k = &config_color_keys[i];
    if (!str_eq_cstr(key, k->name))
      continue;
    if (!parse_color(val, (struct config_color*)((char*)c + k->off)))
      PARSE_WARN(loc, "%s must be #rrggbb or #rrggbbaa", k->name);
    return;
  }
  PARSE_WARN(loc, "unknown key \"" STR_FMT "\" in [colors]", STR_ARG(key));
}

/* The closing delimiter for an argument opened with c, or 0. */
static char arg_closer(char c) {
  switch (c) {
    case '(':
      return ')';
    case '[':
      return ']';
    case '{':
      return '}';
    case '<':
      return '>';
    default:
      return 0;
  }
}

/* action[+action...], each action a name optionally followed by an argument
 * in (), [], {} or <>; or none. Fills out, whose arguments are copied into
 * arena. Returns false if val is malformed (having warned). */
static bool parse_actions(Arena* arena,
                         const struct loc* loc,
                         Str val,
                         ActionList* out) {
  if (str_eq_nocase(val, STR("none")))
    return true;
  if (val.len == 0) {
    PARSE_WARN(loc, "%s", "expected actions, or none");
    return false;
  }

  Str rest = val;
  for (;;) {
    size_t i = 0;
    while (i < rest.len && rest.ptr[i] != '+' && !arg_closer(rest.ptr[i]))
      i++;
    Str name = str_trim(str_slice(rest, 0, i));
    rest = str_slice(rest, i, rest.len);

    struct action a = {.arg = STR("")};
    if (!action_from_name(name, &a.kind)) {
      PARSE_WARN(loc, "unknown action \"" STR_FMT "\"", STR_ARG(name));
      return false;
    }
    char closer = rest.len > 0 ? arg_closer(rest.ptr[0]) : 0;
    if (closer) {
      size_t end = str_find_char(rest, closer);
      if (end == STR_NPOS) {
        PARSE_WARN(loc, "missing '%c' after " STR_FMT, closer, STR_ARG(name));
        return false;
      }
      a.arg = str_arena_dup(arena, str_slice(rest, 1, end));
      rest = str_slice(rest, end + 1, rest.len);
    }
    if (action_takes_arg(a.kind) != (closer != 0)) {
      PARSE_WARN(loc, STR_FMT " %s", STR_ARG(name),
                 closer ? "takes no argument" : "needs an argument");
      return false;
    }
    LIST_PUSH(out, a);

    rest = str_trim_left(rest);
    if (rest.len == 0)
      return true;
    if (rest.ptr[0] != '+') {
      PARSE_WARN(loc, "expected '+' after " STR_FMT, STR_ARG(name));
      return false;
    }
    rest = str_slice(rest, 1, rest.len);
  }
}

/* Key = actions binds the key, replacing what it did before. A malformed
 * line leaves it as it was. */
static void set_key(struct config* cfg,
                    const struct loc* loc,
                    Str key,
                    Str val) {
  struct config_bind b = {.actions = LIST_INIT_ARENA(&cfg->arena)};
  if (!kb_parse_keypress(key, &b.key)) {
    PARSE_WARN(loc, "bad key \"" STR_FMT "\" in [keys]", STR_ARG(key));
    return;
  }
  if (parse_actions(&cfg->arena, loc, val, &b.actions))
    LIST_PUSH(&cfg->binds, b);
}

enum section { SECTION_NONE, SECTION_MAIN, SECTION_COLORS, SECTION_KEYS };

static bool parse_section(Str name, enum section* out) {
  if (str_eq_cstr(name, "main"))
    *out = SECTION_MAIN;
  else if (str_eq_cstr(name, "colors"))
    *out = SECTION_COLORS;
  else if (str_eq_cstr(name, "keys"))
    *out = SECTION_KEYS;
  else
    return false;
  return true;
}

/* Applies key = val, as given, to section. */
static void set_raw(struct config* cfg,
                    const struct loc* loc,
                    enum section section,
                    Str key,
                    Str val) {
  switch (section) {
    case SECTION_NONE:
      PARSE_WARN(loc, "%s", "key outside of a section");
      break;
    case SECTION_MAIN:
      set_main(&cfg->ui, loc, key, val);
      break;
    case SECTION_COLORS:
      set_color(&cfg->ui.colors, loc, key, val);
      break;
    case SECTION_KEYS:
      set_key(cfg, loc, key, val);
      break;
  }
}

/* set_raw with key and val trimmed and val unquoted. */
static void set_value(struct config* cfg,
                      const struct loc* loc,
                      enum section section,
                      Str key,
                      Str val) {
  key = str_trim(key);
  val = str_trim(val);
  if (val.len >= 2 && val.ptr[0] == '"' && val.ptr[val.len - 1] == '"')
    val = str_slice(val, 1, val.len - 1);
  set_raw(cfg, loc, section, key, val);
}

static void parse(struct config* cfg, const char* path, Str text) {
  struct loc loc = {.path = path};
  enum section section = SECTION_NONE;
  Str line;
  while (str_next_line(&text, &line)) {
    loc.line++;
    line = str_trim(line);
    if (line.len == 0 || line.ptr[0] == '#' || line.ptr[0] == ';')
      continue;

    if (line.ptr[0] == '[') {
      Str name;
      if (!str_chop_suffix(&line, STR("]"))) {
        PARSE_WARN(&loc, "%s", "missing ]");
        section = SECTION_NONE;
        continue;
      }
      name = str_trim(str_slice(line, 1, line.len));
      if (!parse_section(name, &section)) {
        PARSE_WARN(&loc, "unknown section [" STR_FMT "]", STR_ARG(name));
        section = SECTION_NONE;
      }
      continue;
    }

    Str key, val;
    if (!str_cut(line, STR("="), &key, &val)) {
      PARSE_WARN(&loc, "%s", "expected key = value");
      continue;
    }
    set_value(cfg, &loc, section, key, val);
  }
}

void config_set_main(struct config* cfg, const char* key, const char* val) {
  char path[64];
  snprintf(path, sizeof path, "--%s", key);
  struct loc loc = {.path = path};
  set_raw(cfg, &loc, SECTION_MAIN, str_from_cstr(key), str_from_cstr(val));
}

void config_set_colors(struct config* cfg, const char* list) {
  struct loc loc = {.path = "--color"};
  Str rest = str_from_cstr(list), item;
  for (;;) {
    bool more = str_cut(rest, STR(","), &item, &rest);
    Str key, val;
    if (!str_cut(item, STR("="), &key, &val))
      PARSE_WARN(&loc, "expected NAME=#rrggbb[aa], got \"" STR_FMT "\"",
                 STR_ARG(item));
    else
      set_value(cfg, &loc, SECTION_COLORS, key, val);
    if (!more)
      return;
  }
}

void config_set_bind(struct config* cfg, const char* arg) {
  struct loc loc = {.path = "--bind"};
  Str key, val;
  if (!str_cut(str_from_cstr(arg), STR("="), &key, &val)) {
    PARSE_WARN(&loc, "%s", "expected KEY=ACTIONS");
    return;
  }
  set_value(cfg, &loc, SECTION_KEYS, key, val);
}

int config_load_file(struct config* cfg, const char* path) {
  FILE* f = fopen(path, "r");
  if (f == NULL) {
    if (errno == ENOENT)
      return 1;
    LOG_ERROR("%s: %s", path, strerror(errno));
    return -1;
  }

  StrBuf text = STRBUF_INIT;
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    sb_append(&text, str_from_parts(chunk, n));
  bool read_err = ferror(f);
  fclose(f);

  int ret = 0;
  if (read_err) {
    LOG_ERROR("%s: read failed", path);
    ret = -1;
  } else {
    parse(cfg, path, sb_str(&text));
  }
  sb_free(&text);
  return ret;
}

const struct config_bind* config_find_bind(const struct config* cfg,
                                           const struct kb_result* key) {
  for (size_t k = 0; k < key->n_keys; k++) {
    struct kb_keypress kp = key->keys[k];
    for (size_t i = cfg->binds.len; i-- > 0;) {
      const struct config_bind* b = &cfg->binds.ptr[i];
      if (b->key.mods == kp.mods && b->key.sym == kp.sym) {
        if (b->actions.len > 0)
          return b;
        break;  // unbound: try the next keypress
      }
    }
  }
  return NULL;
}

void config_load_defaults(struct config* cfg) {
  static const struct {
    const char* key;
    const char* actions;
  } defaults[] = {
      {"Escape", "cancel"},
      {"Control+g", "cancel"},

      {"BackSpace", "delete-prev"},
      {"Shift+BackSpace", "delete-prev"},
      {"Control+h", "delete-prev"},
      {"Control+w", "delete-prev-word"},
      {"Control+BackSpace", "delete-prev-word"},
      {"Alt+BackSpace", "delete-prev-word"},
      {"Delete", "delete-next"},
      {"KP_Delete", "delete-next"},
      {"Control+d", "delete-next"},
      {"Control+Delete", "delete-next-word"},
      {"Alt+d", "delete-next-word"},
      {"Control+k", "delete-line-end"},
      {"Control+u", "delete-line-start"},

      {"Left", "cursor-left"},
      {"Control+b", "cursor-left"},
      {"Right", "cursor-right"},
      {"Control+f", "cursor-right"},
      {"Control+Left", "cursor-word-left"},
      {"Alt+b", "cursor-word-left"},
      {"Control+Right", "cursor-word-right"},
      {"Alt+f", "cursor-word-right"},
      {"Home", "cursor-start"},
      {"Control+a", "cursor-start"},
      {"End", "cursor-end"},
      {"Control+e", "cursor-end"},

      {"Return", "match-exec"},
      {"KP_Enter", "match-exec"},
      {"Control+n", "match-next"},
      {"Down", "match-next"},
      {"Tab", "match-next"},
      {"Control+p", "match-prev"},
      {"Up", "match-prev"},
      {"Shift+Tab", "match-prev"},
  };
  struct loc loc = {.path = "defaults"};
  for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
    loc.line = i + 1;
    set_key(cfg, &loc, str_from_cstr(defaults[i].key),
            str_from_cstr(defaults[i].actions));
  }
  load_default_ui(&cfg->ui);
}
