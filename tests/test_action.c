#include "action.h"
#include "shell.h"
#include "state.h"

#include "test.h"

static void check_quote(const char* s, const char* want) {
  StrBuf out = STRBUF_INIT;
  shell_quote(&out, str_from_cstr(s));
  CHECK_STR(sb_str(&out), want);
  sb_free(&out);
}

static void test_quote(void) {
  check_quote("plain", "plain");
  check_quote("/usr/bin/foo-1.2_x", "/usr/bin/foo-1.2_x");
  check_quote("--opt=a,b", "--opt=a,b");
  check_quote("", "''");
  check_quote("a b", "'a b'");
  check_quote("it's", "'it'\\''s'");
  check_quote("$HOME", "'$HOME'");
  check_quote("~", "'~'");

  /* zsh expands a leading = when the line is eval'd. */
  check_quote("=ls", "'=ls'");
  check_quote("a=b", "a=b");
}

/* Expands tmpl against st, checking the result. */
static void check_expand(struct state* st, const char* tmpl, const char* want) {
  StrBuf out = STRBUF_INIT;
  action_expand(&out, str_from_cstr(tmpl), st);
  CHECK_STR(sb_str(&out), want);
  sb_free(&out);
}

static void push(struct state* st, const char* name, const char* const* argv) {
  StrList words = LIST_INIT;
  for (size_t i = 0; argv[i] != NULL; i++)
    LIST_PUSH(&words, str_from_cstr(argv[i]));
  state_push(st, str_from_cstr(name), &words, STATE_RUN_PRINT);
  LIST_FREE(&words);
}

static void test_expand(void) {
  struct state st;
  state_init(&st);

  /* No selection: everything expands to ''. */
  check_expand(&st, "x {} {n} {0} {1..}", "x '' '' '' ''");

  push(&st, "My Session",
       (const char* const[]){"tmux", "attach", "-t", "a b", NULL});
  state_query_set(&st, STR("my"));
  check_expand(&st, "{}", "tmux attach -t 'a b'");
  check_expand(&st, "{n}", "'My Session'");
  check_expand(&st, "{q}", "my");
  check_expand(&st, "{0}", "tmux");
  check_expand(&st, "{3}", "'a b'");
  check_expand(&st, "{9}", "''");
  check_expand(&st, "{1..2}", "attach -t");
  check_expand(&st, "{2..}", "-t 'a b'");
  check_expand(&st, "{..1}", "tmux attach");
  check_expand(&st, "{2..1}", "''");

  /* Anything else is copied as is. */
  check_expand(&st, "{x} {1.2} { } {", "{x} {1.2} { } {");
  check_expand(&st, "awk '{print $1}' {0}", "awk '{print $1}' tmux");
  state_destroy(&st);
}

int main(void) {
  RUN(test_quote);
  RUN(test_expand);
  return TEST_RESULT;
}
