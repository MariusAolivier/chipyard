#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dory_dma.h"
#include "ne16_dma.h"
#include "ne16_driver.h"
#include "pulp_nnx.h"

#define SENTINEL 0xa5u
#define SCRATCH_OFFSET 0x200u
#define REGION_BYTES 512u

static uint8_t source[REGION_BYTES] __attribute__((aligned(64)));
static uint8_t result[REGION_BYTES] __attribute__((aligned(64)));
static uint8_t expected[REGION_BYTES];
static uint8_t observed[REGION_BYTES];

static int pointer32(const void *p, uint32_t *address) {
  uintptr_t value = (uintptr_t)p;
  if (value < UINT32_C(0x80000000) || value > UINT32_MAX) return -1;
  *address = (uint32_t)value;
  return 0;
}

static int compare(const uint8_t *actual, const uint8_t *want,
                   const char *label) {
  for (size_t i = 0; i < REGION_BYTES; ++i) {
    if (actual[i] != want[i]) {
      printf("%s mismatch at %zu: %u expected %u\n", label, i,
             actual[i], want[i]);
      return -1;
    }
  }
  return 0;
}

static void expected_layout(uint32_t offset, uint32_t length, uint32_t rows,
                            uint32_t planes, uint32_t row_stride,
                            uint32_t plane_stride, uint32_t source_offset,
                            uint32_t source_row, uint32_t source_plane) {
  for (uint32_t p = 0; p < planes; ++p)
    for (uint32_t r = 0; r < rows; ++r)
      memcpy(expected + offset + p * plane_stride + r * row_stride,
             source + source_offset + p * source_plane + r * source_row,
             length);
}

static int direct_geometry(uint32_t ext, uint32_t out, uint32_t src_offset,
                            uint32_t dst_offset, uint32_t length,
                            uint32_t rows, uint32_t planes,
                            uint32_t src_row, uint32_t dst_row,
                            uint32_t src_plane, uint32_t dst_plane) {
  memset(observed, SENTINEL, sizeof(observed));
  memset(result, SENTINEL, sizeof(result));
  memset(expected, SENTINEL, sizeof(expected));
  if (ne16_scratchpad_write(SCRATCH_OFFSET, observed, sizeof(observed)) != 0)
    return -1;
  ne16_dma_config_t load = {
      .src = ext + src_offset,
      .dst = NE16_SCRATCH_BASE + SCRATCH_OFFSET + dst_offset,
      .line_bytes = length, .rows = rows, .planes = planes,
      .src_row_stride = src_row, .dst_row_stride = dst_row,
      .src_plane_stride = src_plane, .dst_plane_stride = dst_plane};
  if (ne16_dma_submit(&load) != 0 ||
      ne16_dma_wait(0) != NE16_DMA_ETIMEOUT ||
      ne16_dma_wait(DORY_NE16_MAX_POLL) != 1) {
    printf("direct load failed status=%u error=%u\n",
           ne16_dma_status(), ne16_dma_error());
    return -1;
  }
  expected_layout(dst_offset, length, rows, planes, dst_row, dst_plane,
                  src_offset, src_row, src_plane);
  if (ne16_scratchpad_read(observed, SCRATCH_OFFSET, sizeof(observed)) != 0 ||
      compare(observed, expected, "scratch data/holes/guards") != 0) return -1;

  ne16_dma_config_t store = {
      .src = load.dst, .dst = out + src_offset,
      .line_bytes = length, .rows = rows, .planes = planes,
      .src_row_stride = dst_row, .dst_row_stride = src_row,
      .src_plane_stride = dst_plane, .dst_plane_stride = src_plane};
  if (ne16_dma_submit(&store) != 0 ||
      ne16_dma_wait(DORY_NE16_MAX_POLL) != 1) return -1;
  memset(expected, SENTINEL, sizeof(expected));
  expected_layout(src_offset, length, rows, planes, src_row, src_plane,
                  src_offset, src_row, src_plane);
  return compare(result, expected, "DRAM data/holes/guards");
}

static void raw32(uint32_t offset, uint32_t value) {
  *(volatile uint32_t *)(uintptr_t)(NE16_DMA_BASE + offset) = value;
}

