#ifndef LM_KB_H
#define LM_KB_H

#include <stdbool.h>
#include <stdint.h>

#include "ztd_str.h"

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

/* sym is the shifted keysym (Shift+n is XKB_KEY_N), mods a mask of kb_mod. */
struct kb_keypress {
  uint32_t mods;
  xkb_keysym_t sym;
};

/* Returns NULL on allocation failure. */
struct kb_manager* kb_manager_new(void);
void kb_manager_destroy(struct kb_manager*);

/* Mod+...+Keysym, e.g. Control+Shift+n. Mods are Control (Ctrl), Shift,
 * Alt (Mod1) and Super (Logo, Mod4), case-insensitive; the keysym is an xkb
 * name, matched exactly first and then ignoring case. The result is
 * normalized to what the keyboard reports: Shift+n is {SHIFT, N}, and an
 * uppercase keysym implies Shift. Returns false if s isn't a key. */
bool kb_parse_keypress(Str s, struct kb_keypress* out);

/* Binds lhs to bind, an index the caller gives meaning to. Returns false on
 * allocation failure. */
bool kb_load_bind(struct kb_manager* mgr,
                  struct kb_keypress lhs,
                  uint32_t bind);
void kb_unload_bind(struct kb_manager* mgr, struct kb_keypress lhs);

/* Either a bound key (bound, with its bind index), or text to insert; neither
 * means the key is ignored. utf8 is NUL-terminated. */
struct kb_result {
  bool bound;
  uint32_t bind;
  char utf8[8];
};

struct kb_result kb_lookup(struct kb_manager* mgr,
                           uint32_t mods,
                           xkb_keysym_t sym);

#endif  // !LM_KB_H
