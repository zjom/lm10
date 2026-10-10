#ifndef LM_CONFIG_H
#define LM_CONFIG_H

#include "kb.h"
#include "ztd_arr.h"

#include <stdbool.h>

struct config_bind {
  struct kb_keypress key;
  enum kb_action action;
};

typedef LIST(struct config_bind) ConfigBindList;


struct config {
  ConfigBindList binds;  // Binds are applied in order, so a later bind for the same key overrides an earlier one.
};

void config_init(struct config* cfg);
void config_destroy(struct config* cfg);

/* Appends the built-in binds. Returns false on allocation failure. */
bool config_load_defaults(struct config* cfg);

#endif  // !LM_CONFIG_H
