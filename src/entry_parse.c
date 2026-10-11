#include "entry_parse.h"

#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "log.h"
#include "shellwords.h"

static bool is_blank(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f' ||
         c == '\n';
}

/* Parses one line (mutated in place) and pushes the entry it describes.
 * Returns true if an entry was pushed. */
static bool parse_line(struct entry_parser* ep, char* line, size_t len) {
  char* p = line;
  char* end = line + len;
  Str name = {0}, icon = {0};
  enum state_run run = STATE_RUN_PRINT;
  bool err = false;
  Str word;

  LIST_CLEAR(&ep->words);

  /* options, up to the first word that isn't one (or "--") */
  while (shellwords_next(&p, end, &word, &err)) {
    if (str_eq(word, STR("--"))) {
      if (!shellwords_next(&p, end, &word, &err))
        break;
      LIST_PUSH(&ep->words, word);
      break;
    }
    if (word.len < 2 || word.ptr[0] != '-') {
      LIST_PUSH(&ep->words, word);
      break;
    }

    if (str_eq(word, STR("-x")) || str_eq(word, STR("--exec"))) {
      run = STATE_RUN_EXEC;
      continue;
    }
    if (str_eq(word, STR("-X")) || str_eq(word, STR("--detach"))) {
      run = STATE_RUN_DETACH;
      continue;
    }

    Str* dst;
    Str val;
    bool has_val = false;
    if (str_starts_with(word, STR("--"))) {
      Str key;
      if (str_cut(word, STR("="), &key, &val)) {
        word = key;
        has_val = true;
      }
    }

    if (str_eq(word, STR("--exec")) || str_eq(word, STR("--detach"))) {
      LOG_WARN("%s: option \"%.*s\" takes no value, skipping line", ep->name,
               (int)word.len, word.ptr);
      return false;
    } else if (str_eq(word, STR("-n")) || str_eq(word, STR("--name"))) {
      dst = &name;
    } else if (str_eq(word, STR("-i")) || str_eq(word, STR("--icon"))) {
      dst = &icon;
    } else {
      LOG_WARN("%s: unknown option \"%.*s\", skipping line", ep->name,
               (int)word.len, word.ptr);
      return false;
    }

    if (!has_val && !shellwords_next(&p, end, &val, &err)) {
      if (!err) {
        LOG_WARN("%s: option \"%.*s\" needs a value, skipping line", ep->name,
                 (int)word.len, word.ptr);
        return false;
      }
      break;
    }
    *dst = val;
  }

  /* the rest is the command and its arguments */
  while (!err && ep->words.len > 0 && shellwords_next(&p, end, &word, &err))
    LIST_PUSH(&ep->words, word);

  if (err) {
    LOG_WARN("%s: unterminated quote or escape, skipping line", ep->name);
    return false;
  }
  if (ep->words.len == 0) {
    if (str_trim(str_from_parts(line, len)).len != 0)
      LOG_WARN("%s: line has no command, skipping", ep->name);
    return false;
  }

  state_push(ep->state, name, &ep->words, icon, run);
  return true;
}

/* Parses every complete line in the buffer, plus the trailing partial one if
 * at_eof. Returns true if any entry was pushed. */
static bool drain_lines(struct entry_parser* ep, bool at_eof) {
  char* buf = ep->buf.ptr;
  size_t len = ep->buf.len;
  size_t pos = 0;
  bool pushed = false;

  while (pos < len) {
    char* nl = memchr(buf + pos, '\n', len - pos);
    if (nl == NULL) {
      if (!at_eof)
        break;
      nl = buf + len;
    }
    pushed |= parse_line(ep, buf + pos, (size_t)(nl - (buf + pos)));
    pos = (size_t)(nl - buf) + (nl < buf + len);
  }

  if (pos > 0) {
    memmove(buf, buf + pos, len - pos);
    sb_truncate(&ep->buf, len - pos);
  }
  return pushed;
}

/* ---- JSON ---- */

