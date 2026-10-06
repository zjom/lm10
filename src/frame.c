#include "frame.h"

#include <sys/mman.h>
#include <unistd.h>

static void buffer_release(void *data, struct wl_buffer *buffer) {
  (void)buffer;
  struct frame *f = data;
  f->busy = false;
  if (f->on_release)
    f->on_release(f->release_data, f);
}
static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

enum lm_err frame_init(struct frame *f, struct wl_shm *shm, int w, int h,
                       frame_release_fn on_release, void *data) {
  *f = (struct frame){0};
  int stride = w * 4;
  size_t size = (size_t)stride * h;

  int fd = memfd_create("lm10-shm", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) < 0) {
    if (fd >= 0)
      close(fd);
    return LM_ERR_SHM;
  }

  uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) {
    close(fd);
    return LM_ERR_MMAP;
  }

  cairo_surface_t *cs = cairo_image_surface_create_for_data(
      (unsigned char *)px, CAIRO_FORMAT_RGB24, w, h, stride);
  if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(cs);
    munmap(px, size);
    close(fd);
    return LM_ERR_CAIRO;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  struct wl_buffer *buf =
      wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  *f = (struct frame){
      .buffer = buf,
      .data = px,
      .size = size,
      .width = w,
      .height = h,
      .stride = stride,
      .cs = cs,
      .on_release = on_release,
      .release_data = data,
  };
  /* f must stay at this address while the buffer exists. */
  wl_buffer_add_listener(buf, &buffer_listener, f);
  return LM_OK;
}

void frame_destroy(struct frame *f) {
  if (f->buffer)
    wl_buffer_destroy(f->buffer);
  if (f->cs)
    cairo_surface_destroy(f->cs);
  if (f->data)
    munmap(f->data, f->size);
  *f = (struct frame){0};
}
