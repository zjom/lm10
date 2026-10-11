#ifndef LM_KB_H
#define LM_KB_H

#include <stdbool.h>
#include <stdint.h>

#include "ztd_str.h"

#include <xkbcommon/xkbcommon.h>

struct kb_manager;

/* Modifiers a binding can require. Lock modifiers (Caps/Num Lock) are never
 * part of the mask. */
enum kb_mod {
  KB_MOD_CTRL = 1 << 0,
  KB_MOD_SHIFT = 1 << 1,
  KB_MOD_ALT = 1 << 2,
  KB_MOD_SUPER = 1 << 3,
};

/* A key and its modifiers, normalized (see kb_normalize) so that a bind and
 * the key press it matches compare equal. */
struct kb_keypress {
  uint32_t mods;
  xkb_keysym_t sym;
};

/* Folds Shift into keysyms that have a shifted form: Shift+a and A are both
 * {0, A}, Shift+Tab is {0, ISO_Left_Tab}. Shift stays a modifier for keys
 * without one, like Shift+BackSpace. */
struct kb_keypress kb_normalize(uint32_t mods, xkb_keysym_t sym);

/* Returns NULL on allocation failure. */
struct kb_manager* kb_manager_new(void);
void kb_manager_destroy(struct kb_manager*);

/* Mod+...+Keysym, e.g. Control+Shift+n. Mods are Control (Ctrl), Shift,
 * Alt (Mod1) and Super (Logo, Mod4), case-insensitive; the keysym is an xkb
 * name, matched exactly first and then ignoring case. The result is
 * normalized. Returns false if s isn't a key. */
bool kb_parse_keypress(Str s, struct kb_keypress* out);

/* Binds lhs to bind, an index the caller gives meaning to. Returns false on
 * allocation failure. */
bool kb_load_bind(struct kb_manager* mgr,
                  struct kb_keypress lhs,
                  uint32_t bind);
void kb_unload_bind(struct kb_manager* mgr, struct kb_keypress lhs);

/* Either a bound key (bound, with its bind index), or text to insert; neither
 * means the key is ignored. text is NUL-terminated UTF-8. */
struct kb_result {
  bool bound;
  uint32_t bind;
  char text[64];
};

/* Resolves a pressed key in the keyboard's current state:
 *
 *  1. the keysym it produces, with the modifiers that produced it left out,
 *     so Control+plus matches Ctrl+Shift+= on a US layout;
 *     Caps Lock's effect on the keysym's case is ignored;
 *  2. failing that, the keysym it has with no modifiers in the first layout,
 *     with every active modifier, so binds work on non-Latin layouts
 *     (Control+w on a Russian one) and as Shift+1 or Shift+BackSpace;
 *  3. failing that, the text it types, unless Control, Alt or Super is held.
 */
struct kb_result kb_resolve(struct kb_manager* mgr,
                            struct xkb_state* state,
                            xkb_keycode_t key);

#endif  // !LM_KB_H
