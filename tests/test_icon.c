#include "icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cairo.h>

#include "test.h"

static char root[] = "/tmp/lm10-icon-test-XXXXXX";

static void mkdirs(const char* rel) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", root, rel);
  for (char* p = path + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(path, 0700);
      *p = '/';
    }
  }
  mkdir(path, 0700);
}

static void write_file(const char* rel, const char* text) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", root, rel);
  FILE* f = fopen(path, "w");
  CHECK(f != NULL);
  if (f == NULL)
    return;
  fputs(text, f);
  fclose(f);
}

/* A size x size PNG, solid red. */
static void write_png(const char* rel, int size) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", root, rel);
  cairo_surface_t* s =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t* cr = cairo_create(s);
  cairo_set_source_rgb(cr, 1, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);
  CHECK(cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(s);
}

/* The red channel of the icon's centre pixel, or -1 without an icon. */
static int center_red(cairo_surface_t* s) {
  if (s == NULL)
    return -1;
  int w = cairo_image_surface_get_width(s);
  int stride = cairo_image_surface_get_stride(s);
  const uint32_t* px =
      (const uint32_t*)(cairo_image_surface_get_data(s) + (w / 2) * stride);
  return (int)(px[w / 2] >> 16 & 0xff);
}

static void setup(void) {
  CHECK(mkdtemp(root) != NULL);
  mkdirs("share/icons/hicolor/16x16/apps");
  mkdirs("share/icons/hicolor/scalable/apps");
  mkdirs("share/icons/mytheme/48x48/apps");
  mkdirs("share/pixmaps");
  write_file("share/icons/hicolor/index.theme",
             "[Icon Theme]\n"
             "Name=Hicolor\n"
             "Directories=16x16/apps,scalable/apps\n"
             "\n"
             "[16x16/apps]\n"
             "Size=16\n"
             "Type=Fixed\n"
             "\n"
             "[scalable/apps]\n"
             "Size=64\n"
             "MinSize=8\n"
             "MaxSize=512\n"
             "Type=Scalable\n");
  write_file("share/icons/mytheme/index.theme",
             "[Icon Theme]\n"
             "Name=Mine\n"
             "Inherits=hicolor\n"
             "Directories=48x48/apps\n"
             "\n"
             "[48x48/apps]\n"
             "Size=48\n"
             "Type=Fixed\n");
  write_png("share/icons/hicolor/16x16/apps/fixed.png", 16);
  write_file("share/icons/hicolor/scalable/apps/vector.svg",
             "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'>"
             "<rect width='10' height='10' fill='#00ff00'/></svg>");
  write_png("share/icons/mytheme/48x48/apps/themed.png", 48);
  write_png("share/pixmaps/legacy.png", 32);

  char data_dirs[600];
  snprintf(data_dirs, sizeof data_dirs, "%s/share", root);
  setenv("XDG_DATA_DIRS", data_dirs, 1);
  setenv("XDG_DATA_HOME", "/nonexistent", 1);
  setenv("HOME", "/nonexistent", 1);
}

static void test_lookup(void) {
  struct icon_cache* c = icon_cache_new("mytheme");
  CHECK(c != NULL);

  CHECK(center_red(icon_cache_get(c, STR("fixed"), 16, true)) == 255);
  CHECK(center_red(icon_cache_get(c, STR("fixed.png"), 16, true)) == 255);
  /* Inherited from hicolor; scaled to the size asked for. */
  cairo_surface_t* s = icon_cache_get(c, STR("vector"), 24, true);
  CHECK(s != NULL && cairo_image_surface_get_width(s) == 24);
  CHECK(center_red(s) == 0);
  CHECK(center_red(icon_cache_get(c, STR("themed"), 24, true)) == 255);
  CHECK(icon_cache_get(c, STR("missing"), 24, false) == NULL);
  CHECK(icon_cache_get(c, STR(""), 24, false) == NULL);

  /* Paths are taken as is. */
  char path[600];
  snprintf(path, sizeof path, "%s/share/pixmaps/legacy.png", root);
  CHECK(center_red(icon_cache_get(c, str_from_cstr(path), 24, true)) == 255);
  icon_cache_destroy(c);
}

static void cleanup(void) {
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  CHECK(system(cmd) == 0);
}

int main(void) {
  setup();
  RUN(test_lookup);
  cleanup();
  return TEST_RESULT;
}
