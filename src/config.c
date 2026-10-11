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
             .border_label = STRBUF_INIT,
             .icon_theme = STRBUF_INIT},
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
  sb_free(&cfg->ui.icon_theme);
}

static bool set_str(StrBuf* dst, Str s) {
  sb_clear(dst);
  return sb_append(dst, s);
}

bool config_set_header(StrBuf* dst, Str s) {
  sb_clear(dst);
  Str before, after;
  while (str_cut(s, STR("\\n"), &before, &after)) {
    sb_append(dst, before);
    sb_append_char(dst, '\n');
    s = after;
  }
  return sb_append(dst, s);
}

#define RGBA(hex)                                      \
  ((struct config_color){((hex) >> 24 & 0xff) / 255.0, \
                         ((hex) >> 16 & 0xff) / 255.0, \
                         ((hex) >> 8 & 0xff) / 255.0, ((hex) & 0xff) / 255.0})

static bool load_default_ui(struct config_ui* ui) {
  ui->width = 600;
  ui->width_percent = false;
  ui->lines = 8;
  ui->padding = 14;
  ui->row_padding = 8;
  ui->border_width = 2;
  ui->border_radius = 12;
  ui->selection_radius = 8;
  ui->show_counter = true;
  ui->icons = true;
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
  return set_str(&ui->font, STR("Sans 13")) &&
         set_str(&ui->prompt, STR("❯ ")) &&
         set_str(&ui->placeholder, STR("Search…")) &&
         set_str(&ui->header, STR("")) && set_str(&ui->border_label, STR("")) &&
         set_str(&ui->icon_theme, STR("hicolor"));
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
  return !out->failed;
}

/* ---- file parsing ---- */

/* Where a parse error is reported. */
struct loc {
  const char* path;
  size_t line;
};

#define PARSE_WARN(loc, fmt, ...) \
  LOG_WARN("%s:%zu: " fmt, (loc)->path, (loc)->line, __VA_ARGS__)

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

static bool set_main(struct config_ui* ui,
                     const struct loc* loc,
                     Str key,
                     Str val) {
  static const struct {
    const char* name;
    size_t off;
    int min;
  } ints[] = {
      {"lines", offsetof(struct config_ui, lines), 1},
      {"padding", offsetof(struct config_ui, padding), 0},
      {"row-padding", offsetof(struct config_ui, row_padding), 0},
      {"border-width", offsetof(struct config_ui, border_width), 0},
      {"border-radius", offsetof(struct config_ui, border_radius), 0},
      {"selection-radius", offsetof(struct config_ui, selection_radius), 0},
  };
  static const struct {
    const char* name;
    size_t off;
  } strs[] = {
      {"font", offsetof(struct config_ui, font)},
      {"prompt", offsetof(struct config_ui, prompt)},
      {"placeholder", offsetof(struct config_ui, placeholder)},
      {"border-label", offsetof(struct config_ui, border_label)},
      {"icon-theme", offsetof(struct config_ui, icon_theme)},
  };

  for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
    if (!str_eq_cstr(key, ints[i].name))
      continue;
    if (!parse_int(val, ints[i].min, 1000, (int*)((char*)ui + ints[i].off)))
      PARSE_WARN(loc, "%s must be a number from %d to 1000", ints[i].name,
                 ints[i].min);
    return true;
  }
  for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
    if (str_eq_cstr(key, strs[i].name))
      return set_str((StrBuf*)((char*)ui + strs[i].off), val);
  }

  if (str_eq_cstr(key, "header"))
    return config_set_header(&ui->header, val);

  if (str_eq_cstr(key, "width")) {
    if (!parse_width(val, &ui->width, &ui->width_percent))
      PARSE_WARN(loc, "%s", "width must be pixels (600) or a percentage (40%)");
  } else if (str_eq_cstr(key, "border-label-pos")) {
    if (!parse_label_pos(val, &ui->border_label_pos, &ui->border_label_bottom))
      PARSE_WARN(loc, "%s",
                 "border-label-pos must be N, N:top or N:bottom (0 centers)");
  } else if (str_eq_cstr(key, "show-counter")) {
    if (!parse_bool(val, &ui->show_counter))
      PARSE_WARN(loc, "%s", "show-counter must be yes or no");
  } else if (str_eq_cstr(key, "icons")) {
    if (!parse_bool(val, &ui->icons))
      PARSE_WARN(loc, "%s", "icons must be yes or no");
  } else {
    PARSE_WARN(loc, "unknown key \"" STR_FMT "\" in [main]", STR_ARG(key));
  }
  return true;
}

