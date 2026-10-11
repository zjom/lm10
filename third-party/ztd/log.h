/* log.h - minimal header-only logging macros for C99+
 *
 * Usage:
 *   #include "log.h"
 *   LOG_INFO("server started on port %d", port);
 *   LOG_ERROR("open failed: %s", strerror(errno));
 *
 * In exactly one .c file, define LOG_IMPLEMENTATION before including it:
 * that file holds the runtime level and stream every file shares.
 *
 * Compile-time filtering (calls below this level compile to nothing):
 *   cc -DLOG_LEVEL=LOG_LEVEL_WARN ...
 * Runtime filtering:
 *   log_set_level(LOG_LEVEL_DEBUG);
 * Colors are used when the stream is a terminal. Disable them entirely:
 *   cc -DLOG_NO_COLOR ...
 */
#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_LEVEL_TRACE 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_WARN 3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_FATAL 5
#define LOG_LEVEL_OFF 6

/* Compile-time minimum level */
#ifndef LOG_LEVEL
#ifdef NDEBUG
#define LOG_LEVEL LOG_LEVEL_INFO
#else
#define LOG_LEVEL LOG_LEVEL_TRACE
#endif
#endif

/* Strip directories from __FILE__ for shorter output */
#define LOG_FILENAME_ \
  (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

/* Runtime minimum level and stream, shared by every file */
extern int log_runtime_level_;
extern FILE* log_stream_; /* NULL means stderr */
#ifdef LOG_IMPLEMENTATION
int log_runtime_level_ = LOG_LEVEL_TRACE;
FILE* log_stream_ = NULL;
#endif

static inline void log_set_level(int level) {
  log_runtime_level_ = level;
}
static inline void log_set_stream(FILE* fp) {
  log_stream_ = fp;
}

#if defined(__GNUC__) || defined(__clang__)
#define LOG_PRINTF_(fmt_idx, arg_idx) \
  __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define LOG_PRINTF_(fmt_idx, arg_idx)
#endif

static inline void log_write_(int level,
                              const char* file,
                              int line,
                              const char* func,
                              const char* fmt,
                              ...) LOG_PRINTF_(5, 6);

static inline void log_write_(int level,
                              const char* file,
                              int line,
                              const char* func,
                              const char* fmt,
                              ...) {
  static const char* names[] = {"TRACE", "DEBUG", "INFO ",
                                "WARN ", "ERROR", "FATAL"};
  FILE* out = log_stream_ ? log_stream_ : stderr;
#ifndef LOG_NO_COLOR
  static const char* colors[] = {"\x1b[90m", "\x1b[36m", "\x1b[32m",
                                 "\x1b[33m", "\x1b[31m", "\x1b[35m"};
  bool tty = isatty(fileno(out));
  const char *color = tty ? colors[level] : "", *reset = tty ? "\x1b[0m" : "";
#else
  const char *color = "", *reset = "";
#endif
  char ts[20], msg[1024];
  time_t now;
  struct tm* tm;
  va_list ap;

  if (level < log_runtime_level_)
    return;

  now = time(NULL);
  tm = localtime(&now);
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", tm);

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  /* Single fprintf so lines from different threads don't interleave */
  fprintf(out, "%s %s%s%s %s:%d %s(): %s\n", ts, color, names[level], reset,
          file, line, func, msg);
  if (level >= LOG_LEVEL_ERROR)
    fflush(out);
}

/* Format string is part of __VA_ARGS__ so zero-arg calls work in plain C99 */
#define LOG_AT_(lvl, ...)                                              \
  do {                                                                 \
    log_write_((lvl), LOG_FILENAME_, __LINE__, __func__, __VA_ARGS__); \
  } while (0)

/* Disabled levels: dead code, so arguments are still type-checked
 * (no unused-variable warnings) but the optimizer removes the call. */
#define LOG_NOOP_(...)                       \
  do {                                       \
    if (0)                                   \
      log_write_(0, "", 0, "", __VA_ARGS__); \
  } while (0)

#if LOG_LEVEL <= LOG_LEVEL_TRACE
#define LOG_TRACE(...) LOG_AT_(LOG_LEVEL_TRACE, __VA_ARGS__)
#else
#define LOG_TRACE(...) LOG_NOOP_(__VA_ARGS__)
#endif

#if LOG_LEVEL <= LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) LOG_AT_(LOG_LEVEL_DEBUG, __VA_ARGS__)
#else
#define LOG_DEBUG(...) LOG_NOOP_(__VA_ARGS__)
#endif

#if LOG_LEVEL <= LOG_LEVEL_INFO
#define LOG_INFO(...) LOG_AT_(LOG_LEVEL_INFO, __VA_ARGS__)
#else
#define LOG_INFO(...) LOG_NOOP_(__VA_ARGS__)
#endif

#if LOG_LEVEL <= LOG_LEVEL_WARN
#define LOG_WARN(...) LOG_AT_(LOG_LEVEL_WARN, __VA_ARGS__)
#else
#define LOG_WARN(...) LOG_NOOP_(__VA_ARGS__)
#endif

#if LOG_LEVEL <= LOG_LEVEL_ERROR
#define LOG_ERROR(...) LOG_AT_(LOG_LEVEL_ERROR, __VA_ARGS__)
#else
#define LOG_ERROR(...) LOG_NOOP_(__VA_ARGS__)
#endif

#if LOG_LEVEL <= LOG_LEVEL_FATAL
#define LOG_FATAL(...) LOG_AT_(LOG_LEVEL_FATAL, __VA_ARGS__)
#else
#define LOG_FATAL(...) LOG_NOOP_(__VA_ARGS__)
#endif

#endif /* LOG_H */
