#ifndef CHIPYARD_DORY_DMA_H
#define CHIPYARD_DORY_DMA_H

#include <stdint.h>
#include "pmsis.h"

#define DORY_DMA_DIR_LOC2EXT 0
#define DORY_DMA_DIR_EXT2LOC 1

typedef struct DmaTransferConf {
  uint32_t ext;
  uint32_t loc;
  int stride_2d;
  int number_of_2d_copies;
  int stride_1d;
  int number_of_1d_copies;
  int length_1d_copy;
  int hwc_to_chw;
  int dir;
} DmaTransferConf;

typedef struct DmaTransfer {
  int id;
} DmaTransfer;

void dma_transfer_1d_async(DmaTransferConf conf);
void dma_transfer_1d_async_ptr(const DmaTransferConf *conf);
void dma_transfer_2d_async(DmaTransferConf conf);
void dma_transfer_3d_async(DmaTransferConf conf);
void dma_transfer_async(DmaTransferConf conf);
DmaTransfer dma_transfer_create(void);
void dma_transfer_free(DmaTransfer transfer);
void dma_transfer_wait(DmaTransfer transfer);
void dma_mutex_init(void);
void dma_mutex_lock(void);
void dma_mutex_unlock(void);

#endif
