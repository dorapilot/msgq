#include "msgq/visionipc/visionbuf.h"

#include <atomic>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

std::atomic<int> offset = 0;

// ---- DMA-BUF heap allocator (Linux mainline; needed so device drivers
// can import these buffers as dmabufs). Falls back to shm files. ----
struct vb_dma_heap_allocation_data {
  uint64_t len;
  uint32_t fd;
  uint32_t fd_flags;
  uint64_t heap_flags;
};
#define VB_DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct vb_dma_heap_allocation_data)

struct vb_dma_buf_sync {
  uint64_t flags;
};
#define VB_DMA_BUF_SYNC_READ      (1 << 0)
#define VB_DMA_BUF_SYNC_WRITE     (2 << 0)
#define VB_DMA_BUF_SYNC_START     (0 << 2)
#define VB_DMA_BUF_SYNC_END       (1 << 2)
#define VB_DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct vb_dma_buf_sync)

static int dma_heap_fd() {
  static int fd = -2;
  if (fd == -2) {
    fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
  }
  return fd;
}

static bool use_dma_heap() {
  return dma_heap_fd() >= 0;
}
// --------------------------------------------------------------------

static void *malloc_with_fd(size_t len, int *fd) {
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

  if (use_dma_heap()) {
    struct vb_dma_heap_allocation_data alloc = {};
    alloc.len = this->mmap_len;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;

    int err = ioctl(dma_heap_fd(), VB_DMA_HEAP_IOCTL_ALLOC, &alloc);
    assert(err == 0);

    this->fd = alloc.fd;
    this->addr = mmap(NULL, this->mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, this->fd, 0);
    assert(this->addr != MAP_FAILED);
    memset(this->addr, 0, this->mmap_len);
  } else {
    this->addr = malloc_with_fd(this->mmap_len, &this->fd);
  }

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
  if (use_dma_heap()) {
    // bracket CPU access on the dmabuf: START invalidates for CPU reads,
    // END cleans for device reads
    struct vb_dma_buf_sync sync_args = {};
    if (dir == VISIONBUF_SYNC_FROM_DEVICE) {
      sync_args.flags = VB_DMA_BUF_SYNC_START | VB_DMA_BUF_SYNC_READ;
    } else {
      sync_args.flags = VB_DMA_BUF_SYNC_END | VB_DMA_BUF_SYNC_WRITE;
    }
    return ioctl(this->fd, VB_DMA_BUF_IOCTL_SYNC, &sync_args);
  }
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
