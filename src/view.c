#include "view.h"

void view_init(struct view *view) {
  str_arr_init(&view->items);
  view->selected = 0;
  view->gen = 0;
}

void view_destroy(struct view *view) {
  str_arr_free(&view->items);
  view->selected = 0;
  view->gen = 0;
}

int view_push(struct view *view, const char *s, size_t len) {
  if (str_arr_push(&view->items, s, len) < 0)
    return -1;
  view->gen++;
  return 0;
}

void view_select_next(struct view *view) {
  size_t n = str_arr_len(&view->items);
  if (n == 0)
    return;
  view->selected = view->selected + 1 >= n ? 0 : view->selected + 1;
  view->gen++;
}

void view_select_prev(struct view *view) {
  size_t n = str_arr_len(&view->items);
  if (n == 0)
    return;
  view->selected = view->selected == 0 ? n - 1 : view->selected - 1;
  view->gen++;
}
