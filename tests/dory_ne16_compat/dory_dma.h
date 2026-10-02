#ifndef CHIPYARD_DORY_DMA_H
#define CHIPYARD_DORY_DMA_H

#include <stdint.h>
#include "pmsis.h"
#include "ne16_dma.h"

#define DORY_DMA_DIR_LOC2EXT 0
#define DORY_DMA_DIR_EXT2LOC 1
#define DORY_DMA_QUEUE_CAPACITY 16u
#define DORY_DMA_GROUP_CAPACITY 16u
#define DORY_DMA_EFULL (-40)
#define DORY_DMA_EHANDLE (-41)

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

/* Single Rocket-core cooperative queue, no CPU-copy backend. Counts/strides
 * equal to zero mean the generated DORY 1D defaults; negatives are invalid.
 * Legacy DmaTransferConf retains the identical endpoint row/plane layout;
 * native enqueue_config supports independently packed endpoint strides.
 * create selects a group; enqueue/async append to that group. A new create,
 * seal, poll or wait freezes its watermark. Poll waits for ALL its configs,
 * never for later groups. Queue-full is explicit backpressure, not a retry.
 * Enqueue errors latch the selected group's first error; no further configs
 * are admitted to that group. Already admitted DMA still runs: poll/wait/free
 * pump it to completion before returning the group's error.
 * Stats count once per logical config actually submitted to hardware.
 * Buffers must remain live until completion; free drains before invalidating.
 * Timeout retains handle/engine/buffers. Drain/poll can recover later.
 * wait_result retains its handle; free_result releases a drained handle even
 * on group error. Legacy void wait consumes the drained handle (no extra free),
 * matching generated loaders that never free. Timeout retains it for recovery.
 * init only discards idle handles, NEVER resets hardware or clears sticky
 * compat errors. Legacy mutex operations are no-ops, not multicore locks.
 * Service/poll/wait pump progress (there is no interrupt or hardware queue). */
int dma_transfer_config(DmaTransferConf conf, int force_1d,
                        ne16_dma_config_t *config);
int dma_transfer_enqueue(DmaTransferConf conf, int force_1d);
/* Native descriptors support independently packed local/external layouts,
 * with the same group ownership, watermarks and error handling as enqueue. */
int dma_transfer_enqueue_config(const ne16_dma_config_t *config);
int dma_transfer_seal(DmaTransfer transfer);
int dma_transfer_service(void);
int dma_transfer_idle(void);
int dma_transfer_poll(DmaTransfer transfer);
int dma_transfer_wait_result(DmaTransfer transfer);
int dma_transfer_free_result(DmaTransfer transfer);
int dma_transfer_drain(void);
void dma_transfer_1d_async(DmaTransferConf conf);
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