static void set_color(struct config_colors* c,
                      const struct loc* loc,
                      Str key,
                      Str val) {
  static const struct {
    const char* name;
    size_t off;
  } names[] = {
#define C(name, field) {name, offsetof(struct config_colors, field)}
      C("background", background),
      C("border", border),
      C("text", text),
      C("match", match),
      C("selection", selection),
      C("selection-text", selection_text),
      C("selection-match", selection_match),
      C("prompt", prompt),
      C("input", input),
      C("placeholder", placeholder),
      C("cursor", cursor),
      C("counter", counter),
      C("separator", separator),
      C("header", header),
      C("border-label", border_label),
#undef C
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (!str_eq_cstr(key, names[i].name))
      continue;
    struct config_color* dst = (struct config_color*)((char*)c + names[i].off);
    if (!parse_color(val, dst))
      PARSE_WARN(loc, "%s must be #rrggbb or #rrggbbaa", names[i].name);
    return;
  }
  PARSE_WARN(loc, "unknown key \"" STR_FMT "\" in [colors]", STR_ARG(key));
}

static const struct {
  const char* name;
  enum action_kind kind;
  bool takes_arg;
} action_names[] = {
    {"cancel", ACTION_CANCEL, false},
    {"delete-prev", ACTION_DELETE_PREV, false},
    {"delete-prev-word", ACTION_DELETE_PREV_WORD, false},
    {"delete-next", ACTION_DELETE_NEXT, false},
    {"delete-next-word", ACTION_DELETE_NEXT_WORD, false},
    {"delete-line-start", ACTION_DELETE_LINE_START, false},
    {"delete-line-end", ACTION_DELETE_LINE_END, false},
    {"delete-line", ACTION_DELETE_LINE, false},
    {"cursor-left", ACTION_CURSOR_LEFT, false},
    {"cursor-right", ACTION_CURSOR_RIGHT, false},
    {"cursor-word-left", ACTION_CURSOR_WORD_LEFT, false},
    {"cursor-word-right", ACTION_CURSOR_WORD_RIGHT, false},
    {"cursor-start", ACTION_CURSOR_START, false},
    {"cursor-end", ACTION_CURSOR_END, false},
    {"match-exec", ACTION_MATCH_EXEC, false},
    {"match-next", ACTION_MATCH_NEXT, false},
    {"match-prev", ACTION_MATCH_PREV, false},
    {"change-prompt", ACTION_CHANGE_PROMPT, true},
    {"change-query", ACTION_CHANGE_QUERY, true},
    {"change-header", ACTION_CHANGE_HEADER, true},
    {"change-border-label", ACTION_CHANGE_BORDER_LABEL, true},
    {"reload", ACTION_RELOAD, true},
    {"execute", ACTION_EXECUTE, true},
};

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
 * arena. Returns 1, 0 if val is malformed (having warned), or -1 on
 * allocation failure. */
