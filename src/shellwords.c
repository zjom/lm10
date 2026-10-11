#include "shellwords.h"

static bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

bool shellwords_next(char** p, char* end, Str* word, bool* err) {
  char* r = *p;
  while (r < end && is_space(*r))
    r++;
  if (r == end) {
    *p = r;
    return false;
  }

  char* start = r;
  char* w = r;
  while (r < end && !is_space(*r)) {
    char c = *r++;
    if (c == '\'') {
      while (r < end && *r != '\'')
        *w++ = *r++;
      if (r == end)
        goto bad;
      r++;
    } else if (c == '"') {
      while (r < end && *r != '"') {
        if (*r == '\\' && r + 1 < end && (r[1] == '"' || r[1] == '\\'))
          r++;
        *w++ = *r++;
      }
      if (r == end)
        goto bad;
      r++;
    } else if (c == '\\') {
      if (r == end)
        goto bad;
      *w++ = *r++;
    } else {
      *w++ = c;
    }
  }
  *p = r;
  *word = str_from_parts(start, (size_t)(w - start));
  return true;

bad:
  *err = true;
  *p = end;
  return false;
}