/* json.h has no message table of its own. */
static const char* json_strerror(size_t err) {
  switch (err) {
    case json_parse_error_expected_comma_or_closing_bracket:
      return "expected ',' or a closing bracket";
    case json_parse_error_expected_colon:
      return "expected ':'";
    case json_parse_error_expected_opening_quote:
      return "expected '\"'";
    case json_parse_error_invalid_string_escape_sequence:
      return "invalid escape sequence";
    case json_parse_error_invalid_number_format:
      return "invalid number";
    case json_parse_error_invalid_value:
      return "invalid value";
    case json_parse_error_premature_end_of_buffer:
      return "unexpected end of value";
    case json_parse_error_invalid_string:
      return "invalid string";
    case json_parse_error_allocator_failed:
      return "allocation failed";
    case json_parse_error_unexpected_trailing_characters:
      return "unexpected trailing characters";
    case json_parse_error_recursion:
      return "nested too deeply";
    case json_parse_error_unterminated_comment:
      return "unterminated comment";
    default:
      return "parse error";
  }
}

/* The flags every value is parsed with: JSONC, plus the line numbers the
 * warnings point at. */
#define JSON_FLAGS                           \
  (json_parse_flags_allow_trailing_comma |   \
   json_parse_flags_allow_c_style_comments | \
   json_parse_flags_allow_location_information)

/* The input line v starts on, given the line its parsed text started on. */
static size_t json_line(const struct json_value_s* v, size_t base) {
  return base + ((const struct json_value_ex_s*)v)->line_no - 1;
}

/* Sets *out to v if it is a string without NUL bytes, which couldn't be
 * passed on in argv. */
static bool json_str(struct json_value_s* v, Str* out) {
  struct json_string_s* js = json_value_as_string(v);
  if (js == NULL || memchr(js->string, '\0', js->string_size) != NULL)
    return false;
  *out = str_from_parts(js->string, js->string_size);
  return true;
}

/* Fills ep->words from a "command" value: an array of strings taken as is,
 * or a string split like a line. Returns false (having warned) if it is
 * neither. */
static bool json_command(struct entry_parser* ep,
                         struct json_value_s* v,
                         size_t line) {
  LIST_CLEAR(&ep->words);
  Str word;

  struct json_array_s* arr = json_value_as_array(v);
  if (arr != NULL) {
    for (struct json_array_element_s* el = arr->start; el; el = el->next) {
      if (!json_str(el->value, &word)) {
        LOG_WARN(
            "%s:%zu: \"command\" elements must be strings, skipping "
            "entry",
            ep->name, json_line(el->value, line));
        return false;
      }
      LIST_PUSH(&ep->words, word);
    }
    return true;
  }

  Str cmd;
  if (!json_str(v, &cmd)) {
    LOG_WARN(
        "%s:%zu: \"command\" must be a string or an array of "
        "strings, skipping entry",
        ep->name, json_line(v, line));
    return false;
  }
  /* The string lives in json.h's allocation, which is ours to mutate. */
  char* p = (char*)cmd.ptr;
  char* end = p + cmd.len;
  bool err = false;
  while (shellwords_next(&p, end, &word, &err))
    LIST_PUSH(&ep->words, word);
  if (err) {
    LOG_WARN(
        "%s:%zu: unterminated quote or escape in \"command\", "
        "skipping entry",
        ep->name, json_line(v, line));
    return false;
  }
  return true;
}

/* Pushes the entry an object describes. Returns true if one was pushed. */
static bool json_entry(struct entry_parser* ep,
                       struct json_value_s* v,
                       size_t line) {
  struct json_object_s* obj = json_value_as_object(v);
  if (obj == NULL) {
    LOG_WARN("%s:%zu: expected an entry object, skipping", ep->name,
             json_line(v, line));
    return false;
  }

  Str name = {0}, icon = {0};
  enum state_run run = STATE_RUN_PRINT;
  LIST_CLEAR(&ep->words);

  for (struct json_object_element_s* el = obj->start; el; el = el->next) {
    Str key = str_from_parts(el->name->string, el->name->string_size);
    struct json_value_s* val = el->value;
    size_t at = json_line(val, line);
    Str s;

    if (str_eq(key, STR("command"))) {
      if (!json_command(ep, val, line))
        return false;
    } else if (str_eq(key, STR("name"))) {
      if (!json_str(val, &name))
        goto not_string;
    } else if (str_eq(key, STR("icon"))) {
      if (json_value_is_null(val))
        icon = STR("");
      else if (!json_str(val, &icon))
        goto not_string;
    } else if (str_eq(key, STR("run"))) {
      bool ok = json_str(val, &s);
      if (ok && str_eq(s, STR("print"))) {
        run = STATE_RUN_PRINT;
      } else if (ok && str_eq(s, STR("exec"))) {
        run = STATE_RUN_EXEC;
      } else if (ok && str_eq(s, STR("detach"))) {
        run = STATE_RUN_DETACH;
      } else {
        LOG_WARN(
            "%s:%zu: \"run\" must be \"print\", \"exec\" or "
            "\"detach\", skipping entry",
            ep->name, at);
        return false;
      }
    } else {
      LOG_WARN("%s:%zu: unknown key \"" STR_FMT "\", ignoring", ep->name, at,
               STR_ARG(key));
    }
    continue;

  not_string:
    LOG_WARN("%s:%zu: \"" STR_FMT "\" must be a string, skipping entry",
             ep->name, at, STR_ARG(key));
    return false;
  }

  if (ep->words.len == 0) {
    LOG_WARN("%s:%zu: entry has no command, skipping", ep->name,
             json_line(v, line));
    return false;
  }
  state_push(ep->state, name, &ep->words, icon, run);
  return true;
}

