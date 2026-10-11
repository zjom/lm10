#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include "log.h"

void config_init(struct config* cfg) {
  *cfg = (struct config){
      .binds = LIST_INIT,
      .ui = {.font = STRBUF_INIT,
             .prompt = STRBUF_INIT,
             .placeholder = STRBUF_INIT,
             .icon_theme = STRBUF_INIT},
  };
}

void config_destroy(struct config* cfg) {
  LIST_FREE(&cfg->binds);
  sb_free(&cfg->ui.font);
  sb_free(&cfg->ui.prompt);
  sb_free(&cfg->ui.placeholder);
  sb_free(&cfg->ui.icon_theme);
}

static bool set_str(StrBuf* dst, Str s) {
  sb_clear(dst);
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
  };
  return set_str(&ui->font, STR("Sans 13")) &&
         set_str(&ui->prompt, STR("❯ ")) &&
         set_str(&ui->placeholder, STR("Search…")) &&
         set_str(&ui->icon_theme, STR("hicolor"));
}

bool config_load_defaults(struct config* cfg) {
  static const struct config_bind defaults[] = {
      {{0, XKB_KEY_Escape}, KB_ACTION_CANCEL},
      {{KB_MOD_CTRL, XKB_KEY_g}, KB_ACTION_CANCEL},

      {{0, XKB_KEY_BackSpace}, KB_ACTION_DELETE_PREV},
      {{KB_MOD_SHIFT, XKB_KEY_BackSpace}, KB_ACTION_DELETE_PREV},
      {{KB_MOD_CTRL, XKB_KEY_h}, KB_ACTION_DELETE_PREV},
      {{KB_MOD_CTRL, XKB_KEY_w}, KB_ACTION_DELETE_PREV_WORD},
      {{KB_MOD_CTRL, XKB_KEY_BackSpace}, KB_ACTION_DELETE_PREV_WORD},
      {{KB_MOD_ALT, XKB_KEY_BackSpace}, KB_ACTION_DELETE_PREV_WORD},
      {{0, XKB_KEY_Delete}, KB_ACTION_DELETE_NEXT},
      {{0, XKB_KEY_KP_Delete}, KB_ACTION_DELETE_NEXT},
      {{KB_MOD_CTRL, XKB_KEY_d}, KB_ACTION_DELETE_NEXT},
      {{KB_MOD_CTRL, XKB_KEY_Delete}, KB_ACTION_DELETE_NEXT_WORD},
      {{KB_MOD_ALT, XKB_KEY_d}, KB_ACTION_DELETE_NEXT_WORD},
      {{KB_MOD_CTRL, XKB_KEY_k}, KB_ACTION_DELETE_LINE_END},
      {{KB_MOD_CTRL, XKB_KEY_u}, KB_ACTION_DELETE_LINE_START},

      {{0, XKB_KEY_Left}, KB_ACTION_CURSOR_LEFT},
      {{KB_MOD_CTRL, XKB_KEY_b}, KB_ACTION_CURSOR_LEFT},
      {{0, XKB_KEY_Right}, KB_ACTION_CURSOR_RIGHT},
      {{KB_MOD_CTRL, XKB_KEY_f}, KB_ACTION_CURSOR_RIGHT},
      {{KB_MOD_CTRL, XKB_KEY_Left}, KB_ACTION_CURSOR_WORD_LEFT},
      {{KB_MOD_ALT, XKB_KEY_b}, KB_ACTION_CURSOR_WORD_LEFT},
      {{KB_MOD_CTRL, XKB_KEY_Right}, KB_ACTION_CURSOR_WORD_RIGHT},
      {{KB_MOD_ALT, XKB_KEY_f}, KB_ACTION_CURSOR_WORD_RIGHT},
      {{0, XKB_KEY_Home}, KB_ACTION_CURSOR_START},
      {{KB_MOD_CTRL, XKB_KEY_a}, KB_ACTION_CURSOR_START},
      {{0, XKB_KEY_End}, KB_ACTION_CURSOR_END},
      {{KB_MOD_CTRL, XKB_KEY_e}, KB_ACTION_CURSOR_END},

      {{0, XKB_KEY_Return}, KB_ACTION_MATCH_EXEC},
      {{0, XKB_KEY_KP_Enter}, KB_ACTION_MATCH_EXEC},
      {{KB_MOD_CTRL, XKB_KEY_n}, KB_ACTION_MATCH_NEXT},
      {{0, XKB_KEY_Down}, KB_ACTION_MATCH_NEXT},
      {{0, XKB_KEY_Tab}, KB_ACTION_MATCH_NEXT},
      {{KB_MOD_CTRL, XKB_KEY_p}, KB_ACTION_MATCH_PREV},
      {{0, XKB_KEY_Up}, KB_ACTION_MATCH_PREV},
      {{KB_MOD_SHIFT, XKB_KEY_ISO_Left_Tab}, KB_ACTION_MATCH_PREV},
  };
  return LIST_APPEND(&cfg->binds, defaults,
                     sizeof defaults / sizeof defaults[0]) &&
         load_default_ui(&cfg->ui);
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

  if (str_eq_cstr(key, "width")) {
    if (!parse_width(val, &ui->width, &ui->width_percent))
      PARSE_WARN(loc, "%s", "width must be pixels (600) or a percentage (40%)");
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

static bool parse_action(Str v, enum kb_action* out) {
  static const struct {
    const char* name;
    enum kb_action action;
  } names[] = {
      {"cancel", KB_ACTION_CANCEL},
      {"delete-prev", KB_ACTION_DELETE_PREV},
      {"delete-prev-word", KB_ACTION_DELETE_PREV_WORD},
      {"delete-next", KB_ACTION_DELETE_NEXT},
      {"delete-next-word", KB_ACTION_DELETE_NEXT_WORD},
      {"delete-line-start", KB_ACTION_DELETE_LINE_START},
      {"delete-line-end", KB_ACTION_DELETE_LINE_END},
      {"delete-line", KB_ACTION_DELETE_LINE},
      {"cursor-left", KB_ACTION_CURSOR_LEFT},
      {"cursor-right", KB_ACTION_CURSOR_RIGHT},
      {"cursor-word-left", KB_ACTION_CURSOR_WORD_LEFT},
      {"cursor-word-right", KB_ACTION_CURSOR_WORD_RIGHT},
      {"cursor-start", KB_ACTION_CURSOR_START},
      {"cursor-end", KB_ACTION_CURSOR_END},
      {"match-exec", KB_ACTION_MATCH_EXEC},
      {"match-next", KB_ACTION_MATCH_NEXT},
      {"match-prev", KB_ACTION_MATCH_PREV},
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (str_eq_cstr(v, names[i].name)) {
      *out = names[i].action;
      return true;
    }
  }
  return false;
}

/* Mod+...+Keysym, e.g. Control+Shift+n. Mods are case-insensitive; the keysym
 * is an xkb name, matched exactly first and then ignoring case. The result is
 * normalized to what the keyboard reports: Shift+n is {SHIFT, N}, and an
 * uppercase keysym implies Shift. */
static bool parse_keypress(Str v, struct kb_keypress* out) {
  static const struct {
    const char* name;
    uint32_t mod;
  } mods[] = {
      {"control", KB_MOD_CTRL}, {"ctrl", KB_MOD_CTRL},  {"shift", KB_MOD_SHIFT},
      {"alt", KB_MOD_ALT},      {"mod1", KB_MOD_ALT},   {"super", KB_MOD_SUPER},
      {"logo", KB_MOD_SUPER},   {"mod4", KB_MOD_SUPER},
  };

  Str mod_part, sym_part;
  if (!str_cut_last(v, STR("+"), &mod_part, &sym_part)) {
    mod_part = STR("");
    sym_part = v;
  }

  uint32_t mask = 0;
  if (mod_part.len > 0) {
    StrSplit it = str_split(mod_part, STR("+"));
    Str name;
    while (str_split_next(&it, &name)) {
      size_t i = 0;
      while (i < sizeof mods / sizeof mods[0] &&
             !str_eq_nocase(name, str_from_cstr(mods[i].name)))
        i++;
      if (i == sizeof mods / sizeof mods[0])
        return false;
      mask |= mods[i].mod;
    }
  }

  /* xkb wants a NUL-terminated name; keysym names are short. */
  char buf[64];
  if (sym_part.len == 0 || sym_part.len >= sizeof buf)
    return false;
  memcpy(buf, sym_part.ptr, sym_part.len);
  buf[sym_part.len] = '\0';
  xkb_keysym_t sym = xkb_keysym_from_name(buf, XKB_KEYSYM_NO_FLAGS);
  if (sym == XKB_KEY_NoSymbol)
    sym = xkb_keysym_from_name(buf, XKB_KEYSYM_CASE_INSENSITIVE);
  if (sym == XKB_KEY_NoSymbol)
    return false;

  if (xkb_keysym_to_lower(sym) != sym)
    mask |= KB_MOD_SHIFT;
  if (mask & KB_MOD_SHIFT) {
    sym = sym == XKB_KEY_Tab ? XKB_KEY_ISO_Left_Tab : xkb_keysym_to_upper(sym);
  }

  *out = (struct kb_keypress){.mods = mask, .sym = sym};
  return true;
}

/* action = Key [Key ...] replaces every bind of that action, so it drops the
 * defaults for it; "none" leaves the action unbound. Returns false on
 * allocation failure. */
static bool set_key(ConfigBindList* binds,
                    const struct loc* loc,
                    Str key,
                    Str val) {
  enum kb_action action;
  if (!parse_action(key, &action)) {
    PARSE_WARN(loc, "unknown action \"" STR_FMT "\" in [keys]", STR_ARG(key));
    return true;
  }

  bool none = str_eq_nocase(val, STR("none"));
  if (!none) {
    /* Check every key first so a typo doesn't drop the existing binds. */
    Str rest = val, field;
    struct kb_keypress kp;
    if (val.len == 0) {
      PARSE_WARN(loc, STR_FMT " needs at least one key, or none", STR_ARG(key));
      return true;
    }
    while (str_next_field(&rest, &field)) {
      if (!parse_keypress(field, &kp)) {
        PARSE_WARN(loc, "bad key \"" STR_FMT "\"", STR_ARG(field));
        return true;
      }
    }
  }

  size_t kept = 0;
  for (size_t i = 0; i < binds->len; i++) {
    if (binds->ptr[i].action != action)
      binds->ptr[kept++] = binds->ptr[i];
  }
  LIST_TRUNCATE(binds, kept);

  if (none)
    return true;
  Str rest = val, field;
  while (str_next_field(&rest, &field)) {
    struct config_bind b = {.action = action};
    parse_keypress(field, &b.key);
    if (!LIST_PUSH(binds, b))
      return false;
  }
  return true;
}

enum section { SECTION_NONE, SECTION_MAIN, SECTION_COLORS, SECTION_KEYS };

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
      if (str_eq_cstr(name, "main")) {
        section = SECTION_MAIN;
      } else if (str_eq_cstr(name, "colors")) {
        section = SECTION_COLORS;
      } else if (str_eq_cstr(name, "keys")) {
        section = SECTION_KEYS;
      } else {
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
    key = str_trim(key);
    val = str_trim(val);
    if (val.len >= 2 && val.ptr[0] == '"' && val.ptr[val.len - 1] == '"')
      val = str_slice(val, 1, val.len - 1);

    switch (section) {
      case SECTION_NONE:
        PARSE_WARN(&loc, "%s", "key outside of a section");
        break;
      case SECTION_MAIN:
        if (!set_main(&cfg->ui, &loc, key, val))
          return false;
        break;
      case SECTION_COLORS:
        set_color(&cfg->ui.colors, &loc, key, val);
        break;
      case SECTION_KEYS:
        if (!set_key(&cfg->binds, &loc, key, val))
          return false;
        break;
    }
  }
  return true;
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
