#include "input.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "log.h"

static bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

/* Pulls the next shell-style word out of [*p, end), unquoting it in place:
 * '...' is literal, "..." honours \" and \\, and a bare backslash escapes
 * the next character. Returns false at end of line; sets *err on an
 * unterminated quote or a trailing backslash. */
static bool next_word(char** p, char* end, Str* word, bool* err) {
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

/* Parses one line (mutated in place) and pushes the entry it describes.
 * Returns true if an entry was pushed. */
static bool parse_line(struct input* in, char* line, size_t len) {
  char* p = line;
  char* end = line + len;
  Str name = {0}, icon = {0};
  bool detached = false, err = false;
  Str word;

  LIST_CLEAR(&in->words);

  /* options, up to the first word that isn't one (or "--") */
  while (next_word(&p, end, &word, &err)) {
    if (str_eq(word, STR("--"))) {
      if (!next_word(&p, end, &word, &err))
        break;
      if (!LIST_PUSH(&in->words, word))
        goto oom;
      break;
    }
    if (word.len < 2 || word.ptr[0] != '-') {
      if (!LIST_PUSH(&in->words, word))
        goto oom;
      break;
    }

    if (str_eq(word, STR("-D")) || str_eq(word, STR("--detached"))) {
      detached = true;
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

    if (str_eq(word, STR("--detached"))) {
      LOG_WARN("stdin: option \"--detached\" takes no value, skipping line");
      return false;
    } else if (str_eq(word, STR("-n")) || str_eq(word, STR("--name"))) {
      dst = &name;
    } else if (str_eq(word, STR("-i")) || str_eq(word, STR("--icon"))) {
      dst = &icon;
    } else {
      LOG_WARN("stdin: unknown option \"%.*s\", skipping line", (int)word.len,
               word.ptr);
      return false;
    }

    if (!has_val && !next_word(&p, end, &val, &err)) {
      if (!err) {
        LOG_WARN("stdin: option \"%.*s\" needs a value, skipping line",
                 (int)word.len, word.ptr);
        return false;
      }
      break;
    }
    *dst = val;
  }

  /* the rest is the command and its arguments */
  while (!err && in->words.len > 0 && next_word(&p, end, &word, &err)) {
    if (!LIST_PUSH(&in->words, word))
      goto oom;
  }

  if (err) {
    LOG_WARN("stdin: unterminated quote or escape, skipping line");
    return false;
  }
  if (in->words.len == 0) {
    if (str_trim(str_from_parts(line, len)).len != 0)
      LOG_WARN("stdin: line has no command, skipping");
    return false;
  }

  if (!state_push(in->state, name, &in->words, icon, detached))
    goto oom;
  return true;

oom:
  LOG_WARN("entry allocation failed");
  return false;
}

/* Parses every complete line in the buffer, plus the trailing partial one if
 * at_eof. Returns true if any entry was pushed. */
static bool drain_lines(struct input* in, bool at_eof) {
  char* buf = in->buf.ptr;
  size_t len = in->buf.len;
  size_t pos = 0;
  bool pushed = false;

  while (pos < len) {
    char* nl = memchr(buf + pos, '\n', len - pos);
    if (nl == NULL) {
      if (!at_eof)
        break;
      nl = buf + len;
    }
    pushed |= parse_line(in, buf + pos, (size_t)(nl - (buf + pos)));
    pos = (size_t)(nl - buf) + (nl < buf + len);
  }

  if (pos > 0) {
    memmove(buf, buf + pos, len - pos);
    sb_truncate(&in->buf, len - pos);
  }
  return pushed;
}

static void stop(struct input* in) {
  if (in->fd == -1)
    return;
  fdm_remove(in->fdm, (uint32_t)in->fd);
  in->fd = -1;
  sb_free(&in->buf);
  LIST_FREE(&in->words);
}

/* One read. Returns true while there may be more to read. */
static bool read_some(struct input* in, bool* pushed) {
  char chunk[4096];
  ssize_t n = read(in->fd, chunk, sizeof chunk);
  if (n == -1) {
    if (errno == EINTR || errno == EAGAIN)
      return true;
    LOG_WARN("stdin: read failed: %s", strerror(errno));
  } else if (n > 0) {
    if (sb_append(&in->buf, str_from_parts(chunk, (size_t)n))) {
      *pushed |= drain_lines(in, false);
      return true;
    }
    LOG_WARN("stdin: allocation failed");
  }
  *pushed |= drain_lines(in, true);
  return false;
}

static int32_t on_readable(struct fdm* fdm, void* data) {
  (void)fdm;
  struct input* in = data;
  bool pushed = false;
  /* Level-triggered, so one read per wakeup: never blocks, and fd stays in
   * whatever blocking mode it was handed to us in. */
  if (!read_some(in, &pushed))
    stop(in);
  if (pushed && in->on_change)
    in->on_change(in->data);
  return 0;
}

int input_init(struct input* in,
               struct fdm* fdm,
               int fd,
               struct state* state,
               void (*on_change)(void* data),
               void* data) {
  *in = (struct input){
      .fdm = fdm,
      .fd = fd,
      .buf = STRBUF_INIT,
      .words = LIST_INIT,
      .state = state,
      .on_change = on_change,
      .data = data,
  };

  if (fdm_add(fdm, (uint32_t)fd, EPOLLIN, in, on_readable, 0) == 0)
    return 0;

  if (errno != EPERM) {
    LOG_ERROR("failed to watch stdin: %s", strerror(errno));
    in->fd = -1;
    return -1;
  }

  /* Regular files and the like can't be polled, but never block either. */
  bool pushed = false;
  while (read_some(in, &pushed))
    ;
  in->fd = -1;
  sb_free(&in->buf);
  LIST_FREE(&in->words);
  if (pushed && on_change)
    on_change(data);
  return 0;
}

void input_destroy(struct input* in) {
  stop(in);
}
