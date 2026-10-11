#include "icon.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <librsvg/rsvg.h>

#include "log.h"
#include "ztd_arena.h"
#include "ztd_arr.h"
#include "ztd_map.h"

/* Searched in this order within each directory. */
static const char* const exts[] = {".png", ".svg"};

enum icon_dir_type { DIR_FIXED, DIR_SCALABLE, DIR_THRESHOLD };

/* An existing directory of a theme, e.g. /usr/share/icons/hicolor/48x48/apps,
 * with the sizes index.theme gives it. */
struct icon_dir {
  Str path;  // NUL-terminated, lives in the cache arena
  enum icon_dir_type type;
  int size, min, max, threshold, scale;
};

typedef LIST(struct icon_dir) IconDirList;

struct icon_theme {
  Str name;
  IconDirList dirs;  // in index.theme order, then base dir order
};

typedef LIST(struct icon_theme) IconThemeList;
typedef MAP(Str, cairo_surface_t*) IconMap;
/* File names, e.g. "firefox.png"; keys in the arena. */
typedef MAP(Str, bool) NameSet;
/* Directory path -> the icon files in it, or NULL if it can't be read. */
typedef MAP(Str, NameSet*) DirMap;

struct icon_cache {
  Arena arena;  // owns everything below except icons and the scratch bufs
  Str theme;
  bool loaded;           // bases and themes have been read
  StrList bases;         // existing icon base dirs, in lookup order
  IconThemeList themes;  // theme, what it inherits (depth first), hicolor
  int px;                // size the cached icons are rendered at
  IconMap icons;         // keys in the arena; NULL values for failed lookups
  DirMap listings;       // in the arena; each directory is read once
  StrBuf path, found, found_name;  // scratch
};

