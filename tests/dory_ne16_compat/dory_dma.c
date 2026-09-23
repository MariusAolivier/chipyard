#include "dory_dma.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "ne16_driver.h"
#include "pulp_nnx.h"

static int pending_error;
static int next_transfer_id;

static void remember_error(int error) {
  if (pending_error == 0) pending_error = error;
}

static int scratch_offset(uint32_t address, size_t length, uint32_t *offset) {
  if (address < NE16_SCRATCH_BASE ||
      address - NE16_SCRATCH_BASE > NE16_SCRATCH_BYTES ||
      length > NE16_SCRATCH_BYTES - (address - NE16_SCRATCH_BASE)) {
    return -1;
  }
  *offset = address - NE16_SCRATCH_BASE;
  return 0;
}

static int transfer(DmaTransferConf conf, int force_1d) {
  printf("DORY DMA begin dir=%d ext=0x%x loc=0x%x len=%d\n", conf.dir,
         conf.ext, conf.loc, conf.length_1d_copy);
  fflush(stdout);
  if (conf.ext == 0 || conf.loc == 0 || conf.length_1d_copy <= 0 ||
      (conf.dir != DORY_DMA_DIR_LOC2EXT && conf.dir != DORY_DMA_DIR_EXT2LOC) ||
      conf.hwc_to_chw != 0) {
    return -1;
  }
  const int copies_2d = force_1d ? 1 :
      (conf.number_of_2d_copies > 0 ? conf.number_of_2d_copies : 1);
  const int copies_1d = force_1d ? 1 :
      (conf.number_of_1d_copies > 0 ? conf.number_of_1d_copies : 1);
  const size_t length = (size_t)conf.length_1d_copy;
  const size_t stride_1d = conf.stride_1d > 0 ? (size_t)conf.stride_1d : length;
  const size_t stride_2d = conf.stride_2d > 0
                               ? (size_t)conf.stride_2d
                               : stride_1d * (size_t)copies_1d;
  const size_t contiguous_length =
      length <= SIZE_MAX / (size_t)copies_1d
          ? length * (size_t)copies_1d
          : SIZE_MAX;
  const int can_batch = copies_1d > 1 && stride_1d == length &&
                        contiguous_length <= stride_2d &&
                        contiguous_length <= (size_t)INT_MAX;
  if (can_batch) {
    for (int plane = 0; plane < copies_2d; ++plane) {
      uintptr_t ext = (uintptr_t)conf.ext + (size_t)plane * stride_2d;
      uint32_t loc = conf.loc + (uint32_t)((size_t)plane * stride_2d);
      if (ext > UINT32_MAX) return -1;
      uint32_t offset;
      if (scratch_offset(loc, contiguous_length, &offset) != 0) return -1;
      int result = conf.dir == DORY_DMA_DIR_EXT2LOC
                       ? ne16_scratchpad_write(offset, (const void *)ext,
                                               contiguous_length)
                       : ne16_scratchpad_read((void *)ext, offset,
                                              contiguous_length);
      if (result != 0) return result;
    }
  } else {
    for (int plane = 0; plane < copies_2d; ++plane) {
      for (int row = 0; row < copies_1d; ++row) {
        uintptr_t ext = (uintptr_t)conf.ext + (size_t)plane * stride_2d +
                        (size_t)row * stride_1d;
        uint32_t loc = conf.loc + (uint32_t)((size_t)plane * stride_2d +
                                              (size_t)row * stride_1d);
        if (ext > UINT32_MAX) return -1;
        uint32_t offset;
        if (scratch_offset(loc, length, &offset) != 0) return -1;
        int result = conf.dir == DORY_DMA_DIR_EXT2LOC
                         ? ne16_scratchpad_write(offset, (const void *)ext,
                                                 length)
                         : ne16_scratchpad_read((void *)ext, offset, length);
        if (result != 0) return result;
      }
    }
  }
  if (conf.dir == DORY_DMA_DIR_EXT2LOC) {
    dory_ne16_compat_note_dma_l2_to_l1();
  } else {
    dory_ne16_compat_note_dma_l1_to_l2();
  }
  printf("DORY DMA end dir=%d\n", conf.dir);
  fflush(stdout);
  printf("DORY DMA transfer before return\n");
  fflush(stdout);
  return 0;
}

void dma_transfer_1d_async(DmaTransferConf conf) {
  printf("DORY DMA wrapper begin\n");
  fflush(stdout);
  printf("DORY DMA wrapper args ext=0x%x loc=0x%x len=%d dir=%d\n",
         conf.ext, conf.loc, conf.length_1d_copy, conf.dir);
  fflush(stdout);
  int result = transfer(conf, 1);
  printf("DORY DMA async returned result=%d\n", result);
  fflush(stdout);
  if (result != 0) remember_error(-40);
}
void dma_transfer_2d_async(DmaTransferConf conf) {
  if (transfer(conf, 0) != 0) remember_error(-41);
}
void dma_transfer_3d_async(DmaTransferConf conf) {
  if (transfer(conf, 0) != 0) remember_error(-42);
}
void dma_transfer_async(DmaTransferConf conf) {
  if (transfer(conf, 0) != 0) remember_error(-43);
}

DmaTransfer dma_transfer_create(void) {
  return (DmaTransfer){.id = next_transfer_id++};
}
void dma_transfer_free(DmaTransfer transfer) { (void)transfer; }
void dma_transfer_wait(DmaTransfer transfer) {
  (void)transfer;
  if (pending_error != 0) dory_ne16_compat_set_error(pending_error);
}
void dma_mutex_init(void) { pending_error = 0; }
void dma_mutex_lock(void) {}
void dma_mutex_unlock(void) {}
