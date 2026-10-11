#ifndef LM_SHELL_H
#define LM_SHELL_H

#include "ztd_str.h"

#include <stdbool.h>

/* Appends s to out quoted for sh, bash and zsh: as is if it is made only of
 * characters no shell treats specially, single-quoted otherwise. */
void shell_quote(StrBuf* out, Str s);

#endif  // !LM_SHELL_H