/* Parses one complete value, starting on the given input line, and pushes
 * the entries in it: an entry object or an array of them. Returns true if
 * any entry was pushed. */
static bool json_value(struct entry_parser* ep,
                       const char* src,
                       size_t len,
                       size_t line) {
  struct json_parse_result_s res;
  struct json_value_s* root =
      json_parse_ex(src, len, JSON_FLAGS, NULL, NULL, &res);
  if (root == NULL) {
    LOG_WARN("%s:%zu: %s, skipping", ep->name, line + res.error_line_no - 1,
             json_strerror(res.error));
    return false;
  }

  bool pushed = false;
  struct json_array_s* arr = json_value_as_array(root);
  if (arr != NULL) {
    for (struct json_array_element_s* el = arr->start; el; el = el->next)
      pushed |= json_entry(ep, el->value, line);
  } else {
    pushed = json_entry(ep, root, line);
  }
  free(root);
  return pushed;
}

/* Warns once per run of bytes that can't start a value. */
static void json_stray(struct entry_parser* ep) {
  if (!ep->json.stray)
    LOG_WARN("%s:%zu: expected an entry object, skipping", ep->name,
             ep->json.line);
  ep->json.stray = true;
}

/* Finds where each top-level value ends, as well as each element of a
 * top-level array, so they can be parsed (and fail) one at a time, and
 * parses them. Returns true if any entry was pushed. */
static bool drain_json(struct entry_parser* ep, bool at_eof) {
  struct entry_json* s = &ep->json;
  char* buf = ep->buf.ptr;
  size_t len = ep->buf.len;
  bool pushed = false;

  for (; s->pos < len; s->pos++) {
    char c = buf[s->pos];
    if (c == '\n')
      s->line++;

    if (s->comment == ENTRY_JSON_LINE_COMMENT) {
      if (c == '\n')
        s->comment = ENTRY_JSON_NO_COMMENT;
      continue;
    }
    if (s->comment == ENTRY_JSON_BLOCK_COMMENT) {
      if (c == '*' && s->pos + 1 == len && !at_eof)
        break;  // the '/' may be in the next read
      if (c == '*' && s->pos + 1 < len && buf[s->pos + 1] == '/') {
        s->comment = ENTRY_JSON_NO_COMMENT;
        s->pos++;
      }
      continue;
    }
    if (s->in_str) {
      if (s->esc)
        s->esc = false;
      else if (c == '\\')
        s->esc = true;
      else if (c == '"')
        s->in_str = false;
      continue;
    }

    switch (c) {
      case ' ':
      case '\t':
      case '\r':
      case '\n':
        break;
      case '/':
        if (s->pos + 1 == len && !at_eof)
          goto out;  // the next byte may be in the next read
        if (s->pos + 1 < len && buf[s->pos + 1] == '/') {
          s->comment = ENTRY_JSON_LINE_COMMENT;
          s->pos++;
        } else if (s->pos + 1 < len && buf[s->pos + 1] == '*') {
          s->comment = ENTRY_JSON_BLOCK_COMMENT;
          s->pos++;
        } else if (s->depth == 0) {
          json_stray(ep);
        }
        break;
      case '"':
        s->in_str = true;
        if (s->depth == 0)
          json_stray(ep);
        break;
      case '[':
      case '{':
        if (s->depth == 0) {
          if (c == '[' && !s->in_array) {
            s->in_array = true;
            break;
          }
          s->start = s->pos;
          s->start_line = s->line;
          s->stray = false;
        }
        s->depth++;
        break;
      case ']':
      case '}':
        if (s->depth > 0) {
          if (--s->depth == 0)
            pushed |= json_value(ep, buf + s->start, s->pos + 1 - s->start,
                                 s->start_line);
        } else if (c == ']' && s->in_array) {
          s->in_array = false;
        } else {
          json_stray(ep);
        }
        break;
      case ',':
        if (s->depth == 0 && !s->in_array)
          json_stray(ep);
        break;
      default:
        if (s->depth == 0)
          json_stray(ep);
        break;
    }
  }
out:

  if (at_eof && (s->depth > 0 || s->in_array))
    LOG_WARN("%s:%zu: unexpected end of input, skipping", ep->name,
             s->depth > 0 ? s->start_line : s->line);

  /* Drop what has been consumed, keeping a value still being read. */
  size_t keep = s->depth > 0 ? s->start : s->pos;
  if (keep > 0) {
    memmove(buf, buf + keep, len - keep);
    sb_truncate(&ep->buf, len - keep);
    s->pos -= keep;
    s->start = 0;  // the value being read, if any, now starts the buffer
  }
  return pushed;
}

