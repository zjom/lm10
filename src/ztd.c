#include "util.h"

#include <stdlib.h>

/* ztd allocates through these, so its containers never see a failed
 * allocation. */
#define ZTD_MALLOC(n) xmalloc(n)
#define ZTD_REALLOC(p, n) xrealloc(p, n)
#define ZTD_FREE(p) free(p)

#define LOG_IMPLEMENTATION
#define ZTD_ARENA_IMPLEMENTATION
#define ZTD_STR_IMPLEMENTATION
#define ZTD_ARR_IMPLEMENTATION
#define ZTD_MAP_IMPLEMENTATION
#include "log.h"
#include "ztd_arena.h"
#include "ztd_arr.h"
#include "ztd_map.h"
#include "ztd_str.h"
