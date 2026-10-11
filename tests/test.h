#ifndef LM_TEST_H
#define LM_TEST_H

/* A minimal test harness: each test file's main runs its tests with RUN and
 * returns TEST_RESULT. CHECK failures are counted and reported, but don't
 * stop the test. */

#include "ztd_str.h"

#include <stdio.h>

static int test_failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      test_failures++;                                                         \
    }                                                                          \
  } while (0)

/* Checks that a Str equals a C string. */
#define CHECK_STR(s, cstr)                                             \
  do {                                                                 \
    Str s_ = (s);                                                      \
    const char* c_ = (cstr);                                           \
    if (!str_eq(s_, str_from_cstr(c_))) {                              \
      fprintf(stderr, "%s:%d: got \"" STR_FMT "\", expected \"%s\"\n", \
              __FILE__, __LINE__, STR_ARG(s_), c_);                    \
      test_failures++;                                                 \
    }                                                                  \
  } while (0)

/* A known bug: cond is what should hold once it's fixed. Passes while cond
 * is false and fails once it holds, so the fix has to turn this into a
 * CHECK. */
#define XFAIL(cond)                                                     \
  do {                                                                  \
    if (cond) {                                                         \
      fprintf(stderr, "%s:%d: XFAIL(%s) now passes; make it a CHECK\n", \
              __FILE__, __LINE__, #cond);                               \
      test_failures++;                                                  \
    }                                                                   \
  } while (0)

#define RUN(fn)                      \
  do {                               \
    fprintf(stderr, "-- %s\n", #fn); \
    fn();                            \
  } while (0)

#define TEST_RESULT (test_failures == 0 ? 0 : 1)

#endif  // !LM_TEST_H
