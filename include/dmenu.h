#ifndef LM_DMENU_H
#define LM_DMENU_H

#include <stdbool.h>

/* dmenu mode reads plain lines and prints the picked one as is (see
 * entry_parser), as dmenu does. It is on with --dmenu, or when lm10 is run
 * as dmenu. */
bool dmenu_wanted(int argc, char** argv);

/* Rewrites dmenu's flags in argv into lm10's, leaving everything else as is,
 * so lm10's flags still work alongside them:
 *
 *   -p TEXT  --prompt TEXT        -nb C  --color background=C
 *   -l N     --lines N            -nf C  --color text=C
 *   -fn F    --font F             -sb C  --color selection=C
 *   -b       --position bottom    -sf C  --color selection-text=C
 *   -m N     --output N           -v     --version
 *
 * -i and -f are dropped, as lm10 always ignores case and takes the keyboard
 * first; so is -w ID, with a warning, as there is no X window to embed in.
 * A flag missing its value is left for getopt to report. Returns the new
 * argv, NULL-terminated, with its length in *out_argc: one allocation, to
 * free() once it is no longer used, pointing into argv. */
char** dmenu_translate(int argc, char** argv, int* out_argc);

#endif  // !LM_DMENU_H
