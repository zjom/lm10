#include "shellwords.h"

#include <string.h>

#include "test.h"

#define MAX_WORDS 8

/* Splits src into words; returns how many, or -1 if it is malformed. */
static int split(const char* src, char* buf, Str words[MAX_WORDS]) {
  size_t len = strlen(src);
  memcpy(buf, src, len);
  char* p = buf;
  bool err = false;
  int n = 0;
  Str w;
  while (n < MAX_WORDS && shellwords_next(&p, buf + len, &w, &err))
    words[n++] = w;
  return err ? -1 : n;
}

static void test_plain(void) {
  char buf[64];
  Str w[MAX_WORDS];
  CHECK(split("  foo bar\tbaz  ", buf, w) == 3);
  CHECK_STR(w[0], "foo");
  CHECK_STR(w[1], "bar");
  CHECK_STR(w[2], "baz");
  CHECK(split("", buf, w) == 0);
  CHECK(split(" \t\r ", buf, w) == 0);
}

static void test_quotes(void) {
  char buf[64];
  Str w[MAX_WORDS];
  CHECK(split("'a b' \"c d\" e\\ f", buf, w) == 3);
  CHECK_STR(w[0], "a b");
  CHECK_STR(w[1], "c d");
  CHECK_STR(w[2], "e f");

  /* Adjacent parts join into one word. */
  CHECK(split("a'b'\"c\"\\d", buf, w) == 1);
  CHECK_STR(w[0], "abcd");

  /* Single quotes are literal; double quotes only unescape \" and \\. */
  CHECK(split("'\\n' \"\\\" \\\\ \\n\"", buf, w) == 2);
  CHECK_STR(w[0], "\\n");
  CHECK_STR(w[1], "\" \\ \\n");

  CHECK(split("'' \"\"", buf, w) == 2);
  CHECK_STR(w[0], "");
  CHECK_STR(w[1], "");
}

static void test_errors(void) {
  char buf[64];
  Str w[MAX_WORDS];
  CHECK(split("ok 'open", buf, w) == -1);
  CHECK(split("ok \"open", buf, w) == -1);
  CHECK(split("trailing\\", buf, w) == -1);
}

int main(void) {
  RUN(test_plain);
  RUN(test_quotes);
  RUN(test_errors);
  return TEST_RESULT;
}
