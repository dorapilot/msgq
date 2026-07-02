#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/ion.h>

#include <linux/msm_ion.h>

#include "msgq/visionipc/visionbuf.h"

// keep trying if x gets interrupted by a signal
#define HANDLE_EINTR(x)                                       \
  ({                                                          \
    decltype(x) ret;                                          \
    int try_cnt = 0;                                          \
    do {                                                      \
      ret = (x);                                              \
    } while (ret == -1 && errno == EINTR && try_cnt++ < 100); \
    ret;                                                      \
  })

// ---- DMA-BUF heap fallback (mainline kernels without /dev/ion) ----
// local UAPI definitions so this builds against old sysroot headers
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
#define VB_DMA_BUF_SYNC_RW        (VB_DMA_BUF_SYNC_READ | VB_DMA_BUF_SYNC_WRITE)
#define VB_DMA_BUF_SYNC_START     (0 << 2)
#define VB_DMA_BUF_SYNC_END       (1 << 2)
#define VB_DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct vb_dma_buf_sync)

static bool use_dma_heap() {
  static int cached = -1;
  if (cached == -1) {
    cached = (access("/dev/ion", F_OK) != 0 &&
              access("/dev/dma_heap/system", F_OK) == 0) ? 1 : 0;
  }
  return cached == 1;
}

static int dma_heap_fd() {
  static int fd = -2;
  if (fd == -2) {
    fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
  }
  return fd;
}
// -------------------------------------------------------------------

struct IonFileHandle {
  IonFileHandle() {
    fd = open("/dev/ion", O_RDWR | O_NONBLOCK);
    assert(fd >= 0);
  }
  ~IonFileHandle() {
    close(fd);
  }
  int fd = -1;
};

int ion_fd() {
  static IonFileHandle fh;
  return fh.fd;
}

void VisionBuf::allocate(size_t length) {
  size_t alloc_len = length + sizeof(uint64_t);

  if (use_dma_heap()) {
    struct vb_dma_heap_allocation_data alloc = {0};
    alloc.len = alloc_len;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;

    int err = HANDLE_EINTR(ioctl(dma_heap_fd(), VB_DMA_HEAP_IOCTL_ALLOC, &alloc));
    assert(err == 0);

    void *mmap_addr = mmap(NULL, alloc_len,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, alloc.fd, 0);
    assert(mmap_addr != MAP_FAILED);

    memset(mmap_addr, 0, alloc_len);

    this->len = length;
    this->mmap_len = alloc_len;
    this->addr = mmap_addr;
    this->handle = 0;
    this->fd = alloc.fd;
    this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
    return;
  }

  struct ion_allocation_data ion_alloc = {0};
  ion_alloc.len = alloc_len;
  ion_alloc.align = 4096;
  ion_alloc.heap_id_mask = 1 << ION_IOMMU_HEAP_ID;
  ion_alloc.flags = ION_FLAG_CACHED;

  int err = HANDLE_EINTR(ioctl(ion_fd(), ION_IOC_ALLOC, &ion_alloc));
  assert(err == 0);

  struct ion_fd_data ion_fd_data = {0};
  ion_fd_data.handle = ion_alloc.handle;
  err = HANDLE_EINTR(ioctl(ion_fd(), ION_IOC_SHARE, &ion_fd_data));
  assert(err == 0);

  void *mmap_addr = mmap(NULL, ion_alloc.len,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED, ion_fd_data.fd, 0);
  assert(mmap_addr != MAP_FAILED);

  memset(mmap_addr, 0, ion_alloc.len);

  this->len = length;
  this->mmap_len = ion_alloc.len;
  this->addr = mmap_addr;
  this->handle = ion_alloc.handle;
  this->fd = ion_fd_data.fd;
  this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
}

void VisionBuf::import(){
  int err;
  assert(this->fd >= 0);

  if (!use_dma_heap()) {
    // Get handle
    struct ion_fd_data fd_data = {0};
    fd_data.fd = this->fd;
    err = HANDLE_EINTR(ioctl(ion_fd(), ION_IOC_IMPORT, &fd_data));
    assert(err == 0);
    this->handle = fd_data.handle;
  } else {
    this->handle = 0;
  }

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
  assert(dir == VISIONBUF_SYNC_FROM_DEVICE || dir == VISIONBUF_SYNC_TO_DEVICE);

  if (use_dma_heap()) {
    // Bracket a CPU access on the dmabuf: START invalidates for reads,
    // END writes back for the device.
    struct vb_dma_buf_sync sync_args = {0};
    if (dir == VISIONBUF_SYNC_FROM_DEVICE) {
      sync_args.flags = VB_DMA_BUF_SYNC_START | VB_DMA_BUF_SYNC_READ;
    } else {
      sync_args.flags = VB_DMA_BUF_SYNC_END | VB_DMA_BUF_SYNC_WRITE;
    }
    return HANDLE_EINTR(ioctl(this->fd, VB_DMA_BUF_IOCTL_SYNC, &sync_args));
  }

  struct ion_flush_data flush_data = {0};
  flush_data.handle = this->handle;
  flush_data.vaddr = this->addr;
  flush_data.offset = 0;
  flush_data.length = this->len;

  // ION_IOC_INV_CACHES ~= DMA_FROM_DEVICE
  // ION_IOC_CLEAN_CACHES ~= DMA_TO_DEVICE
  // ION_IOC_CLEAN_INV_CACHES ~= DMA_BIDIRECTIONAL

  struct ion_custom_data custom_data = {0};

  custom_data.cmd = (dir == VISIONBUF_SYNC_FROM_DEVICE) ?
    ION_IOC_INV_CACHES : ION_IOC_CLEAN_CACHES;

  custom_data.arg = (unsigned long)&flush_data;
  return HANDLE_EINTR(ioctl(ion_fd(), ION_IOC_CUSTOM, &custom_data));
}

int VisionBuf::free() {
  int err = munmap(this->addr, this->mmap_len);
  if (err != 0) return err;

  err = close(this->fd);
  if (err != 0) return err;

  if (use_dma_heap()) return 0;

  struct ion_handle_data handle_data = {.handle = this->handle};
  return HANDLE_EINTR(ioctl(ion_fd(), ION_IOC_FREE, &handle_data));
}

uint64_t VisionBuf::get_frame_id() {
  return *frame_id;
}

void VisionBuf::set_frame_id(uint64_t id) {
  *frame_id = id;
}
