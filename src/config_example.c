/* Built as C23 for #embed; the rest of lm10 is C11. */
#include "config.h"

const char config_example[] = {
#embed "../examples/lm10.ini"
};
const size_t config_example_len = sizeof config_example;
