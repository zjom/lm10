#include "entry_parse.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "shellwords.h"

/* Parses one line (mutated in place) and pushes the entry it describes.
 * Returns true if an entry was pushed. */
static bool parse_line(struct entry_parser* ep, char* line, size_t len) {
  char* p = line;
  char* end = line + len;
  Str name = {0};
  enum state_run run = STATE_RUN_PRINT;
  bool err = false;
  Str word;

  /* It would cut an argv word short, or the sh -c a placeholder ends up
   * in. */
  if (memchr(line, '\0', len) != NULL) {
    LOG_WARN("%s: NUL byte in line, skipping", ep->name);
    return false;
  }

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
    }
    if (!str_eq(word, STR("-n")) && !str_eq(word, STR("--name"))) {
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
    name = val;
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

  state_push(ep->state, name, &ep->words, run);
  return true;
}

/* Parses every complete line in the buffer, plus the trailing partial one if
 * at_eof. Returns true if any entry was pushed. */
static bool drain_lines(struct entry_parser* ep, bool at_eof) {
  char* buf = ep->buf.ptr;
  size_t len = ep->buf.len;
  size_t pos = 0;
  size_t from = ep->scanned;  // a long line is searched for '\n' only once
  bool pushed = false;

  while (pos < len) {
    char* nl = memchr(buf + from, '\n', len - from);
    if (nl == NULL) {
      if (!at_eof)
        break;
      nl = buf + len;
    }
    pushed |= parse_line(ep, buf + pos, (size_t)(nl - (buf + pos)));
    pos = from = (size_t)(nl - buf) + (nl < buf + len);
  }

  if (pos > 0) {
    memmove(buf, buf + pos, len - pos);
    sb_truncate(&ep->buf, len - pos);
  }
  ep->scanned = len - pos;
  return pushed;
}

void entry_parser_init(struct entry_parser* ep,
                       const char* name,
                       struct state* state) {
  *ep = (struct entry_parser){
      .name = name,
      .state = state,
      .buf = STRBUF_INIT,
      .words = LIST_INIT,
  };
}

void entry_parser_destroy(struct entry_parser* ep) {
  sb_free(&ep->buf);
  LIST_FREE(&ep->words);
}

bool entry_parser_feed(struct entry_parser* ep, Str data) {
  sb_append(&ep->buf, data);
  return drain_lines(ep, false);
}

bool entry_parser_finish(struct entry_parser* ep) {
  return drain_lines(ep, true);
}
