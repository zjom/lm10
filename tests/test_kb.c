#include "kb.h"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

#include "test.h"

/* Parses s, checking it gives mods and sym. */
static void check_key(const char* s, uint32_t mods, xkb_keysym_t sym) {
  struct kb_keypress k;
  bool ok = kb_parse_keypress(str_from_cstr(s), &k);
  CHECK(ok);
  if (ok && (k.mods != mods || k.sym != sym)) {
    fprintf(stderr, "  %s: got mods %#x sym %#x, expected %#x %#x\n", s, k.mods,
            k.sym, mods, sym);
    test_failures++;
  }
}

static bool parses(const char* s) {
  struct kb_keypress k;
  return kb_parse_keypress(str_from_cstr(s), &k);
}

static void test_parse(void) {
  check_key("a", 0, XKB_KEY_a);
  check_key("Return", 0, XKB_KEY_Return);
  check_key("Control+n", KB_MOD_CTRL, XKB_KEY_n);
  check_key("ctrl+alt+super+x", KB_MOD_CTRL | KB_MOD_ALT | KB_MOD_SUPER,
            XKB_KEY_x);
  check_key("Mod1+Mod4+Logo+b", KB_MOD_ALT | KB_MOD_SUPER, XKB_KEY_b);
  check_key("Control+plus", KB_MOD_CTRL, XKB_KEY_plus);
  /* Keysym names are matched exactly first, then ignoring case. */
  check_key("return", 0, XKB_KEY_Return);
  check_key("KP_Enter", 0, XKB_KEY_KP_Enter);
}

static void test_shift(void) {
  /* Shift folds into keysyms that have a shifted form. */
  check_key("Shift+n", 0, XKB_KEY_N);
  check_key("N", 0, XKB_KEY_N);
  check_key("Shift+N", 0, XKB_KEY_N);
  check_key("Control+N", KB_MOD_CTRL, XKB_KEY_N);
  check_key("Control+Shift+n", KB_MOD_CTRL, XKB_KEY_N);
  check_key("Shift+Tab", 0, XKB_KEY_ISO_Left_Tab);
  check_key("ISO_Left_Tab", 0, XKB_KEY_ISO_Left_Tab);
  /* It stays a modifier where it doesn't. */
  check_key("Shift+BackSpace", KB_MOD_SHIFT, XKB_KEY_BackSpace);
  check_key("Shift+1", KB_MOD_SHIFT, XKB_KEY_1);
}

/* evdev keycodes + 8 */
enum {
  KEY_1 = 2 + 8,
  KEY_EQUAL = 13 + 8,
  KEY_BACKSPACE = 14 + 8,
  KEY_TAB = 15 + 8,
  KEY_W = 17 + 8,
  KEY_ENTER = 28 + 8,
  KEY_A = 30 + 8,
  KEY_Q = 16 + 8,
};

enum { NONE = 0, SHIFT = 1, LOCK = 2, CTRL = 4 };

struct kbd {
  struct xkb_context* ctx;
  struct xkb_keymap* keymap;
  struct xkb_state* state;
  struct kb_manager* mgr;
};

static uint32_t mask(struct kbd* k, int mods) {
  uint32_t m = 0;
  if (mods & SHIFT)
    m |= 1u << xkb_keymap_mod_get_index(k->keymap, XKB_MOD_NAME_SHIFT);
  if (mods & CTRL)
    m |= 1u << xkb_keymap_mod_get_index(k->keymap, XKB_MOD_NAME_CTRL);
  return m;
}

/* Resolves key with mods held (LOCK: Caps Lock on) in layout group. */
static struct kb_result press(struct kbd* k, int mods, int group, int key) {
  uint32_t lock =
      mods & LOCK ? 1u << xkb_keymap_mod_get_index(k->keymap, XKB_MOD_NAME_CAPS)
                  : 0;
  xkb_state_update_mask(k->state, mask(k, mods), 0, lock, 0, 0,
                        (xkb_layout_index_t)group);
  return kb_resolve(k->mgr, k->state, (xkb_keycode_t)key);
}