static int parse_actions(Arena* arena,
                         const struct loc* loc,
                         Str val,
                         ActionList* out) {
  if (str_eq_nocase(val, STR("none")))
    return 1;
  if (val.len == 0) {
    PARSE_WARN(loc, "%s", "expected actions, or none");
    return 0;
  }

  Str rest = val;
  for (;;) {
    size_t i = 0;
    while (i < rest.len && rest.ptr[i] != '+' && !arg_closer(rest.ptr[i]))
      i++;
    Str name = str_trim(str_slice(rest, 0, i));
    rest = str_slice(rest, i, rest.len);

    size_t n = 0;
    while (n < sizeof action_names / sizeof action_names[0] &&
           !str_eq_cstr(name, action_names[n].name))
      n++;
    if (n == sizeof action_names / sizeof action_names[0]) {
      PARSE_WARN(loc, "unknown action \"" STR_FMT "\"", STR_ARG(name));
      return 0;
    }

    struct action a = {.kind = action_names[n].kind, .arg = STR("")};
    char closer = rest.len > 0 ? arg_closer(rest.ptr[0]) : 0;
    if (closer) {
      size_t end = str_find_char(rest, closer);
      if (end == STR_NPOS) {
        PARSE_WARN(loc, "missing '%c' after " STR_FMT, closer, STR_ARG(name));
        return 0;
      }
      a.arg = str_arena_dup(arena, str_slice(rest, 1, end));
      if (a.arg.ptr == NULL)
        return -1;
      rest = str_slice(rest, end + 1, rest.len);
    }
    if (action_names[n].takes_arg != (closer != 0)) {
      PARSE_WARN(loc, STR_FMT " %s", STR_ARG(name),
                 closer ? "takes no argument" : "needs an argument");
      return 0;
    }
    if (!LIST_PUSH(out, a))
      return -1;

    rest = str_trim_left(rest);
    if (rest.len == 0)
      return 1;
    if (rest.ptr[0] != '+') {
      PARSE_WARN(loc, "expected '+' after " STR_FMT, STR_ARG(name));
      return 0;
    }
    rest = str_slice(rest, 1, rest.len);
  }
}

/* Key = actions binds the key, replacing what it did before. A malformed
 * line leaves it as it was. Returns false on allocation failure. */
static bool set_key(struct config* cfg,
                    const struct loc* loc,
                    Str key,
                    Str val) {
  struct config_bind b = {.actions = LIST_INIT_ARENA(&cfg->arena)};
  if (!kb_parse_keypress(key, &b.key)) {
    PARSE_WARN(loc, "bad key \"" STR_FMT "\" in [keys]", STR_ARG(key));
    return true;
  }
  int ret = parse_actions(&cfg->arena, loc, val, &b.actions);
  if (ret <= 0)
    return ret == 0;
  return LIST_PUSH(&cfg->binds, b);
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

/* Applies key = val (both trimmed, val unquoted) to section. Returns false on
 * allocation failure. */
static bool set_value(struct config* cfg,
                      const struct loc* loc,
                      enum section section,
                      Str key,
                      Str val) {
  key = str_trim(key);
  val = str_trim(val);
  if (val.len >= 2 && val.ptr[0] == '"' && val.ptr[val.len - 1] == '"')
    val = str_slice(val, 1, val.len - 1);

  switch (section) {
    case SECTION_NONE:
      PARSE_WARN(loc, "%s", "key outside of a section");
      break;
    case SECTION_MAIN:
      return set_main(&cfg->ui, loc, key, val);
    case SECTION_COLORS:
      set_color(&cfg->ui.colors, loc, key, val);
      break;
    case SECTION_KEYS:
      return set_key(cfg, loc, key, val);
  }
  return true;
}

/* Returns false on allocation failure. */
static bool parse(struct config* cfg, const char* path, Str text) {
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
    if (!set_value(cfg, &loc, section, key, val))
      return false;
  }
  return true;
}

bool config_apply_override(struct config* cfg, size_t n, const char* arg) {
  struct loc loc = {.path = "-o", .line = n};
  Str lhs, val;
  if (!str_cut(str_from_cstr(arg), STR("="), &lhs, &val)) {
    PARSE_WARN(&loc, "%s", "expected [section.]key=value");
    return true;
  }

  enum section section = SECTION_MAIN;
  Str name, key;
  if (str_cut(lhs, STR("."), &name, &key)) {
    name = str_trim(name);
    if (!parse_section(name, &section)) {
      PARSE_WARN(&loc, "unknown section [" STR_FMT "]", STR_ARG(name));
      return true;
    }
  } else {
    key = lhs;
  }
  return set_value(cfg, &loc, section, key, val);
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
  } else if (text.failed || !parse(cfg, path, sb_str(&text))) {
    LOG_ERROR("%s: allocation failed", path);
    ret = -1;
  }
  sb_free(&text);
  return ret;
}

bool config_load_defaults(struct config* cfg) {
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
    if (!set_key(cfg, &loc, str_from_cstr(defaults[i].key),
                 str_from_cstr(defaults[i].actions)))
      return false;
  }
  return load_default_ui(&cfg->ui);
}
