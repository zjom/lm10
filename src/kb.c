#include "kb.h"

#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

static bool has_shifted_form(xkb_keysym_t sym) {
  return sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab ||
         xkb_keysym_to_upper(sym) != xkb_keysym_to_lower(sym);
}

struct kb_keypress kb_normalize(uint32_t mods, xkb_keysym_t sym) {
  if ((mods & KB_MOD_SHIFT) && has_shifted_form(sym)) {
    mods &= ~(uint32_t)KB_MOD_SHIFT;
    sym = sym == XKB_KEY_Tab ? XKB_KEY_ISO_Left_Tab : xkb_keysym_to_upper(sym);
  }
  return (struct kb_keypress){.mods = mods, .sym = sym};
}

bool kb_parse_keypress(Str s, struct kb_keypress* out) {
  static const struct {
    const char* name;
    uint32_t mod;
  } mods[] = {
      {"control", KB_MOD_CTRL}, {"ctrl", KB_MOD_CTRL},  {"shift", KB_MOD_SHIFT},
      {"alt", KB_MOD_ALT},      {"mod1", KB_MOD_ALT},   {"super", KB_MOD_SUPER},
      {"logo", KB_MOD_SUPER},   {"mod4", KB_MOD_SUPER},
  };

  Str mod_part, sym_part;
  if (!str_cut_last(s, STR("+"), &mod_part, &sym_part)) {
    mod_part = STR("");
    sym_part = s;
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

  *out = kb_normalize(mask, sym);
  return true;
}

/* Text that may go into the query: no control characters. */
static bool is_text(const char* s) {
  if (s[0] == '\0')
    return false;
  for (; *s != '\0'; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20 || c == 0x7f)
      return false;
  }
  return true;
}

struct kb_result kb_resolve(struct xkb_state* state, xkb_keycode_t key) {
  static const struct {
    const char* name;
    uint32_t mod;
  } mod_names[] = {
      {XKB_MOD_NAME_CTRL, KB_MOD_CTRL},
      {XKB_MOD_NAME_SHIFT, KB_MOD_SHIFT},
      {XKB_MOD_NAME_ALT, KB_MOD_ALT},
      {XKB_MOD_NAME_LOGO, KB_MOD_SUPER},
  };
  struct xkb_keymap* keymap = xkb_state_get_keymap(state);
  struct kb_result r = {0};

  uint32_t active = 0, unconsumed = 0;
  for (size_t i = 0; i < sizeof mod_names / sizeof mod_names[0]; i++) {
    xkb_mod_index_t idx = xkb_keymap_mod_get_index(keymap, mod_names[i].name);
    if (idx == XKB_MOD_INVALID ||
        xkb_state_mod_index_is_active(state, idx, XKB_STATE_MODS_EFFECTIVE) <=
            0)
      continue;
    active |= mod_names[i].mod;
    if (xkb_state_mod_index_is_consumed2(state, key, idx,
                                         XKB_CONSUMED_MODE_XKB) != 1)
      unconsumed |= mod_names[i].mod;
  }

  /* Caps Lock changes the keysym's case, but binds ignore it: undo that,
   * keeping what Shift does. */
  xkb_keysym_t sym = xkb_state_key_get_one_sym(state, key);
  xkb_mod_index_t caps = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS);
  if (caps != XKB_MOD_INVALID &&
      xkb_state_mod_index_is_active(state, caps, XKB_STATE_MODS_EFFECTIVE) >
          0 &&
      xkb_state_mod_index_is_consumed2(state, key, caps,
                                       XKB_CONSUMED_MODE_XKB) == 1)
    sym = active & KB_MOD_SHIFT ? xkb_keysym_to_upper(sym)
                                : xkb_keysym_to_lower(sym);

  struct kb_keypress first = kb_normalize(unconsumed, sym);
  if (sym != XKB_KEY_NoSymbol)
    r.keys[r.n_keys++] = first;

  const xkb_keysym_t* base;
  if (xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &base) == 1) {
    struct kb_keypress second = kb_normalize(active, base[0]);
    if (second.mods != first.mods || second.sym != first.sym)
      r.keys[r.n_keys++] = second;
  }

  if (unconsumed & (KB_MOD_CTRL | KB_MOD_ALT | KB_MOD_SUPER))
    return r;
  if (xkb_state_key_get_utf8(state, key, r.text, sizeof r.text) <= 0 ||
      !is_text(r.text))
    r.text[0] = '\0';
  return r;
}
