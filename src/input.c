#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "json.h"
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
  enum state_run run = STATE_RUN_PRINT;
  bool err = false;
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
      LOG_WARN("%s: option \"%.*s\" takes no value, skipping line", in->name,
               (int)word.len, word.ptr);
      return false;
    } else if (str_eq(word, STR("-n")) || str_eq(word, STR("--name"))) {
      dst = &name;
    } else if (str_eq(word, STR("-i")) || str_eq(word, STR("--icon"))) {
      dst = &icon;
    } else {
      LOG_WARN("%s: unknown option \"%.*s\", skipping line", in->name,
               (int)word.len, word.ptr);
      return false;
    }

    if (!has_val && !next_word(&p, end, &val, &err)) {
      if (!err) {
        LOG_WARN("%s: option \"%.*s\" needs a value, skipping line", in->name,
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
    LOG_WARN("%s: unterminated quote or escape, skipping line", in->name);
    return false;
  }
  if (in->words.len == 0) {
    if (str_trim(str_from_parts(line, len)).len != 0)
      LOG_WARN("%s: line has no command, skipping", in->name);
    return false;
  }

  if (!state_push(in->state, name, &in->words, icon, run))
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

/* Fills in->words from a "command" value: an array of strings taken as is,
 * or a string split like a line. Returns false (having warned) if it is
 * neither. */
static bool json_command(struct input* in,
                         struct json_value_s* v,
                         size_t line) {
  LIST_CLEAR(&in->words);
  Str word;

  struct json_array_s* arr = json_value_as_array(v);
  if (arr != NULL) {
    for (struct json_array_element_s* el = arr->start; el; el = el->next) {
      if (!json_str(el->value, &word)) {
        LOG_WARN(
            "%s:%zu: \"command\" elements must be strings, skipping "
            "entry",
            in->name, json_line(el->value, line));
        return false;
      }
      if (!LIST_PUSH(&in->words, word))
        goto oom;
    }
    return true;
  }

  Str cmd;
  if (!json_str(v, &cmd)) {
    LOG_WARN(
        "%s:%zu: \"command\" must be a string or an array of "
        "strings, skipping entry",
        in->name, json_line(v, line));
    return false;
  }
  /* The string lives in json.h's allocation, which is ours to mutate. */
  char* p = (char*)cmd.ptr;
  char* end = p + cmd.len;
  bool err = false;
  while (next_word(&p, end, &word, &err)) {
    if (!LIST_PUSH(&in->words, word))
      goto oom;
  }
  if (err) {
    LOG_WARN(
        "%s:%zu: unterminated quote or escape in \"command\", "
        "skipping entry",
        in->name, json_line(v, line));
    return false;
  }
  return true;

oom:
  LOG_WARN("entry allocation failed");
  return false;
}

/* Pushes the entry an object describes. Returns true if one was pushed. */
static bool json_entry(struct input* in, struct json_value_s* v, size_t line) {
  struct json_object_s* obj = json_value_as_object(v);
  if (obj == NULL) {
    LOG_WARN("%s:%zu: expected an entry object, skipping", in->name,
             json_line(v, line));
    return false;
  }

  Str name = {0}, icon = {0};
  enum state_run run = STATE_RUN_PRINT;
  LIST_CLEAR(&in->words);

  for (struct json_object_element_s* el = obj->start; el; el = el->next) {
    Str key = str_from_parts(el->name->string, el->name->string_size);
    struct json_value_s* val = el->value;
    size_t at = json_line(val, line);
    Str s;

    if (str_eq(key, STR("command"))) {
      if (!json_command(in, val, line))
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
            in->name, at);
        return false;
      }
    } else {
      LOG_WARN("%s:%zu: unknown key \"" STR_FMT "\", ignoring", in->name, at,
               STR_ARG(key));
    }
    continue;

  not_string:
    LOG_WARN("%s:%zu: \"" STR_FMT "\" must be a string, skipping entry",
             in->name, at, STR_ARG(key));
    return false;
  }

  if (in->words.len == 0) {
    LOG_WARN("%s:%zu: entry has no command, skipping", in->name,
             json_line(v, line));
    return false;
  }
  if (!state_push(in->state, name, &in->words, icon, run)) {
    LOG_WARN("entry allocation failed");
    return false;
  }
  return true;
}