/* Bypass software validation, proving hardware rejection touches no bytes. */
static int reject_hardware(uint32_t ext, uint32_t dst, uint32_t bytes,
                           uint32_t rows, uint32_t planes, uint32_t row_stride,
                           uint32_t plane_stride) {
  if (ne16_dma_ack() != 0) return -1;
  raw32(NE16_DMA_SRC, ext);
  raw32(NE16_DMA_DST, dst);
  raw32(NE16_DMA_LINE_BYTES, bytes);
  raw32(NE16_DMA_ROWS, rows);
  raw32(NE16_DMA_PLANES, planes);
  raw32(NE16_DMA_SRC_ROW_STRIDE, row_stride);
  raw32(NE16_DMA_DST_ROW_STRIDE, row_stride);
  raw32(NE16_DMA_SRC_PLANE_STRIDE, plane_stride);
  raw32(NE16_DMA_DST_PLANE_STRIDE, plane_stride);
  uint32_t grants = ne16_dma_counter(NE16_DMA_DMA_GRANTS);
  ne16_memory_fence();
  raw32(NE16_DMA_CMD, NE16_DMA_START);
  int status = ne16_dma_wait(DORY_NE16_MAX_POLL);
  if (status != NE16_DMA_ECONFIG || ne16_dma_error() != 1 ||
      ne16_dma_counter(NE16_DMA_DMA_GRANTS) != grants) {
    printf("HW rejection failed result=%d error=%u\n", status, ne16_dma_error());
    return -1;
  }
  return 0;
}

static int validation_tests(uint32_t ext) {
  ne16_dma_config_t c = {
      .src = ext, .dst = NE16_SCRATCH_BASE + SCRATCH_OFFSET,
      .line_bytes = 4, .rows = 3, .planes = 2,
      .src_row_stride = 8, .dst_row_stride = 8,
      .src_plane_stride = 32, .dst_plane_stride = 32};
  ne16_dma_config_t invalid = c;
  invalid.dst = NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 2u;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c; invalid.src = UINT32_MAX - 1u;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c; invalid.rows = 0;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c; invalid.line_bytes = 0;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c; invalid.dst_row_stride = 3;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c; invalid.dst_plane_stride = 19;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c; invalid.src = c.dst;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c; invalid.dst = ext;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c; invalid.src = 0x10030000u;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  invalid = c;
  invalid.rows = UINT32_MAX; invalid.planes = UINT32_MAX;
  invalid.src_row_stride = UINT32_MAX;
  invalid.src_plane_stride = UINT32_MAX;
  if (ne16_dma_validate(&invalid) >= 0) return -1;
  uint32_t grants = ne16_dma_counter(NE16_DMA_DMA_GRANTS);
  if (ne16_dma_submit(&invalid) >= 0 ||
      ne16_dma_counter(NE16_DMA_DMA_GRANTS) != grants) return -1;
  /* End exactly at 2^32 is legal in the address-domain validator only. */
  invalid = c; invalid.rows = 1; invalid.planes = 1;
  invalid.src = UINT32_MAX - 3u;
  if (ne16_dma_validate(&invalid) != 0) return -1;

  memset(observed, SENTINEL, sizeof(observed));
  if (ne16_scratchpad_write(SCRATCH_OFFSET, observed, sizeof(observed)) != 0)
    return -1;
  const uint32_t loc = NE16_SCRATCH_BASE + SCRATCH_OFFSET;
  if (reject_hardware(ext, loc, 0, 1, 1, 4, 4) != 0 ||
      reject_hardware(ext, loc, 4, 0, 1, 4, 4) != 0 ||
      reject_hardware(ext, loc, 4, 1, 0, 4, 4) != 0 ||
      reject_hardware(ext, loc, 4, 2, 1, 3, 8) != 0 ||
      reject_hardware(ext, loc, 4, 3, 2, 8, 19) != 0 ||
      reject_hardware(ext, NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 2u,
                      4, 1, 1, 4, 4) != 0) return -1;
  if (ne16_scratchpad_read(observed, SCRATCH_OFFSET, sizeof(observed)) != 0)
    return -1;
  memset(expected, SENTINEL, sizeof(expected));
  return compare(observed, expected, "HW rejected scratch guards");
}

