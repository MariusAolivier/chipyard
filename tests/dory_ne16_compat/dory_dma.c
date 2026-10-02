#include "dory_dma.h"

#include <limits.h>
#include <stddef.h>

#include "ne16_driver.h"
#include "pulp_nnx.h"

typedef struct {
  ne16_dma_config_t config;
  uint64_t sequence;
  int direction;
} queued_dma_t;

typedef struct {
  int id, live, sealed, error;
  uint64_t first, last;
} dma_group_t;

static queued_dma_t queue[DORY_DMA_QUEUE_CAPACITY];
static dma_group_t groups[DORY_DMA_GROUP_CAPACITY];
static unsigned int head, count;
static int current_group = -1;
static int next_id = 1;
static int active;
static uint64_t submitted, completed;

static int fail(int error) {
  dory_ne16_compat_set_error(error);
  return error;
}

static int enqueue_error(int error) {
  if (current_group >= 0 && groups[current_group].live &&
      groups[current_group].error == 0)
    groups[current_group].error = error;
  return fail(error);
}

static dma_group_t *find_group(DmaTransfer transfer) {
  for (unsigned int i = 0; i < DORY_DMA_GROUP_CAPACITY; ++i)
    if (groups[i].live && groups[i].id == transfer.id) return &groups[i];
  return NULL;
}

int dma_transfer_config(DmaTransferConf c, int force_1d,
                        ne16_dma_config_t *config) {
  if (config == NULL || c.length_1d_copy <= 0 || c.hwc_to_chw != 0 ||
      (c.dir != DORY_DMA_DIR_EXT2LOC && c.dir != DORY_DMA_DIR_LOC2EXT) ||
      c.number_of_1d_copies < 0 || c.number_of_2d_copies < 0 ||
      c.stride_1d < 0 || c.stride_2d < 0) return NE16_DMA_EINVAL;
  /* Generated DORY uses zero-initialized counts/strides for 1D buffers. */
  uint32_t rows = force_1d || c.number_of_1d_copies == 0
                      ? 1u : (uint32_t)c.number_of_1d_copies;
  uint32_t planes = force_1d || c.number_of_2d_copies == 0
                        ? 1u : (uint32_t)c.number_of_2d_copies;
  uint32_t length = (uint32_t)c.length_1d_copy;
  uint32_t row_stride = c.stride_1d == 0 ? length : (uint32_t)c.stride_1d;
  uint64_t plane_stride = c.stride_2d == 0
                             ? (uint64_t)row_stride * rows
                             : (uint32_t)c.stride_2d;
  if (plane_stride > UINT32_MAX) return NE16_DMA_EADDRESS;
  *config = (ne16_dma_config_t){
      .src = c.dir == DORY_DMA_DIR_EXT2LOC ? c.ext : c.loc,
      .dst = c.dir == DORY_DMA_DIR_EXT2LOC ? c.loc : c.ext,
      .line_bytes = length, .rows = rows, .planes = planes,
      .src_row_stride = row_stride, .dst_row_stride = row_stride,
      .src_plane_stride = (uint32_t)plane_stride,
      .dst_plane_stride = (uint32_t)plane_stride};
  return ne16_dma_validate(config);
}

static void abort_queue(int error) {
  for (unsigned int i = 0; i < DORY_DMA_GROUP_CAPACITY; ++i)
    if (groups[i].live && groups[i].last >= groups[i].first &&
        groups[i].last > completed && groups[i].error == 0)
      groups[i].error = error;
  count = 0;
  active = 0;
  completed = submitted;
}

int dma_transfer_idle(void) {
  return count == 0 && !active && !(ne16_dma_status() & NE16_DMA_BUSY);
}

int dma_transfer_service(void) {
  if (active) {
    int result = ne16_dma_poll();
    if (result == 0) return 0;
    if (result < 0) {
      abort_queue(result);
      return fail(result);
    }
    completed = queue[head].sequence;
    head = (head + 1u) % DORY_DMA_QUEUE_CAPACITY;
    --count;
    active = 0;
  }
  if (count != 0) {
    int result = ne16_dma_submit(&queue[head].config);
    if (result != 0) {
      /* Another raw owner must drain before this queue may advance. */
      if (result != NE16_DMA_EBUSY) abort_queue(result);
      return fail(result);
    }
    active = 1;
    if (queue[head].direction == DORY_DMA_DIR_EXT2LOC)
      dory_ne16_compat_note_dma_l2_to_l1();
    else
      dory_ne16_compat_note_dma_l1_to_l2();
  }
  return count == 0 ? 1 : 0;
}

