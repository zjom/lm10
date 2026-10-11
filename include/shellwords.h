#ifndef LM_SHELLWORDS_H
#define LM_SHELLWORDS_H

#include "ztd_str.h"

#include <stdbool.h>

/* Pulls the next shell-style word out of [*p, end), unquoting it in place:
 * '...' is literal, "..." honours \" and \\, and a bare backslash escapes
 * the next character. Words are separated by spaces, tabs, \r, \v and \f.
 * Returns false at end of input; sets *err on an unterminated quote or a
 * trailing backslash. */
bool shellwords_next(char** p, char* end, Str* word, bool* err);

#endif  // !LM_SHELLWORDS_H
