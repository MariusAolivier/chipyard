#ifndef CHIPYARD_DORY_PIPELINE_H
#define CHIPYARD_DORY_PIPELINE_H

#include "dory_dma.h"
#include "pulp_nnx.h"

#define DORY_PIPELINE_SLOTS 2u
#define DORY_PIPELINE_MAX_LOADS 4u
#define DORY_PIPELINE_EINVAL (-50)
#define DORY_PIPELINE_EPREPARE (-51)
#define DORY_PIPELINE_ETIMEOUT (-52)
#define DORY_PIPELINE_EDRAINED (-53)

typedef struct {
  uint32_t base, bytes;
} dory_pipeline_memory_t;

typedef struct {
  nnx_task_t task;
  ne16_dma_config_t loads[DORY_PIPELINE_MAX_LOADS];
  unsigned int load_count;
  ne16_dma_config_t store;
} dory_pipeline_job_t;

typedef int (*dory_pipeline_prepare_t)(void *context, uint32_t job_index,
                                      unsigned int slot_index,
                                      dory_pipeline_memory_t memory,
                                      dory_pipeline_job_t *job);

typedef enum {
  DORY_SLOT_FREE, DORY_SLOT_LOADING, DORY_SLOT_READY,
  DORY_SLOT_COMPUTING, DORY_SLOT_STORE_PENDING, DORY_SLOT_STORING
} dory_pipeline_state_t;

typedef struct {
  dory_pipeline_memory_t memory;
  dory_pipeline_job_t job;
  DmaTransfer transfer;
  dory_pipeline_state_t state;
  uint32_t index;
} dory_pipeline_slot_t;

typedef struct {
  dory_pipeline_slot_t slots[DORY_PIPELINE_SLOTS];
  dory_pipeline_prepare_t prepare;
  void *context;
  uint32_t jobs, next_load, next_compute, completed, stalled_polls;
  int computing_slot, error;
} dory_pipeline_t;

/* One core/one pipeline owns NNX + DMA. Caller initializes NNX and DMA first.
 * Two disjoint scratch allocations INCLUDE all padding/read-ahead guards.
 * prepare only builds a descriptor/configs (no MMIO or queue operations).
 * DMA configs are native descriptors with independent endpoint strides.
 * All local DMA spans and task pointers must belong to the supplied slot;
 * caller guarantees the entire NE16 access footprint incl wide padding reads
 * fits it, with nonoverlapping live input/output/weights/norm regions.
 * Loads are queued AFTER dispatching another ready job to obtain overlap.
 * Compute and stores remain in job order; a slot becomes free only after its
 * output DMA completes. No scratch is allocated or initialized by this API.
 * poll: 0 ongoing, 1 all stores finished, negative sticky error.
 * A failure/timeout DOES NOT release live buffers: drain before reusing them.
 * drain completes admitted DMA/compute (not unsubmitted jobs/stores), retaining
 * the original sticky error. There is no cancellation/reset of active engines.
 * drain is terminal (do not resume); reinitialize NNX/compat state before
 * another pipeline. An unfinished drained pipeline returns EDRAINED on poll.
 * Caller must keep this struct, context, L2 buffers and both slots alive. */
int dory_pipeline_init(dory_pipeline_t *pipeline,
                       const dory_pipeline_memory_t slots[DORY_PIPELINE_SLOTS],
                       uint32_t jobs, dory_pipeline_prepare_t prepare,
                       void *context);
int dory_pipeline_poll(dory_pipeline_t *pipeline);
int dory_pipeline_run(dory_pipeline_t *pipeline);
int dory_pipeline_drain(dory_pipeline_t *pipeline);

#endif
