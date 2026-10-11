#include "entry_parse.h"

#include <string.h>

#include "test.h"

/* Feeds src in chunks of at most chunk bytes, then ends the input. */
static void parse(struct state* st, const char* src, size_t chunk) {
  struct entry_parser ep;
  entry_parser_init(&ep, "test", st);
  size_t len = strlen(src);
  bool pushed = false;
  for (size_t i = 0; i < len; i += chunk) {
    size_t n = len - i < chunk ? len - i : chunk;
    CHECK(entry_parser_feed(&ep, str_from_parts(src + i, n), &pushed));
  }
  entry_parser_finish(&ep);
  entry_parser_destroy(&ep);
}

/* Parses src whole and byte by byte, and checks both give n entries. The
 * whole parse is left in st. */
static void parse_both(struct state* st, const char* src, size_t n) {
  struct state bytewise;
  state_init(&bytewise);
  parse(&bytewise, src, 1);
  CHECK(bytewise.entries.len == n);
  state_destroy(&bytewise);

  parse(st, src, 4096);
  CHECK(st->entries.len == n);
}

static const struct state_entry* entry(const struct state* st, size_t i) {
  return &st->entries.ptr[i];
}

static void test_lines(void) {
  struct state st;
  state_init(&st);
  parse_both(&st,
             "-n Ranger -i ranger.svg kitty ranger\n"
             "\n"
             "-X foot\r\n"
             "--name='a b' --icon= -x -- -dash arg\n"
             "/usr/bin/true",
             4);
  if (st.entries.len == 4) {
    CHECK_STR(entry(&st, 0)->name, "Ranger");
    CHECK_STR(entry(&st, 0)->icon, "ranger.svg");
    CHECK(!entry(&st, 0)->icon_guessed);
    CHECK(entry(&st, 0)->run == STATE_RUN_PRINT);
    CHECK(entry(&st, 0)->argv.len == 2);

    CHECK_STR(entry(&st, 1)->name, "foot");
    CHECK_STR(entry(&st, 1)->icon, "foot");
    CHECK(entry(&st, 1)->icon_guessed);
    CHECK(entry(&st, 1)->run == STATE_RUN_DETACH);

    CHECK_STR(entry(&st, 2)->name, "a b");
    CHECK_STR(entry(&st, 2)->icon, "");
    CHECK(entry(&st, 2)->run == STATE_RUN_EXEC);
    CHECK_STR(entry(&st, 2)->argv.ptr[0], "-dash");

    /* The unterminated last line counts, and the icon is the basename. */
    CHECK_STR(entry(&st, 3)->icon, "true");
  }
  state_destroy(&st);
}

static void test_bad_lines(void) {
  struct state st;
  state_init(&st);
  parse_both(&st,
             "-q cmd\n"          // unknown option
             "-n\n"              // option without a value
             "--exec=yes cmd\n"  // flag with a value
             "'open\n"           // unterminated quote
             "-n only-a-name\n"  // no command
             "ok\n",
             1);
  if (st.entries.len == 1)
    CHECK_STR(entry(&st, 0)->name, "ok");
  state_destroy(&st);
}

static void test_json(void) {
  struct state st;
  state_init(&st);
  parse_both(
      &st,
      "// leading comment\n"
      "[\n"
      "  {\"name\": \"Lock\", \"icon\": \"lock\", \"run\": \"detach\",\n"
      "   \"command\": \"loginctl lock-session\"},\n"
      "  /* a [block] comment with \"quotes\" */\n"
      "  {\"command\": [\"sh\", \"-c\", \"echo '}'\"], \"icon\": null},\n"
      "]\n"
      "{\"command\": \"x\", \"run\": \"exec\",}\n",
      3);
  if (st.entries.len == 3) {
    CHECK_STR(entry(&st, 0)->name, "Lock");
    CHECK(entry(&st, 0)->argv.len == 2);
    CHECK(entry(&st, 0)->run == STATE_RUN_DETACH);

    CHECK_STR(entry(&st, 1)->name, "sh -c echo '}'");
    CHECK(entry(&st, 1)->argv.len == 3);
    CHECK_STR(entry(&st, 1)->icon, "");
    CHECK(!entry(&st, 1)->icon_guessed);

    CHECK(entry(&st, 2)->run == STATE_RUN_EXEC);
  }
  state_destroy(&st);
}

static void test_bad_json(void) {
  struct state st;
  state_init(&st);
  /* Each bad element is skipped on its own. */
  parse_both(&st,
             "[{\"command\": 1}, {\"name\": \"no command\"},\n"
             " {\"command\": \"x\", \"run\": \"later\"}, 42, \"str\",\n"
             " {\"command\": \"ok\", \"unknown\": true},\n"
             " {\"command\": \"a\" \"b\"}]\n"
             "{\"command\": \"unterminated\"",
             1);
  if (st.entries.len == 1)
    CHECK_STR(entry(&st, 0)->name, "ok");
  state_destroy(&st);
}

static void test_format_detection(void) {
  struct state st;
  state_init(&st);
  /* Blank lines before the first value don't decide the format. */
  parse_both(&st, "\n\n  {\"command\": \"x\"}", 1);
  state_destroy(&st);

  state_init(&st);
  parse_both(&st, "/usr/bin/foo\n", 1);
  state_destroy(&st);

  /* A line list may start with the [ (test) command... */
  state_init(&st);
  parse_both(&st, "[ -f /etc/passwd ] && echo yes\n", 1);
  state_destroy(&st);

  /* ...while arrays of entries, or empty ones, are JSON. */
  state_init(&st);
  parse_both(&st, "[\n  // entries\n  {\"command\": \"x\"}]", 1);
  state_destroy(&st);
  state_init(&st);
  parse_both(&st, "[[{\"command\": \"x\"}], []]", 1);
  state_destroy(&st);
  state_init(&st);
  parse_both(&st, "[]\n{\"command\": \"x\"}", 1);
  state_destroy(&st);
}

int main(void) {
  RUN(test_lines);
  RUN(test_bad_lines);
  RUN(test_json);
  RUN(test_bad_json);
  RUN(test_format_detection);
  return TEST_RESULT;
}
