#include "kb.h"
#include "ztd_arena.h"
#include "ztd_map.h"

#include <ctype.h>
#include <stdlib.h>

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
