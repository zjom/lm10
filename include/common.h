#ifndef LM_COMMON_H
#define LM_COMMON_H

enum lm_err {
  LM_OK = 0,
  LM_ERR_UNKNOWN_KEYMAP_FORMAT = -1,
  LM_ERR_KEYMAP_COMPILE_FAILED = -2,
  LM_ERR_KEY_EVENT_NO_KEYMAP = -3,
  LM_ERR_FAILED_CREATE_STATE_FROM_KEYMAP = -4,
  LM_ERR_MMAP = -5,
  LM_ERR_SHM = -6,
  LM_ERR_CAIRO = -7,
  LM_ERR_EXEC = -8,
  LM_ERR_POLL = -9,
  LM_ERR_DISPLAY = -10,
};

static inline const char* lm_strerror(enum lm_err code) {
  switch (code) {
    case LM_OK:
      return "no error";
    case LM_ERR_UNKNOWN_KEYMAP_FORMAT:
      return "unknown keymap format";
    case LM_ERR_KEYMAP_COMPILE_FAILED:
      return "failed to compile keymap";
    case LM_ERR_KEY_EVENT_NO_KEYMAP:
      return "received key event when no keymap registered";
    case LM_ERR_FAILED_CREATE_STATE_FROM_KEYMAP:
      return "xkb failed to create state from keymap";
    case LM_ERR_MMAP:
      return "mmap error";
    case LM_ERR_SHM:
      return "shm error";
    case LM_ERR_CAIRO:
      return "cairo error";
    case LM_ERR_EXEC:
      return "exec error";
    case LM_ERR_POLL:
      return "event loop error";
    case LM_ERR_DISPLAY:
      return "wayland display error";
  }
  return "unknown error";
}

#endif  // !LM_COMMON_H
