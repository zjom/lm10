#include "kb.h"
#include "ztd_arena.h"
#include "ztd_map.h"

#include <ctype.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

/* Hashed bytewise: struct kb_keypress has no padding. */
typedef MAP(struct kb_keypress, enum kb_action) KeybindMap;

struct kb_manager {
  Arena arena;
  KeybindMap binds;
};

struct kb_manager* kb_manager_new(void) {
  struct kb_manager* mgr = calloc(1, sizeof(struct kb_manager));
  if (mgr == NULL)
    return NULL;
  /* The map keeps a pointer to the arena, so it must be the one in mgr. */
  mgr->arena = (Arena)ARENA_INIT;
  mgr->binds = (KeybindMap)MAP_INIT_ARENA(&mgr->arena);
  return mgr;
}

void kb_manager_destroy(struct kb_manager* mgr) {
  if (mgr == NULL)
    return;
  arena_free(&mgr->arena);
  free(mgr);
}

bool kb_load_bind(struct kb_manager* mgr,
                  struct kb_keypress lhs,
                  enum kb_action rhs) {
  return MAP_PUT(&mgr->binds, lhs, rhs);
}

void kb_unload_bind(struct kb_manager* mgr, struct kb_keypress lhs) {
  MAP_REMOVE(&mgr->binds, lhs);
}

bool kb_load_defaults(struct kb_manager* mgr) {
  static const struct {
    struct kb_keypress key;
    enum kb_action action;
  } defaults[] = {
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
  for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
    if (!kb_load_bind(mgr, defaults[i].key, defaults[i].action))
      return false;
  }
  return true;
}

struct kb_result kb_get_action(struct kb_manager* mgr,
                               uint32_t mods,
                               xkb_keysym_t sym) {
  struct kb_keypress key = {.mods = mods, .sym = sym};
  struct kb_result result = {.action =
                                 MAP_GET_OR(&mgr->binds, key, KB_ACTION_NONE)};
  if (result.action != KB_ACTION_NONE)
    return result;

  /* Unbound chords don't type anything; Shift alone still does. */
  if (mods & (KB_MOD_CTRL | KB_MOD_ALT | KB_MOD_SUPER))
    return result;

  /* ASCII only for now: one printable byte. */
  if (xkb_keysym_to_utf8(sym, result.utf8, sizeof result.utf8) != 2 ||
      !isprint((unsigned char)result.utf8[0]))
    result.utf8[0] = '\0';
  return result;
}
