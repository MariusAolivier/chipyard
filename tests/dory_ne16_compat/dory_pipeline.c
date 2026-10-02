#include "dory_pipeline.h"

#include <stddef.h>

#include "ne16_driver.h"

static int pipeline_error(dory_pipeline_t *p, int error) {
  if (p->error == 0) p->error = error;
  dory_ne16_compat_set_error(error);
  return p->error;
}

static int init_error(int error) {
  dory_ne16_compat_set_error(error);
  return error;
}

static int within(dory_pipeline_memory_t memory, uint32_t address,
                  uint64_t span) {
  return address >= memory.base &&
         (uint64_t)address + span <= (uint64_t)memory.base + memory.bytes;
}

static int valid_transfer(dory_pipeline_memory_t memory, DmaTransferConf c,
                          int direction) {
  ne16_dma_config_t config;
  if (c.dir != direction || dma_transfer_config(c, 0, &config) != 0) return 0;
  uint64_t span = (uint64_t)(config.planes - 1u) * config.src_plane_stride +
                  (uint64_t)(config.rows - 1u) * config.src_row_stride +
                  config.line_bytes;
  return within(memory, c.loc, span);
}

static int valid_job(dory_pipeline_slot_t *slot) {
  dory_pipeline_job_t *job = &slot->job;
  if (job->load_count == 0 || job->load_count > DORY_PIPELINE_MAX_LOADS)
    return 0;
  for (unsigned int i = 0; i < job->load_count; ++i)
    if (!valid_transfer(slot->memory, job->loads[i], DORY_DMA_DIR_EXT2LOC))
      return 0;
  if (!valid_transfer(slot->memory, job->store, DORY_DMA_DIR_LOC2EXT))
    return 0;
  for (unsigned int i = 0; i < 6; ++i) {
    /* Shift and bias/scale are optional depending on descriptor flags. */
    if (i >= 3 && job->task.words[i] == 0) continue;
    if (!within(slot->memory, job->task.words[i], 1)) return 0;
  }
  return 1;
}

int dory_pipeline_init(dory_pipeline_t *p,
                       const dory_pipeline_memory_t memory[DORY_PIPELINE_SLOTS],
                       uint32_t jobs, dory_pipeline_prepare_t prepare,
                       void *context) {
  if (p == NULL || memory == NULL || jobs == 0 || prepare == NULL)
    return init_error(DORY_PIPELINE_EINVAL);
  for (unsigned int i = 0; i < DORY_PIPELINE_SLOTS; ++i) {
    if (memory[i].bytes == 0 || memory[i].base < NE16_SCRATCH_BASE ||
        (uint64_t)memory[i].base + memory[i].bytes >
            NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES)
      return init_error(DORY_PIPELINE_EINVAL);
  }
  if ((uint64_t)memory[0].base < (uint64_t)memory[1].base + memory[1].bytes &&
      (uint64_t)memory[1].base < (uint64_t)memory[0].base + memory[0].bytes)
    return init_error(DORY_PIPELINE_EINVAL);
  if (dory_ne16_compat_error() != 0) return dory_ne16_compat_error();
  if (!dma_transfer_idle() || !ne16_idle() || !nnx_dispatch_check())
    return init_error(NE16_DMA_EBUSY);
  *p = (dory_pipeline_t){.prepare = prepare, .context = context, .jobs = jobs,
                          .computing_slot = -1};
  for (unsigned int i = 0; i < DORY_PIPELINE_SLOTS; ++i)
    p->slots[i].memory = memory[i];
  return 0;
}

static int enqueue_load(dory_pipeline_t *p, unsigned int index) {
  dory_pipeline_slot_t *slot = &p->slots[index];
  slot->job = (dory_pipeline_job_t){0};
  slot->index = p->next_load;
  if (p->prepare(p->context, slot->index, index, slot->memory, &slot->job) != 0)
    return pipeline_error(p, DORY_PIPELINE_EPREPARE);
  if (!valid_job(slot)) return pipeline_error(p, DORY_PIPELINE_EINVAL);
  slot->transfer = dma_transfer_create();
  if (slot->transfer.id == 0)
    return pipeline_error(p, dory_ne16_compat_error());
  slot->state = DORY_SLOT_LOADING;
  for (unsigned int i = 0; i < slot->job.load_count; ++i) {
    int result = dma_transfer_enqueue(slot->job.loads[i], 0);
    if (result != 0) return pipeline_error(p, result);
  }
  (void)dma_transfer_seal(slot->transfer);
  ++p->next_load;
  return 0;
}

