#ifndef LM_CONFIG_H
#define LM_CONFIG_H

#include "action.h"
#include "kb.h"
#include "ztd_arena.h"
#include "ztd_arr.h"
#include "ztd_str.h"

#include <stdbool.h>
#include <stddef.h>

/* A key and the actions it runs, in order; none means it is unbound. */
struct config_bind {
  struct kb_keypress key;
  ActionList actions;  // lives in the config arena
};

typedef LIST(struct config_bind) ConfigBindList;

/* Straight (not premultiplied) RGBA, each channel in [0, 1]. */
struct config_color {
  double r, g, b, a;
};

struct config_colors {
  struct config_color background;
  struct config_color border;
  struct config_color text;            // unselected rows
  struct config_color match;           // matched part of unselected rows
  struct config_color selection;       // selected row background
  struct config_color selection_text;  // selected row
  struct config_color selection_match;
  struct config_color prompt;
  struct config_color input;  // query text
  struct config_color placeholder;
  struct config_color cursor;
  struct config_color counter;
  struct config_color separator;
};

/* Sizes are in logical pixels. */
struct config_ui {
  StrBuf font;  // pango font description, e.g. "Sans 13"
  StrBuf prompt;
  StrBuf placeholder;  // shown while the query is empty
  int width;           // pixels, or a percentage of the output if
  bool width_percent;  //   width_percent
  int lines;           // visible rows; the surface height follows from it
  int padding;         // between the panel edge and its contents
  int row_padding;     // around the text inside each row
  int border_width;
  int border_radius;
  int selection_radius;
  bool show_counter;  // "matches/entries" at the right of the query row
  bool icons;         // draw entry icons
  StrBuf icon_theme;  // freedesktop icon theme to look them up in
  struct config_colors colors;
};

struct config {
  Arena arena;           // owns the binds' action lists and arguments
  ConfigBindList binds;  // Binds are applied in order, so a later bind for the
                         // same key overrides an earlier one.
  struct config_ui ui;
};

void config_init(struct config* cfg);
void config_destroy(struct config* cfg);

/* Appends the built-in binds and sets the built-in ui. Returns false on
 * allocation failure. */
bool config_load_defaults(struct config* cfg);

/* The default config file: $XDG_CONFIG_HOME/lm10/lm10.ini, falling back to
 * ~/.config/lm10/lm10.ini. Returns false if neither variable is set or on
 * allocation failure. */
bool config_default_path(StrBuf* out);

/* Applies an INI file on top of what cfg already holds:
 *
 *   # comment
 *   [main]
 *   font = Sans 13
 *   width = 40%
 *   [colors]
 *   background = #1e1e2ef0
 *   [keys]
 *   Control+n = match-next
 *   Control+r = change-prompt(> )+reload(ls ~)
 *   Control+g = none
 *
 * Each [keys] line binds one key (see kb_parse_keypress) to a list of
 * actions joined by '+', run in order; "none" unbinds it. Actions taking an
 * argument wrap it in (), [], {} or <>, ending at the first closing one.
 *
 * Values may be wrapped in double quotes to keep surrounding spaces. Bad
 * lines are skipped with a warning. Returns 0, 1 if the file doesn't exist,
 * or -1 if it couldn't be read. */
int config_load_file(struct config* cfg, const char* path);

/* Applies one command-line override, [section.]key=value, on top of what cfg
 * already holds; the section defaults to main, and the value is read as in
 * the file. n numbers the override in warnings. Bad overrides are skipped with
 * a warning. Returns false on allocation failure. */
bool config_apply_override(struct config* cfg, size_t n, const char* arg);

#endif  // !LM_CONFIG_H
