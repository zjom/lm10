#include "action.h"

#include "shell.h"

/* Parses a decimal index; an empty s leaves *out alone. */
static bool parse_index(Str s, size_t* out) {
  uint64_t n;
  if (s.len == 0)
    return true;
  for (size_t i = 0; i < s.len; i++) {
    if (s.ptr[i] < '0' || s.ptr[i] > '9')
      return false;
  }
  if (!str_to_u64(s, 10, &n))
    return false;
  *out = (size_t)n;
  return true;
}

/* Appends the expansion of the placeholder {name}. Returns false if name
 * isn't one. */
static bool expand_one(StrBuf* out, Str name, struct state* state) {
  const struct state_entry* e = state_get_selection(state);

  if (str_eq(name, STR("q"))) {
    shell_quote(out, sb_str(&state->query));
    return true;
  }
  if (str_eq(name, STR("n"))) {
    shell_quote(out, e != NULL ? e->name : STR(""));
    return true;
  }

  /* {}, {N} or {N..M}: argv words from..to */
  size_t from = 0, to = (size_t)-1;
  Str lo, hi;
  if (name.len == 0) {
    /* every word */
  } else if (str_cut(name, STR(".."), &lo, &hi)) {
    if (!parse_index(lo, &from) || !parse_index(hi, &to))
      return false;
  } else {
    if (!parse_index(name, &from))
      return false;
    to = from;
  }

  size_t n = e != NULL ? e->argv.len : 0;
  if (to >= n)
    to = n == 0 ? 0 : n - 1;
  if (n == 0 || from > to) {
    shell_quote(out, STR(""));
    return true;
  }
  for (size_t i = from; i <= to; i++) {
    if (i > from)
      sb_append_char(out, ' ');
    shell_quote(out, e->argv.ptr[i]);
  }
  return true;
}

bool action_expand(StrBuf* out, Str tmpl, struct state* state) {
  sb_clear(out);
  size_t i = 0;
  while (i < tmpl.len) {
    size_t open = str_find_char(str_slice(tmpl, i, tmpl.len), '{');
    if (open == STR_NPOS) {
      sb_append(out, str_slice(tmpl, i, tmpl.len));
      break;
    }
    open += i;
    sb_append(out, str_slice(tmpl, i, open));

    Str rest = str_slice(tmpl, open + 1, tmpl.len);
    size_t close = str_find_char(rest, '}');
    if (close == STR_NPOS ||
        !expand_one(out, str_slice(rest, 0, close), state)) {
      sb_append_char(out, '{');
      i = open + 1;
      continue;
    }
    i = open + 1 + close + 1;
  }
  return !out->failed;
}