int dory_pipeline_poll(dory_pipeline_t *p) {
  if (p == NULL) return DORY_PIPELINE_EINVAL;
  if (p->error != 0) return p->error;
  if (dory_ne16_compat_error() != 0)
    return pipeline_error(p, dory_ne16_compat_error());
  if (p->completed == p->jobs) return 1;
  int progress = 0;
  int result = dma_transfer_service();
  if (result < 0) return pipeline_error(p, result);

  for (unsigned int i = 0; i < DORY_PIPELINE_SLOTS; ++i) {
    dory_pipeline_slot_t *slot = &p->slots[i];
    if (slot->state != DORY_SLOT_LOADING && slot->state != DORY_SLOT_STORING)
      continue;
    result = dma_transfer_poll(slot->transfer);
    if (result < 0) return pipeline_error(p, result);
    if (result == 1) {
      result = dma_transfer_free_result(slot->transfer);
      if (result != 0) return pipeline_error(p, result);
      if (slot->state == DORY_SLOT_LOADING) slot->state = DORY_SLOT_READY;
      else {
        slot->state = DORY_SLOT_FREE;
        ++p->completed;
      }
      progress = 1;
    }
  }

  if (p->computing_slot >= 0) {
    dory_pipeline_slot_t *slot = &p->slots[p->computing_slot];
    if (nnx_resolve_check(&slot->job.task)) {
      ne16_memory_fence();
      slot->state = DORY_SLOT_STORE_PENDING;
      p->computing_slot = -1;
      progress = 1;
    }
    if (dory_ne16_compat_error() != 0)
      return pipeline_error(p, dory_ne16_compat_error());
  }

  /* Dispatch before enqueueing stores/next loads so those DMA grants can
   * overlap this job. Only one NNX task is owned at a time. */
  for (unsigned int i = 0; i < DORY_PIPELINE_SLOTS; ++i) {
    dory_pipeline_slot_t *slot = &p->slots[i];
    if (p->computing_slot < 0 && slot->state == DORY_SLOT_READY &&
        slot->index == p->next_compute) {
      nnx_dispatch_task(&slot->job.task);
      if (dory_ne16_compat_error() != 0)
        return pipeline_error(p, dory_ne16_compat_error());
      p->computing_slot = (int)i;
      slot->state = DORY_SLOT_COMPUTING;
      ++p->next_compute;
      progress = 1;
    }
  }

  for (unsigned int i = 0; i < DORY_PIPELINE_SLOTS; ++i) {
    dory_pipeline_slot_t *slot = &p->slots[i];
    /* Unless this is the tail, keep the previous result resident until the
     * next compute is dispatched, then start its store concurrently. */
    if (slot->state == DORY_SLOT_STORE_PENDING &&
        (p->computing_slot >= 0 || p->next_compute == p->jobs)) {
      slot->transfer = dma_transfer_create();
      if (slot->transfer.id == 0)
        return pipeline_error(p, dory_ne16_compat_error());
      slot->state = DORY_SLOT_STORING;
      result = dma_transfer_enqueue(slot->job.store, 0);
      if (result != 0) return pipeline_error(p, result);
      (void)dma_transfer_seal(slot->transfer);
      progress = 1;
    }
  }

  /* Only bootstrap one job. Subsequent loads begin while another computes,
   * rather than preloading both slots before the first dispatch. */
  if (p->next_load < p->jobs &&
      (p->next_load == 0 || p->computing_slot >= 0 ||
       p->next_compute == p->next_load)) {
    for (unsigned int i = 0; i < DORY_PIPELINE_SLOTS; ++i) {
      if (p->slots[i].state == DORY_SLOT_FREE) {
        result = enqueue_load(p, i);
        if (result != 0) return result;
        progress = 1;
        break;
      }
    }
  }
  if (progress) p->stalled_polls = 0;
  else if (++p->stalled_polls >= DORY_NE16_MAX_POLL)
    return pipeline_error(p, DORY_PIPELINE_ETIMEOUT);
  return p->completed == p->jobs ? 1 : 0;
}

int dory_pipeline_run(dory_pipeline_t *p) {
  int result;
  do { result = dory_pipeline_poll(p); } while (result == 0);
  return result == 1 ? 0 : result;
}

int dory_pipeline_drain(dory_pipeline_t *p) {
  if (p == NULL) return DORY_PIPELINE_EINVAL;
  if (p->error == 0 && p->completed < p->jobs)
    (void)pipeline_error(p, DORY_PIPELINE_EDRAINED);
  int result = dma_transfer_drain();
  if (!dma_transfer_idle()) return result != 0 ? result : NE16_DMA_EBUSY;
  /* Resolve without NNX's sticky-error short circuit. */
  if (ne16_wait_idle(DORY_NE16_MAX_POLL) != 0) return NE16_DMA_ETIMEOUT;
  int error = result;
  for (unsigned int i = 0; i < DORY_PIPELINE_SLOTS; ++i) {
    if (p->slots[i].state == DORY_SLOT_LOADING ||
        p->slots[i].state == DORY_SLOT_STORING) {
      result = dma_transfer_free_result(p->slots[i].transfer);
      if (error == 0 && result != 0) error = result;
    }
    p->slots[i].state = DORY_SLOT_FREE;
  }
  p->computing_slot = -1;
  return error;
}