static int dory_queue_tests(uint32_t ext, uint32_t out) {
  dory_ne16_compat_reset_stats();
  dma_mutex_init();
  DmaTransferConf load = {
      .ext = ext, .loc = NE16_SCRATCH_BASE + SCRATCH_OFFSET,
      .stride_2d = 32, .number_of_2d_copies = 2,
      .stride_1d = 8, .number_of_1d_copies = 3,
      .length_1d_copy = 4, .dir = DORY_DMA_DIR_EXT2LOC};
  memset(observed, SENTINEL, sizeof(observed));
  if (ne16_scratchpad_write(SCRATCH_OFFSET, observed, sizeof(observed)) != 0)
    return -1;
  DmaTransfer first = dma_transfer_create();
  dma_transfer_2d_async(load);
  DmaTransferConf second = load;
  second.ext += 65; second.loc += 129;
  dma_transfer_3d_async(second);
  if (dma_transfer_seal(first) != 0) return -1;
  /* Later group must not change first's fixed completion watermark. */
  DmaTransfer later = dma_transfer_create();
  DmaTransferConf store = load;
  store.ext = out; store.dir = DORY_DMA_DIR_LOC2EXT;
  dma_transfer_async(store);
  if (dma_transfer_seal(later) != 0) return -1;
  int status;
  uint32_t polls = 0;
  do {
    status = dma_transfer_poll(first);
    if (++polls >= DORY_NE16_MAX_POLL) return -1;
  } while (status == 0);
  if (status != 1) return -1;
  memset(expected, SENTINEL, sizeof(expected));
  expected_layout(0, 4, 3, 2, 8, 32, 0, 8, 32);
  expected_layout(129, 4, 3, 2, 8, 32, 65, 8, 32);
  if (ne16_scratchpad_read(observed, SCRATCH_OFFSET, sizeof(observed)) != 0 ||
      compare(observed, expected, "group watermark (two logical configs)") != 0 ||
      dma_transfer_free_result(first) != 0 ||
      dma_transfer_wait_result(later) != 0 ||
      dma_transfer_free_result(later) != 0) return -1;
  memset(expected, SENTINEL, sizeof(expected));
  expected_layout(0, 4, 3, 2, 8, 32, 0, 8, 32);
  if (compare(result, expected, "DORY store") != 0) return -1;
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  if (stats.dma_l2_to_l1_count != 2 || stats.dma_l1_to_l2_count != 1)
    return -1;

  /* Freed handles fail deterministically, never reused for another group. */
  if (dma_transfer_poll(first) != DORY_DMA_EHANDLE) return -1;
  dory_ne16_compat_reset_stats();
  DmaTransfer bad = dma_transfer_create();
  DmaTransferConf invalid = load;
  invalid.loc = NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 2u;
  dma_transfer_async(invalid);
  if (dory_ne16_compat_error() == 0 ||
      dma_transfer_wait_result(bad) != NE16_DMA_EADDRESS) return -1;
  stats = dory_ne16_compat_stats();
  if (stats.dma_l2_to_l1_count != 0) return -1;
  if (dma_transfer_free_result(bad) != NE16_DMA_EADDRESS) return -1;
  dory_ne16_compat_reset_stats();
  bad = dma_transfer_create();
  invalid = load; invalid.hwc_to_chw = 1;
  dma_transfer_async(invalid);
  if (dory_ne16_compat_error() == 0 ||
      dma_transfer_wait_result(bad) != NE16_DMA_EINVAL ||
      dma_transfer_free_result(bad) != NE16_DMA_EINVAL) return -1;

  dory_ne16_compat_reset_stats();
  DmaTransfer full = dma_transfer_create();
  for (unsigned int i = 0; i < DORY_DMA_QUEUE_CAPACITY; ++i)
    if (dma_transfer_enqueue(load, 0) != 0) return -1;
  if (dma_transfer_enqueue(load, 0) != DORY_DMA_EFULL ||
      dma_transfer_wait_result(full) != DORY_DMA_EFULL ||
      !dma_transfer_idle() ||
      dma_transfer_free_result(full) != DORY_DMA_EFULL) return -1;
  stats = dory_ne16_compat_stats();
  if (stats.dma_l2_to_l1_count != DORY_DMA_QUEUE_CAPACITY) return -1;
  dory_ne16_compat_reset_stats();
  DmaTransfer retained = dma_transfer_create();
  if (dma_transfer_enqueue(load, 0) != 0) return -1;
  dma_mutex_init();
  if (dory_ne16_compat_error() != NE16_DMA_EBUSY ||
      dma_transfer_wait_result(retained) != 0 ||
      dma_transfer_free_result(retained) != 0) return -1;
  stats = dory_ne16_compat_stats();
  if (stats.dma_l2_to_l1_count != 1) return -1;
  dory_ne16_compat_reset_stats();

  /* A failed async command must not conceal or cancel earlier admitted work. */
  bad = dma_transfer_create();
  if (dma_transfer_enqueue(load, 0) != 0) return -1;
  invalid = load; invalid.hwc_to_chw = 1;
  dma_transfer_async(invalid);
  if (dma_transfer_wait_result(bad) != NE16_DMA_EINVAL ||
      !dma_transfer_idle() ||
      dma_transfer_free_result(bad) != NE16_DMA_EINVAL) return -1;
  stats = dory_ne16_compat_stats();
  if (stats.dma_l2_to_l1_count != 1) return -1;
  dory_ne16_compat_reset_stats();

  for (unsigned int i = 0; i < DORY_DMA_GROUP_CAPACITY * 3u; ++i) {
    DmaTransfer legacy = dma_transfer_create();
    if (legacy.id == 0) return -1;
    dma_transfer_1d_async(load);
    dma_transfer_wait(legacy);
    if (dory_ne16_compat_error() != 0 || !dma_transfer_idle()) return -1;
  }
  stats = dory_ne16_compat_stats();
  if (stats.dma_l2_to_l1_count != DORY_DMA_GROUP_CAPACITY * 3u) return -1;
  dory_ne16_compat_reset_stats();
  return dma_transfer_drain();
}

