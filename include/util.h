#ifndef LM_UTIL_H
#define LM_UTIL_H

#include <stddef.h>

/* Prints "lm10: " and the message to stderr, then exits with status 1. */
_Noreturn void die(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* malloc, calloc and realloc that die instead of returning NULL. */
void* xmalloc(size_t n);
void* xcalloc(size_t n, size_t size);
void* xrealloc(void* p, size_t n);

#endif  // !LM_UTIL_H