/* Whether JSON starts at buf[i]: an object, an array or a comment. With
 * array_item, what follows a '[' is checked instead: an entry object, an
 * array of them, the closing ']' or a comment. Returns ENTRY_FORMAT_UNKNOWN
 * if it takes more input to tell. */
static enum entry_format json_at(const char* buf,
                                 size_t len,
                                 size_t i,
                                 bool at_eof,
                                 bool array_item) {
  while (i < len && is_blank(buf[i]))
    i++;
  if (i == len)
    return at_eof ? ENTRY_FORMAT_LINES : ENTRY_FORMAT_UNKNOWN;
  if (buf[i] == '{' || (array_item && buf[i] == ']'))
    return ENTRY_FORMAT_JSON;
  if (buf[i] == '[') {
    /* A line list can start with the [ command, as in "[ -f x ] && ...". */
    return array_item ? ENTRY_FORMAT_JSON
                      : json_at(buf, len, i + 1, at_eof, true);
  }
  if (buf[i] == '/') {
    if (i + 1 == len)
      return at_eof ? ENTRY_FORMAT_LINES : ENTRY_FORMAT_UNKNOWN;
    if (buf[i + 1] == '/' || buf[i + 1] == '*')
      return ENTRY_FORMAT_JSON;
  }
  return ENTRY_FORMAT_LINES;
}

/* Picks the format from how the input starts: JSON if it opens an object,
 * an array or a comment, lines otherwise. */
static enum entry_format detect_format(const struct entry_parser* ep,
                                       bool at_eof) {
  return json_at(ep->buf.ptr, ep->buf.len, 0, at_eof, false);
}

/* Parses whatever complete entries the buffer holds, and the rest if
 * at_eof. Returns true if any entry was pushed. */
static bool drain(struct entry_parser* ep, bool at_eof) {
  if (ep->format == ENTRY_FORMAT_UNKNOWN)
    ep->format = detect_format(ep, at_eof);
  switch (ep->format) {
    case ENTRY_FORMAT_LINES:
      return drain_lines(ep, at_eof);
    case ENTRY_FORMAT_JSON:
      return drain_json(ep, at_eof);
    default:
      return false;
  }
}

void entry_parser_init(struct entry_parser* ep,
                       const char* name,
                       struct state* state) {
  *ep = (struct entry_parser){
      .name = name,
      .state = state,
      .buf = STRBUF_INIT,
      .words = LIST_INIT,
      .json = {.line = 1},
  };
}

void entry_parser_destroy(struct entry_parser* ep) {
  sb_free(&ep->buf);
  LIST_FREE(&ep->words);
}

bool entry_parser_feed(struct entry_parser* ep, Str data) {
  sb_append(&ep->buf, data);
  return drain(ep, false);
}

bool entry_parser_finish(struct entry_parser* ep) {
  return drain(ep, true);
}