int main(void) {
  uint32_t ext, out;
  if (pointer32(source, &ext) != 0 || pointer32(result, &out) != 0) {
    printf("DMA test buffers must be 32-bit external DRAM addresses\n");
    return 1;
  }
  for (size_t i = 0; i < sizeof(source); ++i)
    source[i] = (uint8_t)(i * 29u + 3u);
  uint32_t counter = UINT32_C(0xdeadbeef);
  if (ne16_dma_counter_read(NE16_DMA_CMD, &counter) != NE16_DMA_EINVAL ||
      counter != UINT32_C(0xdeadbeef) ||
      ne16_dma_counter_read(NE16_DMA_DMA_GRANTS, NULL) != NE16_DMA_EINVAL)
    return 1;
  dory_ne16_compat_reset_stats();
  (void)ne16_dma_counter(NE16_DMA_CMD);
  if (dory_ne16_compat_error() != NE16_DMA_EINVAL) return 1;
  dory_ne16_compat_reset_stats();
  if (direct_geometry(ext, out, 16, 32, 128, 1, 1, 128, 128, 128, 128) != 0 ||
      direct_geometry(ext, out, 1, 3, 37, 1, 1, 37, 37, 37, 37) != 0 ||
      direct_geometry(ext, out, 2, 5, 7, 4, 1, 17, 19, 80, 80) != 0 ||
      direct_geometry(ext, out, 3, 7, 5, 3, 3, 11, 13, 41, 47) != 0 ||
      /* Full bursts, mixed head/burst/tail rows, misaligned scratch. */
      direct_geometry(ext, out, 0, 0, 256, 1, 1, 256, 256, 256, 256) != 0 ||
      direct_geometry(ext, out, 8, 8, 200, 2, 1, 216, 208, 432, 416) != 0 ||
      direct_geometry(ext, out, 0, 4, 96, 1, 1, 96, 96, 96, 96) != 0 ||
      validation_tests(ext) != 0) return 1;
  memset(result, SENTINEL, sizeof(result));
  if (dory_queue_tests(ext, out) != 0) {
    printf("DORY async queue failed: %d\n", dory_ne16_compat_error());
    return 1;
  }
  if (ne16_dma_ack() != 0) return 1;
  printf("NE16 DMA PASS: hardware 1D/2D/3D, byte masks, guards, queue watermarks\n");
  return 0;
}
