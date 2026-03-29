#include "msgq/visionipc/visionbuf.h"

#include <atomic>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#ifndef __APPLE__
#include <linux/dma-heap.h>
#endif

std::atomic<int> offset = 0;

#ifndef __APPLE__
static int dma_heap_fd = -2;  // -2 = not tried yet

static int get_dma_heap_fd() {
  if (dma_heap_fd == -2) {
    dma_heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
  }
  return dma_heap_fd;
}

static bool alloc_dma_buf(size_t len, int *fd) {
  int heap = get_dma_heap_fd();
  if (heap < 0) return false;

  struct dma_heap_allocation_data alloc = {};
  alloc.len = len;
  alloc.fd_flags = O_RDWR | O_CLOEXEC;
  if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) return false;

  *fd = alloc.fd;
  return true;
}
#endif

static void *malloc_with_fd(size_t len, int *fd) {
#ifndef __APPLE__
  // Try DMA-BUF heap first (GPU-importable via EGL)
  if (alloc_dma_buf(len, fd)) {
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (addr != MAP_FAILED) {
      memset(addr, 0, len);
      return addr;
    }
    close(*fd);
  }
#endif

  // Fallback: shm (works everywhere but not GPU-importable)
  char full_path[0x100];
#ifdef __APPLE__
  snprintf(full_path, sizeof(full_path)-1, "/tmp/visionbuf_%d_%d", getpid(), offset++);
#else
  snprintf(full_path, sizeof(full_path)-1, "/dev/shm/msgq_visionbuf_%d_%d", getpid(), offset++);
#endif

  *fd = open(full_path, O_RDWR | O_CREAT, 0664);
  assert(*fd >= 0);

  unlink(full_path);

  int ret = ftruncate(*fd, len);
  assert(ret == 0);
  void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
  assert(addr != MAP_FAILED);

  return addr;
}

void VisionBuf::allocate(size_t length) {
  this->len = length;
  this->mmap_len = this->len + sizeof(uint64_t);
  this->addr = malloc_with_fd(this->mmap_len, &this->fd);
  this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
}

void VisionBuf::import(){
  assert(this->fd >= 0);
  this->addr = mmap(NULL, this->mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, this->fd, 0);
  assert(this->addr != MAP_FAILED);

  this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
}

void VisionBuf::init_yuv(size_t init_width, size_t init_height, size_t init_stride, size_t init_uv_offset){
  this->width = init_width;
  this->height = init_height;
  this->stride = init_stride;
  this->uv_offset = init_uv_offset;

  this->y = (uint8_t *)this->addr;
  this->uv = this->y + this->uv_offset;
}

int VisionBuf::sync(int dir) {
  return 0;
}

int VisionBuf::free() {
  int err = munmap(this->addr, this->mmap_len);
  if (err != 0) return err;

  err = close(this->fd);
  return err;
}

uint64_t VisionBuf::get_frame_id() {
  return *frame_id;
}

void VisionBuf::set_frame_id(uint64_t id) {
  *frame_id = id;
}
