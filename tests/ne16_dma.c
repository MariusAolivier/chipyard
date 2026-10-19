#include "ne16_dma.h"

#include <stddef.h>

#include "ne16_driver.h"

void dory_ne16_compat_set_error(int error);

static uint32_t read32(uint32_t offset) {
  return *(volatile uint32_t *)(uintptr_t)(NE16_DMA_BASE + offset);
}

static void write32(uint32_t offset, uint32_t value) {
  *(volatile uint32_t *)(uintptr_t)(NE16_DMA_BASE + offset) = value;
}

static int span(uint32_t address, uint32_t line, uint32_t rows,
                uint32_t planes, uint32_t row_stride, uint32_t plane_stride,
                uint64_t *end) {
  uint64_t row_span = (uint64_t)(rows - 1u) * row_stride + line;
  if ((rows > 1 && line > row_stride) ||
      (planes > 1 && row_span > plane_stride)) return NE16_DMA_EINVAL;
  uint64_t plane_span = (uint64_t)(planes - 1u) * plane_stride;
  const uint64_t limit = UINT64_C(1) << 32;
  /* Bound each term before adding, including UINT32_MAX counts/strides. */
  if (row_span > limit || plane_span > limit - row_span)
    return NE16_DMA_EADDRESS;
  uint64_t size = plane_span + row_span;
  *end = (uint64_t)address + size;
  return *end <= (UINT64_C(1) << 32) ? 0 : NE16_DMA_EADDRESS;
}

int ne16_dma_validate(const ne16_dma_config_t *c) {
  if (c == NULL || c->line_bytes == 0 || c->rows == 0 || c->planes == 0)
    return NE16_DMA_EINVAL;
  uint64_t src_end, dst_end;
  int result = span(c->src, c->line_bytes, c->rows, c->planes,
                    c->src_row_stride, c->src_plane_stride, &src_end);
  if (result != 0) return result;
  result = span(c->dst, c->line_bytes, c->rows, c->planes,
                c->dst_row_stride, c->dst_plane_stride, &dst_end);
  if (result != 0) return result;
  int src_scratch = c->src >= NE16_SCRATCH_BASE &&
                    src_end <= NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES;
  int dst_scratch = c->dst >= NE16_SCRATCH_BASE &&
                    dst_end <= NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES;
  int src_dram = c->src >= UINT32_C(0x80000000);
  int dst_dram = c->dst >= UINT32_C(0x80000000);
  return (src_scratch && dst_dram) || (dst_scratch && src_dram)
             ? 0 : NE16_DMA_EADDRESS;
}

uint32_t ne16_dma_status(void) { return read32(NE16_DMA_STATUS); }
uint32_t ne16_dma_error(void) { return read32(NE16_DMA_ERROR); }

int ne16_dma_counter_read(uint32_t offset, uint32_t *value) {
  if (value == NULL) return NE16_DMA_EINVAL;
  switch (offset) {
    case NE16_DMA_BYTES_COPIED:
    case NE16_DMA_CPU_GRANTS:
    case NE16_DMA_DMA_GRANTS:
    case NE16_DMA_NE16_GRANTS:
    case NE16_DMA_CONTENTION:
    case NE16_DMA_NE16_OVERLAP:
    case NE16_DMA_BUSY_CYCLES:
    case NE16_DMA_NE16_BUSY_CYCLES:
    case NE16_DMA_BOTH_BUSY_CYCLES:
    case NE16_DMA_COMMAND_CYCLES:
      *value = read32(offset);
      return 0;
    default:
      return NE16_DMA_EINVAL;
  }
}

uint32_t ne16_dma_counter(uint32_t offset) {
  uint32_t value = 0;
  int result = ne16_dma_counter_read(offset, &value);
  if (result != 0) dory_ne16_compat_set_error(result);
  return value;
}

int ne16_dma_ack(void) {
  if (ne16_dma_status() & NE16_DMA_BUSY) return NE16_DMA_EBUSY;
  ne16_memory_fence();
  write32(NE16_DMA_CMD, NE16_DMA_ACK);
  return 0;
}

int ne16_dma_submit(const ne16_dma_config_t *c) {
  int result = ne16_dma_validate(c);
  if (result != 0) return result;
  uint32_t status = ne16_dma_status();
  if (status & NE16_DMA_BUSY) return NE16_DMA_EBUSY;
  if (status & (NE16_DMA_DONE | NE16_DMA_FAILED)) {
    result = ne16_dma_ack();
    if (result != 0) return result;
  }
  write32(NE16_DMA_SRC, c->src);
  write32(NE16_DMA_DST, c->dst);
  write32(NE16_DMA_LINE_BYTES, c->line_bytes);
  write32(NE16_DMA_ROWS, c->rows);
  write32(NE16_DMA_PLANES, c->planes);
  write32(NE16_DMA_SRC_ROW_STRIDE, c->src_row_stride);
  write32(NE16_DMA_DST_ROW_STRIDE, c->dst_row_stride);
  write32(NE16_DMA_SRC_PLANE_STRIDE, c->src_plane_stride);
  write32(NE16_DMA_DST_PLANE_STRIDE, c->dst_plane_stride);
  ne16_memory_fence();
  write32(NE16_DMA_CMD, NE16_DMA_START);
  return 0;
}

int ne16_dma_poll(void) {
  uint32_t status = ne16_dma_status();
  if (status & NE16_DMA_BUSY) return 0;
  ne16_memory_fence();
  if (status & NE16_DMA_FAILED) {
    switch (ne16_dma_error()) {
      case 1: return NE16_DMA_ECONFIG;
      case 2: return NE16_DMA_ETL;
      case 3: return NE16_DMA_EADDRESS;
      default: return NE16_DMA_ESTATE;
    }
  }
  return status & NE16_DMA_DONE ? 1 : NE16_DMA_ESTATE;
}

int ne16_dma_wait(uint32_t max_poll) {
  for (uint32_t i = 0; i < max_poll; ++i) {
    int result = ne16_dma_poll();
    if (result != 0) return result;
  }
  return NE16_DMA_ETIMEOUT;
}
