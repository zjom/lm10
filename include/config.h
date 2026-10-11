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
  struct config_color header;
  struct config_color border_label;
};

/* Sizes are in logical pixels. */
struct config_ui {
  StrBuf font;  // pango font description, e.g. "Sans 13"
  /* prompt, header and border_label are the initial values; actions change
   * the state's copies. */
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
  bool fuzzy;            // match the query as a subsequence, best matches first
  bool show_counter;     // "matches/entries" at the right of the query row
  StrBuf header;         // lines between the query and the rows; may be empty
  StrBuf border_label;   // drawn in a gap in the border; may be empty
  int border_label_pos;  // 0 centers it, N > 0 puts it N pixels from the left
                         //   edge, N < 0 -N pixels from the right one
  bool border_label_bottom;  // on the bottom border instead of the top
  struct config_colors colors;
};

/* How a [main] key's value is read. */
enum config_kind {
  CONFIG_STRING,
  CONFIG_TEXT,       // a string in which \n breaks lines
  CONFIG_INT,        // from min to 1000
  CONFIG_BOOL,       // yes/no, true/false, on/off, 1/0
  CONFIG_WIDTH,      // sets width and width_percent
  CONFIG_LABEL_POS,  // sets border_label_pos and border_label_bottom
  CONFIG_MATCH,      // substring|fuzzy, sets fuzzy
};

struct config_key {
  const char* name;
  const char* arg;  // what the value looks like, for --help
  enum config_kind kind;
  size_t off;  // of its field in struct config_ui
  int min;     // CONFIG_INT only
};

/* Every [main] key; each can also be set with --KEY VALUE. */
extern const struct config_key config_main_keys[];
extern const size_t config_n_main_keys;

struct config_color_key {
  const char* name;
  size_t off;  // of its field in struct config_colors
};

/* Every [colors] key. */
extern const struct config_color_key config_color_keys[];
extern const size_t config_n_color_keys;

struct config {
  Arena arena;           // owns the binds' action lists and arguments
  ConfigBindList binds;  // Binds are applied in order, so a later bind for the
                         // same key overrides an earlier one.
  struct config_ui ui;
};

void config_init(struct config* cfg);
void config_destroy(struct config* cfg);

/* Appends the built-in binds and sets the built-in ui. */
void config_load_defaults(struct config* cfg);

/* The default config file: $XDG_CONFIG_HOME/lm10/lm10.ini, falling back to
 * ~/.config/lm10/lm10.ini. Returns false if neither variable is set. */
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

/* Command-line flags, applied on top of what cfg already holds; values are read
 * as in the file, and bad ones are skipped with a warning. */

/* --KEY VAL sets [main] KEY to VAL, taken as is: not trimmed or unquoted. */
void config_set_main(struct config* cfg, const char* key, const char* val);

/* --color NAME=VALUE[,NAME=VALUE...] sets [colors] keys. */
void config_set_colors(struct config* cfg, const char* list);

/* --bind KEY=ACTIONS binds a key as a [keys] line does. */
void config_set_bind(struct config* cfg, const char* arg);

/* The bind for one of key's keypresses, trying them in order: for each, the
 * last bind for it wins. NULL if none is bound, or bound to none. */
const struct config_bind* config_find_bind(const struct config* cfg,
                                           const struct kb_result* key);

/* Sets dst to s with each \n turned into a line break, as headers are
 * written. */
void config_unescape(StrBuf* dst, Str s);

#endif  // !LM_CONFIG_H