/* Parses one complete value, starting on the given input line, and pushes
 * the entries in it: an entry object or an array of them. Returns true if
 * any entry was pushed. */
static bool json_value(struct input* in,
                       const char* src,
                       size_t len,
                       size_t line) {
  struct json_parse_result_s res;
  struct json_value_s* root =
      json_parse_ex(src, len, JSON_FLAGS, NULL, NULL, &res);
  if (root == NULL) {
    LOG_WARN("%s:%zu: %s, skipping", in->name, line + res.error_line_no - 1,
             json_strerror(res.error));
    return false;
  }

  bool pushed = false;
  struct json_array_s* arr = json_value_as_array(root);
  if (arr != NULL) {
    for (struct json_array_element_s* el = arr->start; el; el = el->next)
      pushed |= json_entry(in, el->value, line);
  } else {
    pushed = json_entry(in, root, line);
  }
  free(root);
  return pushed;
}

/* Warns once per run of bytes that can't start a value. */
static void json_stray(struct input* in) {
  if (!in->json.stray)
    LOG_WARN("%s:%zu: expected an entry object, skipping", in->name,
             in->json.line);
  in->json.stray = true;
}

/* Finds where each top-level value ends, as well as each element of a
 * top-level array, so they can be parsed (and fail) one at a time, and
 * parses them. Returns true if any entry was pushed. */
static bool drain_json(struct input* in, bool at_eof) {
  struct input_json* s = &in->json;
  char* buf = in->buf.ptr;
  size_t len = in->buf.len;
  bool pushed = false;

  for (; s->pos < len; s->pos++) {
    char c = buf[s->pos];
    if (c == '\n')
      s->line++;

    if (s->comment == INPUT_JSON_LINE_COMMENT) {
      if (c == '\n')
        s->comment = INPUT_JSON_NO_COMMENT;
      continue;
    }
    if (s->comment == INPUT_JSON_BLOCK_COMMENT) {
      if (c == '*' && s->pos + 1 == len && !at_eof)
        break; /* the '/' may be in the next read */
      if (c == '*' && s->pos + 1 < len && buf[s->pos + 1] == '/') {
        s->comment = INPUT_JSON_NO_COMMENT;
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
          goto out; /* the next byte may be in the next read */
        if (s->pos + 1 < len && buf[s->pos + 1] == '/') {
          s->comment = INPUT_JSON_LINE_COMMENT;
          s->pos++;
        } else if (s->pos + 1 < len && buf[s->pos + 1] == '*') {
          s->comment = INPUT_JSON_BLOCK_COMMENT;
          s->pos++;
        } else if (s->depth == 0) {
          json_stray(in);
        }
        break;
      case '"':
        s->in_str = true;
        if (s->depth == 0)
          json_stray(in);
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
            pushed |= json_value(in, buf + s->start, s->pos + 1 - s->start,
                                 s->start_line);
        } else if (c == ']' && s->in_array) {
          s->in_array = false;
        } else {
          json_stray(in);
        }
        break;
      case ',':
        if (s->depth == 0 && !s->in_array)
          json_stray(in);
        break;
      default:
        if (s->depth == 0)
          json_stray(in);
        break;
    }
  }
out:

  if (at_eof && (s->depth > 0 || s->in_array))
    LOG_WARN("%s:%zu: unexpected end of input, skipping", in->name,
             s->depth > 0 ? s->start_line : s->line);

  /* Drop what has been consumed, keeping a value still being read. */
  size_t keep = s->depth > 0 ? s->start : s->pos;
  if (keep > 0) {
    memmove(buf, buf + keep, len - keep);
    sb_truncate(&in->buf, len - keep);
    s->pos -= keep;
    s->start = 0; /* the value being read, if any, now starts the buffer */
  }
  return pushed;
}

/* Picks the format from the first byte that isn't whitespace: JSON if it
 * opens an object, an array or a comment, lines otherwise. */