static bool is_dir(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool read_file(const char* path, StrBuf* out) {
  FILE* f = fopen(path, "r");
  if (f == NULL)
    return false;
  sb_clear(out);
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    sb_append(out, str_from_parts(chunk, n));
  bool ok = !ferror(f) && !out->failed;
  fclose(f);
  return ok;
}

/* ---- themes ---- */

static void add_base(struct icon_cache* c, Str dir, const char* suffix) {
  if (dir.len == 0)
    return;
  sb_clear(&c->path);
  sb_appendf(&c->path, STR_FMT "%s", STR_ARG(dir), suffix);
  if (c->path.failed || !is_dir(sb_cstr(&c->path)))
    return;
  Str p = sb_str(&c->path);
  ARR_FOREACH(const Str, b, &c->bases) {
    if (str_eq(*b, p))
      return;
  }
  Str owned = str_arena_dup(&c->arena, p);
  if (owned.ptr != NULL)
    LIST_PUSH(&c->bases, owned);
}

/* The base directories from the icon theme spec. */
static void load_bases(struct icon_cache* c) {
  Str home = str_from_cstr(getenv("HOME"));
  Str data_home = str_from_cstr(getenv("XDG_DATA_HOME"));
  Str data_dirs = str_from_cstr(getenv("XDG_DATA_DIRS"));
  if (data_dirs.len == 0)
    data_dirs = STR("/usr/local/share:/usr/share");

  add_base(c, home, "/.icons");
  if (data_home.len > 0)
    add_base(c, data_home, "/icons");
  else
    add_base(c, home, "/.local/share/icons");
  StrSplit it = str_split(data_dirs, STR(":"));
  for (Str d; str_split_next(&it, &d);)
    add_base(c, d, "/icons");
}

/* Whether name is in a comma-separated list. */
static bool listed(Str list, Str name) {
  StrSplit it = str_split(list, STR(","));
  for (Str s; str_split_next(&it, &s);) {
    if (str_eq(str_trim(s), name))
      return true;
  }
  return false;
}

/* "[name]" -> "name"; empty if malformed. */
static Str section_name(Str line) {
  if (!str_chop_suffix(&line, STR("]")))
    return (Str){0};
  return str_trim(str_slice(line, 1, line.len));
}

/* Pushes d once for every theme root that has it. */
static void add_dir(struct icon_cache* c,
                    struct icon_theme* t,
                    const StrList* roots,
                    Str subdir,
                    struct icon_dir d) {
  if (d.size <= 0 || d.scale <= 0)
    return;
  if (d.min < 0)
    d.min = d.size;
  if (d.max < 0)
    d.max = d.size;
  ARR_FOREACH(const Str, root, roots) {
    sb_clear(&c->path);
    sb_appendf(&c->path, STR_FMT "/" STR_FMT, STR_ARG(*root), STR_ARG(subdir));
    if (c->path.failed || !is_dir(sb_cstr(&c->path)))
      continue;
    d.path = str_arena_dup(&c->arena, sb_str(&c->path));
    if (d.path.ptr != NULL)
      LIST_PUSH(&t->dirs, d);
  }
}

static void set_dir_key(struct icon_dir* d, Str key, Str val) {
  int64_t n;
  if (str_eq_cstr(key, "Type")) {
    if (str_eq_cstr(val, "Fixed"))
      d->type = DIR_FIXED;
    else if (str_eq_cstr(val, "Scalable"))
      d->type = DIR_SCALABLE;
    else if (str_eq_cstr(val, "Threshold"))
      d->type = DIR_THRESHOLD;
    return;
  }
  if (!str_to_i64(val, 10, &n) || n < 0 || n > 100000)
    return;
  if (str_eq_cstr(key, "Size"))
    d->size = (int)n;
  else if (str_eq_cstr(key, "MinSize"))
    d->min = (int)n;
  else if (str_eq_cstr(key, "MaxSize"))
    d->max = (int)n;
  else if (str_eq_cstr(key, "Threshold"))
    d->threshold = (int)n;
  else if (str_eq_cstr(key, "Scale"))
    d->scale = (int)n;
}

/* Reads the next "key = value" line, skipping blanks and comments. A section
 * header is returned as the key with an empty value and *header set. */
static bool next_entry(Str* rest, Str* key, Str* val, bool* header) {
  Str line;
  while (str_next_line(rest, &line)) {
    line = str_trim(line);
    if (line.len == 0 || line.ptr[0] == '#')
      continue;
    *header = line.ptr[0] == '[';
    if (*header) {
      *key = section_name(line);
      *val = (Str){0};
      return true;
    }
    if (str_cut(line, STR("="), key, val)) {
      *key = str_trim(*key);
      *val = str_trim(*val);
      return true;
    }
  }
  return false;
}

/* Fills t->dirs from an index.theme and returns its Inherits value, a view
 * into text. */
static Str parse_index(struct icon_cache* c,
                       struct icon_theme* t,
                       const StrList* roots,
                       Str text) {
  Str dirs = {0}, scaled = {0}, inherits = {0};
  Str rest = text, key, val, section = {0};
  bool header;
  while (next_entry(&rest, &key, &val, &header)) {
    if (header)
      section = key;
    else if (!str_eq_cstr(section, "Icon Theme"))
      continue;
    else if (str_eq_cstr(key, "Directories"))
      dirs = val;
    else if (str_eq_cstr(key, "ScaledDirectories"))
      scaled = val;
    else if (str_eq_cstr(key, "Inherits"))
      inherits = val;
  }

  /* Each listed directory has a section of its own. */
  static const struct icon_dir defaults = {
      .type = DIR_THRESHOLD, .min = -1, .max = -1, .threshold = 2, .scale = 1};
  struct icon_dir d = defaults;
  bool in_dir = false;
  rest = text;
  while (next_entry(&rest, &key, &val, &header)) {
    if (header) {
      if (in_dir)
        add_dir(c, t, roots, section, d);
      section = key;
      in_dir =
          section.len > 0 && (listed(dirs, section) || listed(scaled, section));
      d = defaults;
    } else if (in_dir) {
      set_dir_key(&d, key, val);
    }
  }
  if (in_dir)
    add_dir(c, t, roots, section, d);
  return inherits;
}

static void load_theme(struct icon_cache* c, Str name, int depth) {
  if (name.len == 0 || depth > 16)
    return;
  ARR_FOREACH(const struct icon_theme, t, &c->themes) {
    if (str_eq(t->name, name))
      return;
  }

  /* A theme may be split over several base dirs; the first index.theme
   * found describes all of them. */
  ArenaMark mark = arena_mark(&c->arena);
  StrList roots = LIST_INIT_ARENA(&c->arena);
  StrBuf text = STRBUF_INIT;
  bool have_index = false;
  ARR_FOREACH(const Str, b, &c->bases) {
    sb_clear(&c->path);
    sb_appendf(&c->path, STR_FMT "/" STR_FMT, STR_ARG(*b), STR_ARG(name));
    if (c->path.failed || !is_dir(sb_cstr(&c->path)))
      continue;
    Str root = str_arena_dup(&c->arena, sb_str(&c->path));
    if (root.ptr == NULL || !LIST_PUSH(&roots, root))
      continue;
    if (!have_index) {
      sb_append_cstr(&c->path, "/index.theme");
      have_index = !c->path.failed && read_file(sb_cstr(&c->path), &text);
    }
  }
  if (!have_index) {
    arena_rewind(&c->arena, mark);
    sb_free(&text);
    return;
  }

  struct icon_theme t = {
      .name = str_arena_dup(&c->arena, name),
      .dirs = LIST_INIT_ARENA(&c->arena),
  };
  Str inherits = parse_index(c, &t, &roots, sb_str(&text));
  if (t.name.ptr != NULL && LIST_PUSH(&c->themes, t)) {
    LOG_DEBUG("icon theme " STR_FMT ": %zu dirs", STR_ARG(t.name), t.dirs.len);
    StrSplit it = str_split(inherits, STR(","));
    for (Str parent; str_split_next(&it, &parent);)
      load_theme(c, str_trim(parent), depth + 1);
  }
  sb_free(&text);
}

/* ---- lookup ---- */

/* How far the directory's icons are from px pixels; 0 is a match. */
static int dir_distance(const struct icon_dir* d, int px) {
  int lo, hi;
  switch (d->type) {
    case DIR_FIXED:
      lo = hi = d->size;
      break;
    case DIR_SCALABLE:
      lo = d->min;
      hi = d->max;
      break;
    case DIR_THRESHOLD:
    default:
      lo = d->size - d->threshold;
      hi = d->size + d->threshold;
      break;
  }
  lo *= d->scale;
  hi *= d->scale;
  return px < lo ? lo - px : px > hi ? px - hi : 0;
}

static bool has_icon_ext(Str name) {
  for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
    if (str_ends_with(name, str_from_cstr(exts[i])))
      return true;
  }
  return false;
}