static void check_bind(struct kbd* k, int mods, int group, int key, int bind) {
  struct kb_result r = press(k, mods, group, key);
  if (!r.bound || r.bind != (uint32_t)bind) {
    fprintf(stderr, "  key %d mods %d group %d: expected bind %d, got %s %u\n",
            key, mods, group, bind, r.bound ? "bind" : "text", r.bind);
    test_failures++;
  }
}

static void check_text(struct kbd* k,
                       int mods,
                       int group,
                       int key,
                       const char* text) {
  struct kb_result r = press(k, mods, group, key);
  CHECK(!r.bound);
  CHECK_STR(str_from_cstr(r.text), text);
}

static void test_resolve(void) {
  struct kbd k = {.ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS)};
  struct xkb_rule_names names = {.layout = "us,ru"};
  k.keymap = xkb_keymap_new_from_names(k.ctx, &names, 0);
  CHECK(k.keymap != NULL);
  if (k.keymap == NULL) {
    xkb_context_unref(k.ctx);
    return;
  }
  k.state = xkb_state_new(k.keymap);
  k.mgr = kb_manager_new();

  static const char* const binds[] = {
      "Control+a", "Control+plus",    "Shift+1",   "Shift+Tab",
      "Tab",       "Shift+BackSpace", "Control+w", "Control+A",
  };
  for (size_t i = 0; i < sizeof binds / sizeof binds[0]; i++) {
    struct kb_keypress kp;
    CHECK(kb_parse_keypress(str_from_cstr(binds[i]), &kp));
    CHECK(kb_load_bind(k.mgr, kp, (uint32_t)i));
  }

  check_bind(&k, CTRL, 0, KEY_A, 0);
  check_bind(&k, CTRL | LOCK, 0, KEY_A, 0);       // Caps Lock
  check_bind(&k, CTRL | SHIFT, 0, KEY_EQUAL, 1);  // plus on a US layout
  check_bind(&k, SHIFT, 0, KEY_1, 2);             // exclam isn't bound
  check_bind(&k, SHIFT, 0, KEY_TAB, 3);
  check_bind(&k, NONE, 0, KEY_TAB, 4);
  check_bind(&k, SHIFT, 0, KEY_BACKSPACE, 5);
  check_bind(&k, CTRL, 1, KEY_W, 6);  // Russian layout
  check_bind(&k, CTRL, 0, KEY_W, 6);
  check_bind(&k, CTRL | SHIFT, 0, KEY_A, 7);
  check_bind(&k, CTRL | SHIFT, 1, KEY_A, 7);

  check_text(&k, NONE, 0, KEY_A, "a");
  check_text(&k, SHIFT, 0, KEY_A, "A");
  check_text(&k, LOCK, 0, KEY_A, "A");
  check_text(&k, SHIFT, 0, KEY_EQUAL, "+");
  check_text(&k, NONE, 1, KEY_A, "ф");
  check_text(&k, SHIFT, 1, KEY_A, "Ф");
  check_text(&k, NONE, 0, KEY_ENTER, "");      // unbound, but not text
  check_text(&k, NONE, 0, KEY_BACKSPACE, "");  // likewise
  check_text(&k, CTRL, 0, KEY_Q, "");          // an unbound chord

  kb_manager_destroy(k.mgr);
  xkb_state_unref(k.state);
  xkb_keymap_unref(k.keymap);
  xkb_context_unref(k.ctx);
}

static void test_bad(void) {
  CHECK(!parses(""));
  CHECK(!parses("Control+"));
  CHECK(!parses("Hyper+a"));
  CHECK(!parses("NotAKeysym"));
  CHECK(!parses("+"));
}

int main(void) {
  RUN(test_parse);
  RUN(test_shift);
  RUN(test_resolve);
  RUN(test_bad);
  return TEST_RESULT;
}
