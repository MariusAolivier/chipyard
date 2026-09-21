#include <stdint.h>
#include <stdio.h>

#include "dory_dma.h"
#include "ne16_driver.h"
#include "pulp_nnx.h"

static uint8_t source[64];
static uint8_t result[64];

static int check_region(const uint8_t *actual, size_t offset, size_t length,
                        size_t source_offset) {
  for (size_t i = 0; i < length; ++i) {
    if (actual[offset + i] != source[source_offset + i]) {
      printf("DMA mismatch at %zu: actual=%u expected=%u\n", offset + i,
             actual[offset + i], source[source_offset + i]);
      return 1;
    }
  }
  return 0;
}

int main(void) {
  for (size_t i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)(i + 3);
  for (size_t i = 0; i < sizeof(result); ++i) result[i] = 0;

  dory_ne16_compat_reset_stats();
  dma_mutex_init();
  const DmaTransferConf load = {
      .ext = (uint32_t)(uintptr_t)source,
      .loc = NE16_SCRATCH_BASE + 0x200u,
      .stride_2d = 32,
      .number_of_2d_copies = 2,
      .stride_1d = 8,
      .number_of_1d_copies = 3,
      .length_1d_copy = 4,
      .hwc_to_chw = 0,
      .dir = DORY_DMA_DIR_EXT2LOC,
  };
  DmaTransfer transfer = dma_transfer_create();
  dma_transfer_2d_async(load);
  dma_transfer_wait(transfer);
  dma_transfer_free(transfer);
  if (dory_ne16_compat_error() != 0) {
    printf("valid L2-to-L1 DMA failed: %d\n", dory_ne16_compat_error());
    return 1;
  }

  uint8_t scratch[4];
  for (size_t plane = 0; plane < 2; ++plane) {
    for (size_t row = 0; row < 3; ++row) {
      size_t location = 0x200u + plane * 32u + row * 8u;
      size_t source_offset = plane * 32u + row * 8u;
      if (ne16_scratchpad_read(scratch, (uint32_t)location, sizeof(scratch)) != 0) {
        printf("2D L2-to-L1 read failed\n");
        return 1;
      }
      for (size_t i = 0; i < sizeof(scratch); ++i) {
        if (scratch[i] != source[source_offset + i]) {
          printf("2D L2-to-L1 mismatch at %zu\n", source_offset + i);
          return 1;
        }
      }
    }
  }

  dory_ne16_compat_reset_stats();
  const DmaTransferConf store = {
      .ext = (uint32_t)(uintptr_t)result,
      .loc = NE16_SCRATCH_BASE + 0x200u,
      .stride_2d = 32,
      .number_of_2d_copies = 2,
      .stride_1d = 8,
      .number_of_1d_copies = 3,
      .length_1d_copy = 4,
      .hwc_to_chw = 0,
      .dir = DORY_DMA_DIR_LOC2EXT,
  };
  dma_transfer_2d_async(store);
  dma_transfer_wait(transfer);
  if (dory_ne16_compat_error() != 0) {
    printf("valid L1-to-L2 DMA failed: %d\n", dory_ne16_compat_error());
    return 1;
  }
  for (size_t plane = 0; plane < 2; ++plane) {
    for (size_t row = 0; row < 3; ++row) {
      size_t offset = plane * 32u + row * 8u;
      if (check_region(result, offset, 4, offset) != 0) {
        printf("2D L1-to-L2 layout failed\n");
        return 1;
      }
    }
  }

  dory_ne16_compat_reset_stats();
  DmaTransferConf invalid = load;
  invalid.loc = NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 2u;
  invalid.length_1d_copy = 4;
  dma_transfer_2d_async(invalid);
  dma_transfer_wait(transfer);
  if (dory_ne16_compat_error() == 0) {
    printf("out-of-bounds DMA was accepted\n");
    return 1;
  }

  dory_ne16_compat_reset_stats();
  invalid = load;
  invalid.hwc_to_chw = 1;
  dma_transfer_async(invalid);
  dma_transfer_wait(transfer);
  if (dory_ne16_compat_error() == 0) {
    printf("unsupported HWC-to-CHW DMA was accepted\n");
    return 1;
  }
  printf("NE16 DMA PASS\n");
  return 0;
}
