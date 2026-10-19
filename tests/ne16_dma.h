#ifndef NE16_DMA_H
#define NE16_DMA_H

#include <stdint.h>

#define NE16_DMA_BASE 0x10031000UL
#define NE16_DMA_CMD 0x00u
#define NE16_DMA_STATUS 0x04u
#define NE16_DMA_SRC 0x08u
#define NE16_DMA_DST 0x0cu
#define NE16_DMA_LINE_BYTES 0x10u
#define NE16_DMA_ROWS 0x14u
#define NE16_DMA_PLANES 0x18u
#define NE16_DMA_SRC_ROW_STRIDE 0x1cu
#define NE16_DMA_DST_ROW_STRIDE 0x20u
#define NE16_DMA_SRC_PLANE_STRIDE 0x24u
#define NE16_DMA_DST_PLANE_STRIDE 0x28u
#define NE16_DMA_ERROR 0x2cu
#define NE16_DMA_BYTES_COPIED 0x30u
#define NE16_DMA_CPU_GRANTS 0x38u
#define NE16_DMA_DMA_GRANTS 0x3cu
#define NE16_DMA_NE16_GRANTS 0x40u
#define NE16_DMA_CONTENTION 0x44u
#define NE16_DMA_NE16_OVERLAP 0x48u
#define NE16_DMA_BUSY_CYCLES 0x4cu
#define NE16_DMA_NE16_BUSY_CYCLES 0x50u
#define NE16_DMA_BOTH_BUSY_CYCLES 0x54u
#define NE16_DMA_COMMAND_CYCLES 0x58u

#define NE16_DMA_START 1u
#define NE16_DMA_ACK 2u
#define NE16_DMA_BUSY 1u
#define NE16_DMA_DONE 2u
#define NE16_DMA_FAILED 4u

enum {
  NE16_DMA_EINVAL = -100,
  NE16_DMA_ECONFIG = -101,
  NE16_DMA_ETL = -102,
  NE16_DMA_EADDRESS = -103,
  NE16_DMA_EBUSY = -104,
  NE16_DMA_ETIMEOUT = -105,
  NE16_DMA_ESTATE = -106
};

typedef struct {
  uint32_t src, dst, line_bytes, rows, planes;
  uint32_t src_row_stride, dst_row_stride;
  uint32_t src_plane_stride, dst_plane_stride;
} ne16_dma_config_t;

/* One Rocket-core owner; do not mix raw MMIO with the DORY queue while live.
 * Exactly one scratch endpoint and one 32-bit DRAM endpoint are required.
 * Validation checks full strided spans; negotiated TL legality is checked by HW.
 * No reset or software-copy fallback. Busy submit/ack never write MMIO.
 * Fences publish source data before START and acquire destination after DONE.
 * Timeout leaves the engine live: retain buffers and poll/drain before reuse. */
int ne16_dma_validate(const ne16_dma_config_t *config);
int ne16_dma_submit(const ne16_dma_config_t *config);
uint32_t ne16_dma_status(void);
uint32_t ne16_dma_error(void);
/* Checked read: invalid offset/null output returns EINVAL without MMIO; output
 * is unchanged on failure. The convenience wrapper preserves valid call sites
 * and reports invalid offsets through the sticky DORY compat error. */
int ne16_dma_counter_read(uint32_t offset, uint32_t *value);
uint32_t ne16_dma_counter(uint32_t offset);
/* Nonblocking: 0 busy, 1 completed, negative explicit error (idle is ESTATE).
 * Poll does not ACK; completion/error remains readable until ack/next submit. */
int ne16_dma_poll(void);
int ne16_dma_wait(uint32_t max_poll);
int ne16_dma_ack(void);

#endif
