#include "state.h"

#include <string.h>

#include "test.h"

static void push(struct state* st, const char* name) {
  StrList argv = LIST_INIT;
  LIST_PUSH(&argv, STR("cmd"));
  state_push(st, str_from_cstr(name), &argv, STR(""), STATE_RUN_PRINT);
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

/* The i-th match's highlighted spans, as "start+len" joined by ' '. */
static void check_spans(const struct state* st, size_t i, const char* want) {
  SpanList spans = LIST_INIT;
  state_match_spans(st, i, &spans);
  StrBuf got = STRBUF_INIT;
  ARR_FOREACH(const struct state_span, sp, &spans) {
    sb_appendf(&got, "%s%zu+%zu", got.len > 0 ? " " : "", sp->start, sp->len);
  }
  CHECK_STR(sb_str(&got), want);
  sb_free(&got);
  LIST_FREE(&spans);
}

static void test_unicode_matching(void) {
  struct state st;
  state_init(&st);
  push(&st, "Über");
  push(&st, "ÜBERALL");
  push(&st, "über");
  push(&st, "ascii");
  push(&st, "Straße");

  state_query_set(&st, STR("üb"));
  check_matches(&st, "Über|ÜBERALL|über");
  check_spans(&st, 0, "0+3");  // Ü is two bytes

  state_query_set(&st, STR("SSE"));
  check_matches(&st, "");  // simple case mapping only: ß isn't ss
  state_query_set(&st, STR("STRA"));
  check_matches(&st, "Straße");
  state_query_set(&st, STR("ASC"));
  check_matches(&st, "ascii");
  check_spans(&st, 0, "0+3");
  state_query_set(&st, STR("ßE"));
  check_spans(&st, 0, "4+3");

  /* An empty query matches everything, with no span. */
  state_query_clear(&st);
  CHECK(st.matches.len == 5);
  check_spans(&st, 0, "");
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

  state_query_insert(&st, STR("a\xfe"));
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
  state_query_insert(&st, STR("a"));
  check_matches(&st, "alpha|alpine|beta|Alps|gamma|lap");
  state_query_insert(&st, STR("l"));
  check_matches(&st, "alpha|alpine|Alps");
  state_query_insert(&st, STR("p"));
  check_matches(&st, "alpha|alpine|Alps");
  state_query_insert(&st, STR("s"));
  check_matches(&st, "Alps");

  /* Deleting widens again, and inserting mid-query refilters fully. */
  state_query_delete(&st, STATE_MOTION_CHAR_LEFT);
  check_matches(&st, "alpha|alpine|Alps");
  state_cursor_move(&st, STATE_MOTION_START);
  state_query_insert(&st, STR("l"));  // "lalp"
  check_matches(&st, "");
  state_query_delete(&st, STATE_MOTION_CHAR_LEFT);
  state_cursor_move(&st, STATE_MOTION_END);
  state_query_set(&st, STR("la"));
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
  state_query_delete(&st, STATE_MOTION_CHAR_RIGHT);
  CHECK(st.gen == g);
  CHECK_STR(sb_str(&st.header), "a\nb");
  state_destroy(&st);
}

static void test_fuzzy(void) {
  struct state st;
  state_init(&st);
  const char* names[] = {
      "fabric",           // f-b with a gap
      "foo bar",          // both on word boundaries
      "firefox browser",  // likewise, further apart
      "FooBar",           // camel case
      "xfb",              // consecutive, but not at a boundary
      "nothing",
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    push(&st, names[i]);
  state_set_fuzzy(&st, true);
  check_matches(&st, "fabric|foo bar|firefox browser|FooBar|xfb|nothing");

  state_query_set(&st, STR("fb"));
  /* Word starts beat gaps, which beat matching mid-word; ties keep entry
   * order. firefox browser keeps its leading f rather than the shorter
   * "fox b". */
  check_matches(&st, "foo bar|FooBar|firefox browser|fabric|xfb");
  check_spans(&st, 0, "0+1 4+1");
  check_spans(&st, 2, "0+1 8+1");
  check_spans(&st, 4, "1+2");  // one run: fb in xfb
  for (size_t i = 1; i < st.matches.len; i++)
    CHECK(st.matches.ptr[i - 1].score >= st.matches.ptr[i].score);

  /* Narrowing keeps the ranking right. */
  state_query_insert(&st, STR("r"));
  check_matches(&st, "firefox browser|fabric|foo bar|FooBar");

  /* Entries arriving later go in by score once sorted; the selection stays
   * put. */
  state_select_next(&st);
  push(&st, "f b r");
  push(&st, "zzfzbzr");
  CHECK(st.selected == 1);
  state_sort(&st);
  check_matches(&st, "f b r|firefox browser|fabric|foo bar|FooBar|zzfzbzr");

  /* Back to substrings, in entry order. */
  state_set_fuzzy(&st, false);
  check_matches(&st, "");
  state_query_set(&st, STR("b"));
  check_matches(&st, "fabric|foo bar|firefox browser|FooBar|xfb|f b r|zzfzbzr");
  state_destroy(&st);
}

int main(void) {
  RUN(test_unicode_matching);
  RUN(test_invalid_utf8);
  RUN(test_narrowing);
  RUN(test_gen);
  RUN(test_fuzzy);
  return TEST_RESULT;
}
