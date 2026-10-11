#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void die(const char* fmt, ...) {
  va_list ap;
  fputs("lm10: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

void* xmalloc(size_t n) {
  void* p = malloc(n);
  if (p == NULL && n != 0)
    die("out of memory");
  return p;
}

void* xcalloc(size_t n, size_t size) {
  void* p = calloc(n, size);
  if (p == NULL && n != 0 && size != 0)
    die("out of memory");
  return p;
}

void* xrealloc(void* p, size_t n) {
  p = realloc(p, n);
  if (p == NULL && n != 0)
    die("out of memory");
  return p;
}