/* The icon files in dir, read on first use, so a lookup doesn't cost a
 * syscall per directory and extension. NULL if dir can't be read. */
static const NameSet* listing(struct icon_cache* c, Str dir) {
  NameSet** hit = MAP_GET(&c->listings, dir);
  if (hit != NULL)
    return *hit;

  NameSet* set = NULL;
  sb_clear(&c->path);
  sb_append(&c->path, dir);
  DIR* d = c->path.failed ? NULL : opendir(sb_cstr(&c->path));
  if (d != NULL) {
    set = ARENA_NEW_UNINIT(&c->arena, NameSet);
    if (set != NULL)
      *set = (NameSet)MAP_INIT_STR_ARENA(&c->arena);
    for (struct dirent* e; set != NULL && (e = readdir(d)) != NULL;) {
      Str name = str_from_cstr(e->d_name);
      if (!has_icon_ext(name))
        continue;
      Str owned = str_arena_dup(&c->arena, name);
      if (owned.ptr != NULL)
        MAP_PUT(set, owned, true);
    }
    closedir(d);
  }
  Str key = str_arena_dup(&c->arena, dir);
  if (key.ptr != NULL)
    MAP_PUT(&c->listings, key, set);
  return set;
}

/* Whether dir/name.ext exists for one of exts; leaves its path in c->path. */
static bool find_file(struct icon_cache* c, Str dir, Str name) {
  const NameSet* files = listing(c, dir);
  if (files == NULL)
    return false;
  for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
    sb_clear(&c->found_name);
    sb_appendf(&c->found_name, STR_FMT "%s", STR_ARG(name), exts[i]);
    if (c->found_name.failed ||
        !MAP_HAS((NameSet*)files, sb_str(&c->found_name)))
      continue;
    sb_clear(&c->path);
    sb_appendf(&c->path, STR_FMT "/" STR_FMT, STR_ARG(dir),
               STR_ARG(sb_str(&c->found_name)));
    return !c->path.failed;
  }
  return false;
}

/* The closest size in the first theme that has the icon, as the spec's
 * LookupIcon does. */
static bool find_in_theme(struct icon_cache* c,
                          const struct icon_theme* t,
                          Str name,
                          int px) {
  int best = INT_MAX;
  ARR_FOREACH(const struct icon_dir, d, &t->dirs) {
    int dist = dir_distance(d, px);
    if (dist >= best || !find_file(c, d->path, name))
      continue;
    best = dist;
    sb_clear(&c->found);
    sb_append(&c->found, sb_str(&c->path));
    if (dist == 0)
      break;
  }
  return best != INT_MAX && !c->found.failed;
}

/* Finds the file for name and leaves its path in c->found. */
static bool lookup(struct icon_cache* c, Str name, int px) {
  sb_clear(&c->found);
  if (str_find_char(name, '/') != STR_NPOS) {
    sb_append(&c->found, name);
    return !c->found.failed && access(sb_cstr(&c->found), R_OK) == 0;
  }

  /* Allow "firefox.png" for "firefox". */
  for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++)
    str_chop_suffix(&name, str_from_cstr(exts[i]));

  if (!c->loaded) {
    c->loaded = true;
    load_bases(c);
    load_theme(c, c->theme, 0);
    load_theme(c, STR("hicolor"), 0);
  }
  ARR_FOREACH(const struct icon_theme, t, &c->themes) {
    if (find_in_theme(c, t, name, px))
      return true;
  }

  /* Unthemed icons sit directly in a base dir. */
  ARR_FOREACH(const Str, b, &c->bases) {
    if (find_file(c, *b, name))
      goto found;
  }
  if (find_file(c, STR("/usr/share/pixmaps"), name))
    goto found;
  return false;

