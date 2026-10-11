#ifndef LM_ICON_H
#define LM_ICON_H

#include "ztd_str.h"

#include <cairo.h>

#include <stdbool.h>

/* Finds icons by freedesktop icon theme name (or by path, if the name has a
 * '/') and keeps them rasterized. PNG and SVG files are supported. */
struct icon_cache;

/* theme is the preferred icon theme; hicolor is always the last fallback.
 * Themes are only read on the first lookup. Returns NULL on allocation
 * failure. */
struct icon_cache* icon_cache_new(const char* theme);
void icon_cache_destroy(struct icon_cache* c);

/* The icon as a px by px image surface, or NULL if it can't be found or
 * loaded (looked up only once; warns then if warn is set). Owned by the cache;
 * valid until a call with a different px, which drops everything rendered at
 * the old size. */
cairo_surface_t* icon_cache_get(struct icon_cache* c,
                                Str name,
                                int px,
                                bool warn);

#endif  // !LM_ICON_H
