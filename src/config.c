#include "config.h"

#include <xkbcommon/xkbcommon-keysyms.h>

void config_init(struct config* cfg) {
  cfg->binds = (ConfigBindList)LIST_INIT;
}

void config_destroy(struct config* cfg) {
  LIST_FREE(&cfg->binds);
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
                     sizeof defaults / sizeof defaults[0]);
}