static enum input_format detect_format(const struct input* in, bool at_eof) {
  const char* buf = in->buf.ptr;
  size_t len = in->buf.len;
  size_t i = 0;
  while (i < len && (is_space(buf[i]) || buf[i] == '\n'))
    i++;
  if (i == len)
    return at_eof ? INPUT_FORMAT_LINES : INPUT_FORMAT_UNKNOWN;
  if (buf[i] == '{' || buf[i] == '[')
    return INPUT_FORMAT_JSON;
  if (buf[i] == '/') {
    if (i + 1 == len)
      return at_eof ? INPUT_FORMAT_LINES : INPUT_FORMAT_UNKNOWN;
    if (buf[i + 1] == '/' || buf[i + 1] == '*')
      return INPUT_FORMAT_JSON;
  }
  return INPUT_FORMAT_LINES;
}

/* Parses whatever complete entries the buffer holds, and the rest if
 * at_eof. Returns true if any entry was pushed. */
static bool drain(struct input* in, bool at_eof) {
  if (in->format == INPUT_FORMAT_UNKNOWN)
    in->format = detect_format(in, at_eof);
  switch (in->format) {
    case INPUT_FORMAT_LINES:
      return drain_lines(in, at_eof);
    case INPUT_FORMAT_JSON:
      return drain_json(in, at_eof);
    default:
      return false;
  }
}

/* Stops reading; at_eof says the command, if any, is done writing, so it is
 * waited for rather than killed. */
static void stop(struct input* in, bool at_eof) {
  if (in->fd == -1)
    return;
  fdm_remove(in->fdm, (uint32_t)in->fd);
  if (in->pid > 0) {
    close(in->fd);
    if (!at_eof)
      kill(-in->pid, SIGTERM); /* its process group: sh and its children */
    while (waitpid(in->pid, NULL, 0) == -1 && errno == EINTR)
      ;
    in->pid = -1;
  }
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
    LOG_WARN("%s: read failed: %s", in->name, strerror(errno));
  } else if (n > 0) {
    if (sb_append(&in->buf, str_from_parts(chunk, (size_t)n))) {
      *pushed |= drain(in, false);
      return true;
    }
    LOG_WARN("%s: allocation failed", in->name);
  }
  *pushed |= drain(in, true);
  return false;
}

static int32_t on_readable(struct fdm* fdm, void* data) {
  (void)fdm;
  struct input* in = data;
  bool pushed = false;
  /* Level-triggered, so one read per wakeup: never blocks, and fd stays in
   * whatever blocking mode it was handed to us in. */
  if (!read_some(in, &pushed))
    stop(in, true);
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
      .name = "stdin",
      .fdm = fdm,
      .fd = fd,
      .pid = -1,
      .buf = STRBUF_INIT,
      .words = LIST_INIT,
      .json = {.line = 1},
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

int input_init_command(struct input* in,
                       struct fdm* fdm,
                       const char* cmd,
                       struct state* state,
                       void (*on_change)(void* data),
                       void* data) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) == -1) {
    LOG_ERROR("reload: failed to create pipe: %s", strerror(errno));
    return -1;
  }

  pid_t pid = fork();
  if (pid == -1) {
    LOG_ERROR("reload: fork failed: %s", strerror(errno));
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    /* Its own process group, so stop() can kill whatever it started. */
    setpgid(0, 0);
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull == -1 || dup2(devnull, STDIN_FILENO) == -1 ||
        dup2(fds[1], STDOUT_FILENO) == -1)
      _exit(127);
    execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
    _exit(127);
  }
  setpgid(pid, pid); /* as the child does, whichever runs first */
  close(fds[1]);

  *in = (struct input){
      .name = "reload",
      .fdm = fdm,
      .fd = fds[0],
      .pid = pid,
      .buf = STRBUF_INIT,
      .words = LIST_INIT,
      .json = {.line = 1},
      .state = state,
      .on_change = on_change,
      .data = data,
  };
  if (fdm_add(fdm, (uint32_t)fds[0], EPOLLIN, in, on_readable, 0) != 0) {
    LOG_ERROR("reload: failed to watch output: %s", strerror(errno));
    close(fds[0]);
    kill(-pid, SIGTERM);
    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
      ;
    in->fd = -1;
    in->pid = -1;
    return -1;
  }
  return 0;
}

void input_destroy(struct input* in) {
  stop(in, false);
}
