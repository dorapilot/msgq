#include "msgq/test_runner.h"
#include "msgq/visionipc/visionbuf.h"

#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <vector>

struct Access {
  uint64_t flags;
  uint64_t value;
};

static std::vector<Access> accesses;
static size_t observed_offset;
static int sync_error;
static bool allocation_error;

extern "C" int __real_open(const char *path, int flags, ...);

extern "C" int __wrap_open(const char *path, int flags, ...) {
  if (strcmp(path, "/dev/dma_heap/system") == 0) return __real_open("/dev/null", O_RDONLY);
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }
  return __real_open(path, flags, mode);
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list args;
  va_start(args, request);
  void *arg = va_arg(args, void *);
  va_end(args);
  if (request == DMA_HEAP_IOCTL_ALLOC) {
    if (allocation_error) {
      errno = EIO;
      return -1;
    }
    auto *allocation = static_cast<dma_heap_allocation_data *>(arg);
    allocation->fd = memfd_create("visionbuf-test", MFD_CLOEXEC);
    if (ftruncate(allocation->fd, allocation->len) != 0) return -1;
    std::vector<uint8_t> dirty(allocation->len, 0xa5);
    return pwrite(allocation->fd, dirty.data(), dirty.size(), 0) == static_cast<ssize_t>(dirty.size()) ? 0 : -1;
  }
  if (request == DMA_BUF_IOCTL_SYNC) {
    auto *access = static_cast<dma_buf_sync *>(arg);
    uint64_t value = 0;
    if (pread(fd, &value, sizeof(value), observed_offset) != sizeof(value)) return -1;
    accesses.push_back({access->flags, value});
    if (sync_error) {
      errno = sync_error;
      sync_error = 0;
      return -1;
    }
    return 0;
  }
  errno = ENOTTY;
  return -1;
}

TEST_CASE("DMA-BUF initialization is inside CPU write access") {
  accesses.clear();
  observed_offset = 0;
  VisionBuf buf;
  buf.allocate(64);
  auto recorded = accesses;
  buf.free();
  REQUIRE(recorded.size() == 2);
  REQUIRE(recorded[0].flags == DMA_BUF_SYNC_WRITE);
  REQUIRE(recorded[0].value == 0xa5a5a5a5a5a5a5a5);
  REQUIRE(recorded[1].flags == (DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_END));
  REQUIRE(recorded[1].value == 0);
}

TEST_CASE("DMA-BUF frame ids are read and written inside CPU access") {
  VisionBuf buf;
  buf.allocate(64);
  observed_offset = buf.len;
  accesses.clear();
  buf.set_frame_id(42);
  uint64_t frame_id = buf.get_frame_id();
  auto recorded = accesses;
  buf.free();
  REQUIRE(frame_id == 42);
  REQUIRE(recorded.size() == 4);
  REQUIRE(recorded[0].flags == DMA_BUF_SYNC_WRITE);
  REQUIRE(recorded[0].value == 0);
  REQUIRE(recorded[1].flags == (DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_END));
  REQUIRE(recorded[1].value == 42);
  REQUIRE(recorded[2].flags == DMA_BUF_SYNC_READ);
  REQUIRE(recorded[3].flags == (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END));
}

TEST_CASE("DMA-BUF synchronization retries interruptions and reports failures") {
  VisionBuf buf;
  buf.allocate(64);
  observed_offset = buf.len;
  accesses.clear();
  sync_error = EINTR;
  buf.set_frame_id(73);
  auto recorded = accesses;
  sync_error = EIO;
  bool threw = false;
  try {
    buf.get_frame_id();
  } catch (const std::system_error &error) {
    threw = error.code().value() == EIO;
  }
  sync_error = 0;
  buf.free();
  REQUIRE(recorded.size() == 3);
  REQUIRE(recorded[0].flags == DMA_BUF_SYNC_WRITE);
  REQUIRE(recorded[1].flags == DMA_BUF_SYNC_WRITE);
  REQUIRE(recorded[2].flags == (DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_END));
  REQUIRE(threw);
}

TEST_CASE("DMA heap failures do not silently return non-importable shared memory") {
  allocation_error = true;
  bool threw = false;
  VisionBuf buf;
  try {
    buf.allocate(64);
    buf.free();
  } catch (const std::system_error &error) {
    threw = error.code().value() == EIO;
  }
  allocation_error = false;
  REQUIRE(threw);
}
