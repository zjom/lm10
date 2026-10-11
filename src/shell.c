#include "shell.h"

#include <string.h>

bool shell_quote(StrBuf* out, Str s) {
  static const char safe[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
      "0123456789@%+=:,./_-";
  /* zsh expands a word starting with = to a command's path. */
  bool plain = s.len > 0 && s.ptr[0] != '=';
  for (size_t i = 0; plain && i < s.len; i++)
    plain = s.ptr[i] != '\0' && strchr(safe, s.ptr[i]) != NULL;
  if (plain)
    return sb_append(out, s);

  sb_append_char(out, '\'');
  for (size_t i = 0; i < s.len; i++) {
    if (s.ptr[i] == '\'')
      sb_append(out, STR("'\\''"));
    else
      sb_append_char(out, s.ptr[i]);
  }
  return sb_append_char(out, '\'');
}
