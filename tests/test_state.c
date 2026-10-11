#include "state.h"

#include <string.h>

#include "test.h"

static void push(struct state* st, const char* name) {
  StrList argv = LIST_INIT;
  LIST_PUSH(&argv, STR("cmd"));
  CHECK(state_push(st, str_from_cstr(name), &argv, STR(""), STATE_RUN_PRINT));
  LIST_FREE(&argv);
}

/* The names of the current matches, joined by '|'. */
static void check_matches(const struct state* st, const char* want) {
  StrBuf got = STRBUF_INIT;
  for (size_t i = 0; i < st->matches.len; i++) {
    if (i > 0)
      sb_append_char(&got, '|');
    sb_append(&got, state_match(st, i)->name);
  }
  CHECK_STR(sb_str(&got), want);
  sb_free(&got);
}

static void check_span(const struct state* st,
                       size_t i,
                       size_t want_start,
                       size_t want_len) {
  size_t start = 0, len = 0;
  CHECK(state_match_span(st, i, &start, &len));
  if (start != want_start || len != want_len) {
    fprintf(stderr, "  span of %zu: got %zu+%zu, expected %zu+%zu\n", i, start,
            len, want_start, want_len);
    test_failures++;
  }
}

static void test_unicode_matching(void) {
  struct state st;
  state_init(&st);
  push(&st, "Über");
  push(&st, "ÜBERALL");
  push(&st, "über");
  push(&st, "ascii");
  push(&st, "Straße");

  CHECK(state_query_set(&st, STR("üb")));
  check_matches(&st, "Über|ÜBERALL|über");
  check_span(&st, 0, 0, 3);  // Ü is two bytes

  CHECK(state_query_set(&st, STR("SSE")));
  check_matches(&st, "");  // simple case mapping only: ß isn't ss
  CHECK(state_query_set(&st, STR("STRA")));
  check_matches(&st, "Straße");
  CHECK(state_query_set(&st, STR("ASC")));
  check_matches(&st, "ascii");
  check_span(&st, 0, 0, 3);
  CHECK(state_query_set(&st, STR("ßE")));
  check_span(&st, 0, 4, 3);

  /* An empty query matches everything, with no span. */
  CHECK(state_query_clear(&st));
  CHECK(st.matches.len == 5);
  size_t start, len;
  CHECK(!state_match_span(&st, 0, &start, &len));
  state_destroy(&st);
}

static void test_invalid_utf8(void) {
  struct state st;
  state_init(&st);
  push(&st, "bad\xff name");
  const struct state_entry* e = &st.entries.ptr[0];
  CHECK(e->name.len > 0 && e->name.ptr[e->name.len] == '\0');
  CHECK(strstr(e->name.ptr, "\xff") == NULL);
  CHECK(strstr(e->name.ptr, "\xef\xbf\xbd") != NULL);  // U+FFFD

  CHECK(state_query_insert(&st, STR("a\xfe")));
  CHECK(strstr(sb_cstr(&st.query), "\xfe") == NULL);
  state_destroy(&st);
}

static void test_narrowing(void) {
  struct state st;
  state_init(&st);
  const char* names[] = {"alpha", "alpine", "beta", "Alps", "gamma", "lap"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    push(&st, names[i]);

  /* Typing at the end narrows; it must agree with filtering from scratch. */
  CHECK(state_query_insert(&st, STR("a")));
  check_matches(&st, "alpha|alpine|beta|Alps|gamma|lap");
  CHECK(state_query_insert(&st, STR("l")));
  check_matches(&st, "alpha|alpine|Alps");
  CHECK(state_query_insert(&st, STR("p")));
  check_matches(&st, "alpha|alpine|Alps");
  CHECK(state_query_insert(&st, STR("s")));
  check_matches(&st, "Alps");

  /* Deleting widens again, and inserting mid-query refilters fully. */
  CHECK(state_query_delete(&st, STATE_MOTION_CHAR_LEFT));
  check_matches(&st, "alpha|alpine|Alps");
  state_cursor_move(&st, STATE_MOTION_START);
  CHECK(state_query_insert(&st, STR("l")));  // "lalp"
  check_matches(&st, "");
  CHECK(state_query_delete(&st, STATE_MOTION_CHAR_LEFT));
  state_cursor_move(&st, STATE_MOTION_END);
  CHECK(state_query_set(&st, STR("la")));
  check_matches(&st, "lap");
  state_destroy(&st);
}

static void test_gen(void) {
  struct state st;
  state_init(&st);
  uint64_t g = st.gen;
#define CHANGED(expr)  \
  do {                 \
    expr;              \
    CHECK(st.gen > g); \
    g = st.gen;        \
  } while (0)
  CHANGED(push(&st, "one"));
  CHANGED(push(&st, "two"));
  CHANGED(state_query_insert(&st, STR("o")));
  CHANGED(state_cursor_move(&st, STATE_MOTION_START));
  CHANGED(state_select_next(&st));
  CHANGED(state_select_prev(&st));
  CHANGED(state_set_prompt(&st, STR("> ")));
  CHANGED(state_set_header(&st, STR("a\nb")));
  CHANGED(state_set_border_label(&st, STR(" x ")));
  CHANGED(state_query_set(&st, STR("t")));
  CHANGED(state_clear_entries(&st));
#undef CHANGED
  /* No-ops leave it alone. */
  state_cursor_move(&st, STATE_MOTION_END);
  g = st.gen;
  state_cursor_move(&st, STATE_MOTION_END);
  CHECK(st.gen == g);
  CHECK(state_query_delete(&st, STATE_MOTION_CHAR_RIGHT));
  CHECK(st.gen == g);
  CHECK_STR(sb_str(&st.header), "a\nb");
  state_destroy(&st);
}

int main(void) {
  RUN(test_unicode_matching);
  RUN(test_invalid_utf8);
  RUN(test_narrowing);
  RUN(test_gen);
  return TEST_RESULT;
}
