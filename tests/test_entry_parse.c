#include "entry_parse.h"

#include <string.h>

#include "test.h"

/* Feeds src in chunks of at most chunk bytes, then ends the input. */
static void parse(struct state* st, const char* src, size_t chunk, bool raw) {
  struct entry_parser ep;
  entry_parser_init(&ep, "test", raw, st);
  size_t len = strlen(src);
  for (size_t i = 0; i < len; i += chunk) {
    size_t n = len - i < chunk ? len - i : chunk;
    entry_parser_feed(&ep, str_from_parts(src + i, n));
  }
  entry_parser_finish(&ep);
  entry_parser_destroy(&ep);
}

/* Parses src whole and byte by byte, and checks both give n entries. The
 * whole parse is left in st. */
static void parse_both_as(struct state* st,
                          const char* src,
                          size_t n,
                          bool raw) {
  struct state bytewise;
  state_init(&bytewise);
  parse(&bytewise, src, 1, raw);
  CHECK(bytewise.entries.len == n);
  state_destroy(&bytewise);

  parse(st, src, 4096, raw);
  CHECK(st->entries.len == n);
}

static void parse_both(struct state* st, const char* src, size_t n) {
  parse_both_as(st, src, n, false);
}

static const struct state_entry* entry(const struct state* st, size_t i) {
  return &st->entries.ptr[i];
}

static void test_lines(void) {
  struct state st;
  state_init(&st);
  parse_both(&st,
             "-n Ranger kitty ranger\n"
             "\n"
             "-X foot\r\n"
             "--name='a b' -x -- -dash arg\n"
             "/usr/bin/true",
             4);
  if (st.entries.len == 4) {
    CHECK_STR(entry(&st, 0)->name, "Ranger");
    CHECK(entry(&st, 0)->run == STATE_RUN_PRINT);
    CHECK(entry(&st, 0)->argv.len == 2);

    CHECK_STR(entry(&st, 1)->name, "foot");
    CHECK(entry(&st, 1)->run == STATE_RUN_DETACH);

    CHECK_STR(entry(&st, 2)->name, "a b");
    CHECK(entry(&st, 2)->run == STATE_RUN_EXEC);
    CHECK_STR(entry(&st, 2)->argv.ptr[0], "-dash");

    /* The unterminated last line counts. */
    CHECK_STR(entry(&st, 3)->name, "/usr/bin/true");
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

static void test_nul(void) {
  struct state st;
  state_init(&st);
  struct entry_parser ep;
  entry_parser_init(&ep, "test", false, &st);
  static const char src[] = "rm a\0b\nok\n";
  entry_parser_feed(&ep, str_from_parts(src, sizeof src - 1));
  entry_parser_finish(&ep);
  entry_parser_destroy(&ep);
  CHECK(st.entries.len == 1);
  if (st.entries.len == 1)
    CHECK_STR(entry(&st, 0)->name, "ok");
  state_destroy(&st);
}

static void test_brackets(void) {
  struct state st;
  state_init(&st);
  /* Lines may start with anything, even the [ command or a brace. */
  parse_both(&st, "[ -f /etc/passwd ]\n{ echo; }\n", 2);
  if (st.entries.len == 2)
    CHECK_STR(entry(&st, 0)->argv.ptr[0], "[");
  state_destroy(&st);
}

static void test_raw(void) {
  struct state st;
  state_init(&st);
  /* Lines are kept whole, options and quotes included; only \n ends one. */
  parse_both_as(&st, "-n x 'a  b'\n\nend\r\nlast", 4, true);
  if (st.entries.len == 4) {
    CHECK_STR(entry(&st, 0)->name, "-n x 'a  b'");
    CHECK(entry(&st, 0)->argv.len == 1);
    CHECK_STR(entry(&st, 0)->argv.ptr[0], "-n x 'a  b'");
    CHECK(entry(&st, 0)->run == STATE_RUN_PRINT_LINE);
    CHECK_STR(entry(&st, 1)->name, "");
    CHECK_STR(entry(&st, 2)->argv.ptr[0], "end\r");
    CHECK_STR(entry(&st, 3)->name, "last");
  }
  state_destroy(&st);

  /* A trailing \n doesn't add an empty entry; a NUL byte skips the line. */
  state_init(&st);
  struct entry_parser ep;
  entry_parser_init(&ep, "test", true, &st);
  static const char src[] = "a\0b\nok\n";
  entry_parser_feed(&ep, str_from_parts(src, sizeof src - 1));
  entry_parser_finish(&ep);
  entry_parser_destroy(&ep);
  CHECK(st.entries.len == 1);
  if (st.entries.len == 1)
    CHECK_STR(entry(&st, 0)->name, "ok");
  state_destroy(&st);
}

int main(void) {
  RUN(test_lines);
  RUN(test_bad_lines);
  RUN(test_nul);
  RUN(test_brackets);
  RUN(test_raw);
  return TEST_RESULT;
}
