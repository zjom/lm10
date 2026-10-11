#include "kb.h"

#include <xkbcommon/xkbcommon-keysyms.h>

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
  /* Normalized to what the keyboard reports. */
  check_key("Shift+n", KB_MOD_SHIFT, XKB_KEY_N);
  check_key("N", KB_MOD_SHIFT, XKB_KEY_N);
  check_key("Control+N", KB_MOD_CTRL | KB_MOD_SHIFT, XKB_KEY_N);
  check_key("Shift+Tab", KB_MOD_SHIFT, XKB_KEY_ISO_Left_Tab);
  check_key("Shift+BackSpace", KB_MOD_SHIFT, XKB_KEY_BackSpace);
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
  RUN(test_bad);
  return TEST_RESULT;
}
