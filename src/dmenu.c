#include "dmenu.h"

#include <string.h>

#include "log.h"
#include "util.h"

bool dmenu_wanted(int argc, char** argv) {
  if (argc > 0) {
    const char* slash = strrchr(argv[0], '/');
    if (strcmp(slash != NULL ? slash + 1 : argv[0], "dmenu") == 0)
      return true;
  }
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--dmenu") == 0)
      return true;
  }
  return false;
}

/* How a dmenu flag becomes lm10's. */
enum kind {
  PASS_VALUE,  // lm10 takes the flag's value
  OWN_VALUE,   // lm10 takes a value of its own
  NO_VALUE,
  COLOR,       // --color KEY=VALUE
  DROP,
  DROP_VALUE,  // the flag and its value, with a warning
};

static const struct {
  const char* dmenu;
  enum kind kind;
  const char* lm10;
  const char* value;  // OWN_VALUE's value, or COLOR's key
} flags[] = {
    {"-p", PASS_VALUE, "--prompt", NULL},
    {"-l", PASS_VALUE, "--lines", NULL},
    {"-fn", PASS_VALUE, "--font", NULL},
    {"-m", PASS_VALUE, "--output", NULL},
    {"-b", OWN_VALUE, "--position", "bottom"},
    {"-v", NO_VALUE, "--version", NULL},
    {"-nb", COLOR, "--color", "background"},
    {"-nf", COLOR, "--color", "text"},
    {"-sb", COLOR, "--color", "selection"},
    {"-sf", COLOR, "--color", "selection-text"},
    {"-i", DROP, NULL, NULL},
    {"-f", DROP, NULL, NULL},
    {"-w", DROP_VALUE, NULL, NULL},
};
enum { N_FLAGS = sizeof flags / sizeof flags[0] };

static int find_flag(const char* arg) {
  for (int i = 0; i < N_FLAGS; i++) {
    if (strcmp(arg, flags[i].dmenu) == 0)
      return i;
  }
  return -1;
}

char** dmenu_translate(int argc, char** argv, int* out_argc) {
  /* Every word becomes at most two, and only color values need copying:
   * size for both, then lay the copies out after the pointers. */
  size_t text = 0;
  for (int i = 1; i < argc; i++) {
    int f = find_flag(argv[i]);
    if (f >= 0 && flags[f].kind == COLOR && i + 1 < argc)
      text += strlen(flags[f].value) + 1 + strlen(argv[++i]) + 1;
  }
  size_t n_ptrs = 2 * (size_t)argc + 1;
  char** out = xmalloc(n_ptrs * sizeof *out + text);
  char* buf = (char*)(out + n_ptrs);

  int n = 0;
  if (argc > 0)
    out[n++] = argv[0];
  for (int i = 1; i < argc; i++) {
    int f = find_flag(argv[i]);
    if (f < 0) {
      out[n++] = argv[i];
      continue;
    }
    bool has_next = i + 1 < argc;
    switch (flags[f].kind) {
      case DROP:
        break;
      case DROP_VALUE:
        LOG_WARN("%s: there is no X window to embed in on Wayland, ignoring",
                 argv[i]);
        i += has_next;
        break;
      case NO_VALUE:
        out[n++] = (char*)flags[f].lm10;
        break;
      case OWN_VALUE:
        out[n++] = (char*)flags[f].lm10;
        out[n++] = (char*)flags[f].value;
        break;
      case PASS_VALUE:
        out[n++] = (char*)flags[f].lm10;
        if (has_next)
          out[n++] = argv[++i];
        break;
      case COLOR:
        out[n++] = (char*)flags[f].lm10;
        if (has_next) {
          const char* val = argv[++i];
          size_t k = strlen(flags[f].value), v = strlen(val);
          memcpy(buf, flags[f].value, k);
          buf[k] = '=';
          memcpy(buf + k + 1, val, v + 1);
          out[n++] = buf;
          buf += k + 1 + v + 1;
        }
        break;
    }
  }
  out[n] = NULL;
  *out_argc = n;
  return out;
}
