#include "entry_parse.h"

#include <string.h>

#include "test.h"

/* Feeds src in chunks of at most chunk bytes, then ends the input. */
static void parse(struct state* st, const char* src, size_t chunk) {
  struct entry_parser ep;
  entry_parser_init(&ep, "test", st);
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

static void test_brackets(void) {
  struct state st;
  state_init(&st);
  /* Lines may start with anything, even the [ command or a brace. */
  parse_both(&st, "[ -f /etc/passwd ]\n{ echo; }\n", 2);
  if (st.entries.len == 2)
    CHECK_STR(entry(&st, 0)->argv.ptr[0], "[");
  state_destroy(&st);
}

int main(void) {
  RUN(test_lines);
  RUN(test_bad_lines);
  RUN(test_brackets);
  return TEST_RESULT;
}