found:
  sb_append(&c->found, sb_str(&c->path));
  return !c->found.failed;
}

/* ---- loading ---- */

static bool paint_png(cairo_t* cr, const char* path, int px) {
  cairo_surface_t* img = cairo_image_surface_create_from_png(path);
  int w = cairo_image_surface_get_width(img);
  int h = cairo_image_surface_get_height(img);
  bool ok = cairo_surface_status(img) == CAIRO_STATUS_SUCCESS && w > 0 && h > 0;
  if (ok) {
    /* Fit and center, keeping the aspect ratio. */
    double s = (double)px / (w > h ? w : h);
    cairo_translate(cr, (px - w * s) / 2, (px - h * s) / 2);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
  } else {
    LOG_WARN("%s: can't load png", path);
  }
  cairo_surface_destroy(img);
  return ok;
}

static bool paint_svg(cairo_t* cr, const char* path, int px) {
  GError* err = NULL;
  RsvgHandle* h = rsvg_handle_new_from_file(path, &err);
  bool ok = h != NULL;
  if (ok) {
    /* Fits and centers the document in the viewport. */
    RsvgRectangle vp = {0, 0, px, px};
    ok = rsvg_handle_render_document(h, cr, &vp, &err);
    g_object_unref(h);
  }
  if (!ok) {
    LOG_WARN("%s: %s", path, err ? err->message : "can't load svg");
    g_clear_error(&err);
  }
  return ok;
}

static cairo_surface_t* load(const char* path, int px) {
  cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return NULL;
  }
  cairo_t* cr = cairo_create(s);
  bool ok = str_ends_with_nocase(str_from_cstr(path), STR(".svg"))
                ? paint_svg(cr, path, px)
                : paint_png(cr, path, px);
  cairo_destroy(cr);
  if (!ok) {
    cairo_surface_destroy(s);
    return NULL;
  }
  cairo_surface_flush(s);
  return s;
}

/* ---- cache ---- */

static void drop_icons(struct icon_cache* c) {
  MAP_FOREACH(e, &c->icons) {
    if (e->val != NULL)
      cairo_surface_destroy(e->val);
  }
  MAP_CLEAR(&c->icons);
}

struct icon_cache* icon_cache_new(const char* theme) {
  struct icon_cache* c = malloc(sizeof *c);
  if (c == NULL)
    return NULL;
  *c = (struct icon_cache){
      .arena = ARENA_INIT,
      .icons = MAP_INIT_STR,
      .path = STRBUF_INIT,
      .found = STRBUF_INIT,
      .found_name = STRBUF_INIT,
  };
  c->listings = (DirMap)MAP_INIT_STR_ARENA(&c->arena);
  c->bases = (StrList)LIST_INIT_ARENA(&c->arena);
  c->themes = (IconThemeList)LIST_INIT_ARENA(&c->arena);
  c->theme = str_arena_dup(&c->arena, str_from_cstr(theme));
  if (c->theme.ptr == NULL) {
    icon_cache_destroy(c);
    return NULL;
  }
  return c;
}

void icon_cache_destroy(struct icon_cache* c) {
  if (c == NULL)
    return;
  drop_icons(c);
  MAP_FREE(&c->icons);
  sb_free(&c->path);
  sb_free(&c->found);
  sb_free(&c->found_name);
  arena_free(&c->arena);
  free(c);
}

cairo_surface_t* icon_cache_get(struct icon_cache* c,
                                Str name,
                                int px,
                                bool warn) {
  if (name.len == 0 || px <= 0)
    return NULL;
  if (px != c->px) {
    drop_icons(c);
    c->px = px;
  }
  cairo_surface_t** hit = MAP_GET(&c->icons, name);
  if (hit != NULL)
    return *hit;

  cairo_surface_t* s = NULL;
  if (lookup(c, name, px)) {
    LOG_DEBUG("icon " STR_FMT ": %s", STR_ARG(name), sb_cstr(&c->found));
    s = load(sb_cstr(&c->found), px);
  } else if (warn) {
    LOG_WARN("icon " STR_FMT " not found", STR_ARG(name));
  }

  /* Failures are cached too, so they aren't looked up every frame. */
  Str key = str_arena_dup(&c->arena, name);
  if (key.ptr == NULL || !MAP_PUT(&c->icons, key, s)) {
    if (s != NULL)
      cairo_surface_destroy(s);
    return NULL;
  }
  return s;
}
