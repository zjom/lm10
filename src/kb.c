#include "kb.h"
#include "ztd_arena.h"
#include "ztd_map.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

/* Hashed bytewise: struct kb_keypress has no padding. */
typedef MAP(struct kb_keypress, uint32_t) KeybindMap;

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

  if (xkb_keysym_to_lower(sym) != sym)
    mask |= KB_MOD_SHIFT;
  if (mask & KB_MOD_SHIFT) {
    sym = sym == XKB_KEY_Tab ? XKB_KEY_ISO_Left_Tab : xkb_keysym_to_upper(sym);
  }

  *out = (struct kb_keypress){.mods = mask, .sym = sym};
  return true;
}

bool kb_load_bind(struct kb_manager* mgr,
                  struct kb_keypress lhs,
                  uint32_t bind) {
  return MAP_PUT(&mgr->binds, lhs, bind);
}

void kb_unload_bind(struct kb_manager* mgr, struct kb_keypress lhs) {
  MAP_REMOVE(&mgr->binds, lhs);
}

struct kb_result kb_lookup(struct kb_manager* mgr,
                           uint32_t mods,
                           xkb_keysym_t sym) {
  struct kb_keypress key = {.mods = mods, .sym = sym};
  struct kb_result result = {0};
  const uint32_t* bind = MAP_GET(&mgr->binds, key);
  if (bind != NULL) {
    result.bound = true;
    result.bind = *bind;
    return result;
  }

  /* Unbound chords don't type anything; Shift alone still does. */
  if (mods & (KB_MOD_CTRL | KB_MOD_ALT | KB_MOD_SUPER))
    return result;

  /* ASCII only for now: one printable byte. */
  if (xkb_keysym_to_utf8(sym, result.utf8, sizeof result.utf8) != 2 ||
      !isprint((unsigned char)result.utf8[0]))
    result.utf8[0] = '\0';
  return result;
}
