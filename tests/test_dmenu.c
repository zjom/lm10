#include "dmenu.h"

#include <stdlib.h>

#include "test.h"

/* Translates the words in in (NULL-terminated) and checks they become the
 * words in want. */
static void check(const char* const* in, const char* const* want) {
  int argc = 0;
  while (in[argc] != NULL)
    argc++;
  int n;
  char** out = dmenu_translate(argc, (char**)in, &n);
  int i = 0;
  for (; want[i] != NULL && i < n; i++)
    CHECK_STR(str_from_cstr(out[i]), want[i]);
  CHECK(want[i] == NULL && i == n && out[n] == NULL);
  free(out);
}

static void test_translate(void) {
  check((const char*[]){"dmenu", "-i", "-p", "run:", "-l", "10", "-b", NULL},
        (const char*[]){"dmenu", "--prompt", "run:", "--lines", "10",
                        "--position", "bottom", NULL});
  check((const char*[]){"lm10", "--dmenu", "-nb", "#000000", "-sf", "#ffffff",
                        "-fn", "Mono 10", "-m", "1", NULL},
        (const char*[]){"lm10", "--dmenu", "--color", "background=#000000",
                        "--color", "selection-text=#ffffff", "--font",
                        "Mono 10", "--output", "1", NULL});
  /* lm10's flags pass through; -w and its value are dropped. */
  check((const char*[]){"dmenu", "--width", "40%", "-w", "123", "-f", "-v",
                        NULL},
        (const char*[]){"dmenu", "--width", "40%", "--version", NULL});
  /* A missing value is left for getopt to report. */
  check((const char*[]){"dmenu", "-p", NULL},
        (const char*[]){"dmenu", "--prompt", NULL});
  check((const char*[]){"dmenu", "-nb", NULL},
        (const char*[]){"dmenu", "--color", NULL});
}

static void test_wanted(void) {
  CHECK(dmenu_wanted(1, (char*[]){"dmenu", NULL}));
  CHECK(dmenu_wanted(1, (char*[]){"/usr/bin/dmenu", NULL}));
  CHECK(dmenu_wanted(2, (char*[]){"lm10", "--dmenu", NULL}));
  CHECK(!dmenu_wanted(2, (char*[]){"lm10", "-v", NULL}));
  CHECK(!dmenu_wanted(1, (char*[]){"dmenu_run", NULL}));
}

int main(void) {
  RUN(test_translate);
  RUN(test_wanted);
  return TEST_RESULT;
}
