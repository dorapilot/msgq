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
#include <system_error>

#ifndef __APPLE__
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#endif

std::atomic<int> offset = 0;

#ifndef __APPLE__
static int ioctl_retry(int fd, unsigned long request, void *arg) {
  int ret;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret < 0 && errno == EINTR);
  return ret;
}

static int get_dma_heap_fd() {
  static const int heap_fd = []() {
    int fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno != ENOENT && errno != ENODEV) {
      throw std::system_error(errno, std::generic_category(), "open DMA heap");
    }
    return fd;
  }();
  return heap_fd;
}

static bool alloc_dma_buf(size_t len, int *fd) {
  int heap = get_dma_heap_fd();
  if (heap < 0) return false;

  struct dma_heap_allocation_data alloc = {};
  alloc.len = len;
  alloc.fd_flags = O_RDWR | O_CLOEXEC;
  if (ioctl_retry(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
    throw std::system_error(errno, std::generic_category(), "allocate DMA-BUF");
  }

  *fd = alloc.fd;
  return true;
}
#endif

static void *malloc_with_fd(size_t len, int *fd, bool *is_dma_buf) {
#ifndef __APPLE__
  if (alloc_dma_buf(len, fd)) {
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (addr != MAP_FAILED) {
      *is_dma_buf = true;
      return addr;
    }
    int err = errno;
    close(*fd);
    throw std::system_error(err, std::generic_category(), "map DMA-BUF");
  }
#endif

  // Hosts without a DMA heap use shared memory.
  *is_dma_buf = false;
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
  this->addr = malloc_with_fd(this->mmap_len, &this->fd, &this->is_dma_buf);
  this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
  if (this->is_dma_buf) {
    try {
      begin_cpu_access(true);
      memset(this->addr, 0, this->mmap_len);
      end_cpu_access(true);
    } catch (...) {
      free();
      throw;
    }
  }
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

int VisionBuf::sync(int) {
  // DMA-BUF CPU access is bracketed at the actual memory access sites.
  return 0;
}

void VisionBuf::begin_cpu_access(bool write) {
#ifndef __APPLE__
  if (is_dma_buf) {
    struct dma_buf_sync access = {};
    access.flags = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;
    if (ioctl_retry(fd, DMA_BUF_IOCTL_SYNC, &access) < 0) {
      throw std::system_error(errno, std::generic_category(), "begin DMA-BUF CPU access");
    }
  }
#endif
}

void VisionBuf::end_cpu_access(bool write) {
#ifndef __APPLE__
  if (is_dma_buf) {
    struct dma_buf_sync access = {};
    access.flags = DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
    if (ioctl_retry(fd, DMA_BUF_IOCTL_SYNC, &access) < 0) {
      throw std::system_error(errno, std::generic_category(), "end DMA-BUF CPU access");
    }
  }
#endif
}

int VisionBuf::free() {
  int err = munmap(this->addr, this->mmap_len);
  if (err != 0) return err;

  err = close(this->fd);
  return err;
}

uint64_t VisionBuf::get_frame_id() {
  begin_cpu_access();
  uint64_t id = *frame_id;
  end_cpu_access();
  return id;
}

void VisionBuf::set_frame_id(uint64_t id) {
  begin_cpu_access(true);
  *frame_id = id;
  end_cpu_access(true);
}