int dma_transfer_enqueue(DmaTransferConf conf, int force_1d) {
  if (current_group < 0 || !groups[current_group].live ||
      groups[current_group].sealed) return enqueue_error(DORY_DMA_EHANDLE);
  if (groups[current_group].error != 0)
    return fail(groups[current_group].error);
  ne16_dma_config_t config;
  int result = dma_transfer_config(conf, force_1d, &config);
  if (result != 0) return enqueue_error(result);
  if (count == DORY_DMA_QUEUE_CAPACITY) return enqueue_error(DORY_DMA_EFULL);
  if (submitted == UINT64_MAX) return enqueue_error(DORY_DMA_EFULL);
  queue[(head + count) % DORY_DMA_QUEUE_CAPACITY] = (queued_dma_t){
      .config = config, .sequence = ++submitted, .direction = conf.dir};
  ++count;
  groups[current_group].last = submitted;
  /* Start only: later descriptors are pumped by service/poll/wait. */
  if (!active) {
    result = dma_transfer_service();
    if (result < 0) return enqueue_error(result);
  }
  return 0;
}

void dma_transfer_1d_async(DmaTransferConf c) {
  (void)dma_transfer_enqueue(c, 1);
}
void dma_transfer_2d_async(DmaTransferConf c) {
  (void)dma_transfer_enqueue(c, 0);
}
void dma_transfer_3d_async(DmaTransferConf c) {
  (void)dma_transfer_enqueue(c, 0);
}
void dma_transfer_async(DmaTransferConf c) {
  (void)dma_transfer_enqueue(c, 0);
}

DmaTransfer dma_transfer_create(void) {
  if (current_group >= 0) {
    groups[current_group].sealed = 1;
    current_group = -1;
  }
  for (unsigned int i = 0; i < DORY_DMA_GROUP_CAPACITY; ++i) {
    if (!groups[i].live) {
      if (next_id == INT_MAX) break;
      groups[i] = (dma_group_t){.id = next_id++, .live = 1,
                               .first = submitted + 1u, .last = submitted};
      current_group = (int)i;
      return (DmaTransfer){groups[i].id};
    }
  }
  fail(DORY_DMA_EFULL);
  return (DmaTransfer){0};
}

int dma_transfer_seal(DmaTransfer transfer) {
  dma_group_t *group = find_group(transfer);
  if (group == NULL) return fail(DORY_DMA_EHANDLE);
  group->sealed = 1;
  if (current_group >= 0 && groups[current_group].id == transfer.id)
    current_group = -1;
  return 0;
}

int dma_transfer_poll(DmaTransfer transfer) {
  dma_group_t *group = find_group(transfer);
  if (group == NULL) return fail(DORY_DMA_EHANDLE);
  (void)dma_transfer_seal(transfer);
  /* A completed group's fixed watermark cannot be extended by later groups. */
  if (group->last < group->first || group->last <= completed)
    return group->error != 0 ? fail(group->error) : 1;
  int result = dma_transfer_service();
  if (group->last <= completed)
    return group->error != 0 ? fail(group->error) : 1;
  if (result < 0) return result;
  return 0;
}

int dma_transfer_wait_result(DmaTransfer transfer) {
  for (uint32_t i = 0; i < DORY_NE16_MAX_POLL; ++i) {
    int result = dma_transfer_poll(transfer);
    if (result != 0) return result == 1 ? 0 : fail(result);
  }
  return fail(NE16_DMA_ETIMEOUT);
}

void dma_transfer_wait(DmaTransfer transfer) {
  /* Generated loaders never free: consume only after all admitted DMA ends. */
  (void)dma_transfer_free_result(transfer);
}

int dma_transfer_free_result(DmaTransfer transfer) {
  dma_group_t *group = find_group(transfer);
  if (group == NULL) return fail(DORY_DMA_EHANDLE);
  int result = dma_transfer_wait_result(transfer);
  /* Even on timeout retain the handle and buffers until the HW is drained. */
  if (group->last >= group->first && group->last > completed) return result;
  group->live = 0;
  return result;
}

void dma_transfer_free(DmaTransfer transfer) {
  (void)dma_transfer_free_result(transfer);
}

int dma_transfer_drain(void) {
  if (current_group >= 0)
    (void)dma_transfer_seal((DmaTransfer){groups[current_group].id});
  for (uint32_t i = 0; i < DORY_NE16_MAX_POLL; ++i) {
    int result = dma_transfer_service();
    if (result != 0) return result == 1 ? 0 : result;
  }
  return fail(NE16_DMA_ETIMEOUT);
}

void dma_mutex_init(void) {
  /* Never reset or acknowledge an active hardware engine. */
  if (count != 0 || active || (ne16_dma_status() & NE16_DMA_BUSY)) {
    fail(NE16_DMA_EBUSY);
    return;
  }
  for (unsigned int i = 0; i < DORY_DMA_GROUP_CAPACITY; ++i)
    groups[i].live = 0;
  current_group = -1;
  head = 0;
}
void dma_mutex_lock(void) {}
void dma_mutex_unlock(void) {}
