#ifndef LM_KB_H
#define LM_KB_H

#include <stdbool.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon.h>

struct kb_manager;

/* Modifiers a binding can require. Lock modifiers (Caps/Num Lock) are never
 * part of the mask, so they don't affect matching. */
enum kb_mod {
  KB_MOD_CTRL = 1 << 0,
  KB_MOD_SHIFT = 1 << 1,
  KB_MOD_ALT = 1 << 2,
  KB_MOD_SUPER = 1 << 3,
};

enum kb_action {
  KB_ACTION_NONE,
  KB_ACTION_CANCEL,

  KB_ACTION_DELETE_PREV,
  KB_ACTION_DELETE_PREV_WORD,
  KB_ACTION_DELETE_LINE,

  KB_ACTION_MATCH_EXEC,
  KB_ACTION_MATCH_NEXT,
  KB_ACTION_MATCH_PREV,
};

/* sym is the shifted keysym (Shift+n is XKB_KEY_N), mods a mask of kb_mod. */
struct kb_keypress {
  uint32_t mods;
  xkb_keysym_t sym;
};

/* Returns NULL on allocation failure. */
struct kb_manager* kb_manager_new(void);
void kb_manager_destroy(struct kb_manager*);

/* Returns false on allocation failure. */
bool kb_load_bind(struct kb_manager* mgr,
                  struct kb_keypress lhs,
                  enum kb_action rhs);
void kb_unload_bind(struct kb_manager* mgr, struct kb_keypress lhs);
bool kb_load_defaults(struct kb_manager* mgr);

/* Either a bound action, or (action == KB_ACTION_NONE) text to insert; both
 * empty means the key is ignored. utf8 is NUL-terminated. */
struct kb_result {
  enum kb_action action;
  char utf8[8];
};

struct kb_result kb_get_action(struct kb_manager* mgr,
                               uint32_t mods,
                               xkb_keysym_t sym);

#endif  // !LM_KB_H
