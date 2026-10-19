#include "ne16-perf-wrap.h"
#include "dory_dma.h"
#include "ne16_driver.h"
#include "pulp_nnx.h"

#include <stdio.h>

#define PERF_MAX 64

ne16_perf_t ne16_perf;

static uint64_t busy_start;
static uint64_t last_check;
static uint64_t detect_time;
static uint32_t detect_seq;
static int in_flight;
static int idle_pending;

static uint64_t task_dispatch[PERF_MAX];
static uint64_t task_detect[PERF_MAX];

/* Transfers are attributed by their first enqueued config: a load writes into
 * the scratchpad, a store writes to DRAM. Loads and stores are sealed in job
 * order, so index j is job j. */
static int pending_kind_set;
static int pending_is_load;
static uint32_t load_n, store_n;
static int load_id[PERF_MAX], store_id[PERF_MAX];
static uint64_t load_seal[PERF_MAX], load_done[PERF_MAX];
static uint64_t store_seal[PERF_MAX], store_done[PERF_MAX];
static int load_done_set[PERF_MAX], store_done_set[PERF_MAX];

void __real_nnx_dispatch_task(nnx_task_t *task);
int __real_nnx_resolve_check(nnx_task_t *task);
int __real_dma_transfer_enqueue_config(const ne16_dma_config_t *config);
int __real_dma_transfer_seal(DmaTransfer transfer);
int __real_dma_transfer_poll(DmaTransfer transfer);
int __real_ne16_dma_submit(const ne16_dma_config_t *config);
int __real_ne16_dma_poll(void);

ne16_perf_dma_t ne16_perf_dma;

/* Enqueue times of descriptors not yet submitted; the software queue is
 * FIFO, so each submit takes the oldest. */
#define ENQUEUED_MAX 32
static uint64_t enqueued[ENQUEUED_MAX];
static uint32_t enqueued_head, enqueued_count;
static uint64_t submit_end, hw_done;
static int descriptor_active, descriptor_is_load, descriptor_seen;

void __wrap_nnx_dispatch_task(nnx_task_t *task) {
  uint64_t start = ne16_perf_cycles();
  __real_nnx_dispatch_task(task);
  uint64_t end = ne16_perf_cycles();
  ne16_perf.dispatch_cycles += end - start;
  if (ne16_perf.tasks < PERF_MAX) task_dispatch[ne16_perf.tasks] = start;
  ++ne16_perf.tasks;
  /* A dispatch in a later poll than the detection means the NE16 was idle
   * waiting for this job's data, not for the CPU. */
  if (idle_pending && ne16_perf.poll_seq != detect_seq) {
    ne16_perf.dma_wait_cycles += start - detect_time;
    ++ne16_perf.dma_wait_events;
  }
  idle_pending = 0;
  busy_start = end;
  last_check = end;
  in_flight = 1;
}

int __wrap_nnx_resolve_check(nnx_task_t *task) {
  ++ne16_perf.resolve_calls;
  uint64_t before = ne16_perf_cycles();
  int done = __real_nnx_resolve_check(task);
  if (in_flight) {
    if (done) {
      uint64_t now = ne16_perf_cycles();
      ne16_perf.busy_cycles += now - busy_start;
      ne16_perf.cpu_lag_upper_cycles += before - last_check;
      detect_time = now;
      detect_seq = ne16_perf.poll_seq;
      idle_pending = 1;
      in_flight = 0;
      uint32_t job = ne16_perf.tasks - 1;
      if (job < PERF_MAX) task_detect[job] = now;
    } else {
      last_check = before;
    }
  }
  return done;
}

int __wrap_dma_transfer_enqueue_config(const ne16_dma_config_t *config) {
  if (!pending_kind_set) {
    pending_is_load = config->dst >= NE16_SCRATCH_BASE &&
                      config->dst < NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES;
    pending_kind_set = 1;
  }
  /* Recorded first: the queue may submit it before this call returns. */
  if (enqueued_count < ENQUEUED_MAX)
    enqueued[(enqueued_head + enqueued_count++) % ENQUEUED_MAX] =
        ne16_perf_cycles();
  return __real_dma_transfer_enqueue_config(config);
}

int __wrap_dma_transfer_seal(DmaTransfer transfer) {
  uint64_t seal = ne16_perf_cycles();
  int result = __real_dma_transfer_seal(transfer);
  if (pending_is_load && load_n < PERF_MAX) {
    load_id[load_n] = transfer.id;
    load_seal[load_n++] = seal;
  } else if (!pending_is_load && store_n < PERF_MAX) {
    store_id[store_n] = transfer.id;
    store_seal[store_n++] = seal;
  }
  pending_kind_set = 0;
  return result;
}

int __wrap_dma_transfer_poll(DmaTransfer transfer) {
  int result = __real_dma_transfer_poll(transfer);
  if (result == 1) {
    uint64_t now = ne16_perf_cycles();
    for (uint32_t i = 0; i < load_n; ++i)
      if (load_id[i] == transfer.id && !load_done_set[i]) {
        load_done[i] = now;
        load_done_set[i] = 1;
      }
    for (uint32_t i = 0; i < store_n; ++i)
      if (store_id[i] == transfer.id && !store_done_set[i]) {
        store_done[i] = now;
        store_done_set[i] = 1;
      }
  }
  return result;
}

int __wrap_ne16_dma_submit(const ne16_dma_config_t *config) {
  uint64_t start = ne16_perf_cycles();
  int result = __real_ne16_dma_submit(config);
  if (result != 0) return result;
  uint64_t enqueue = start;
  if (enqueued_count != 0) {
    enqueue = enqueued[enqueued_head];
    enqueued_head = (enqueued_head + 1u) % ENQUEUED_MAX;
    --enqueued_count;
  }
  if (descriptor_seen && start > hw_done) {
    ++ne16_perf_dma.gaps;
    if (enqueue > hw_done) {
      ne16_perf_dma.starve_cycles += enqueue - hw_done;
      ne16_perf_dma.pump_cycles += start - enqueue;
    } else {
      ne16_perf_dma.pump_cycles += start - hw_done;
    }
  }
  submit_end = ne16_perf_cycles();
  descriptor_active = 1;
  descriptor_is_load = config->dst >= NE16_SCRATCH_BASE &&
                       config->dst < NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES;
  return 0;
}

int __wrap_ne16_dma_poll(void) {
  int result = __real_ne16_dma_poll();
  if (result != 0 && descriptor_active) {
    uint64_t now = ne16_perf_cycles();
    uint32_t hw = ne16_dma_counter(NE16_DMA_COMMAND_CYCLES);
    if (descriptor_is_load) {
      ne16_perf_dma.hw_load_cycles += hw;
      ne16_perf_dma.observed_load_cycles += now - submit_end;
      ++ne16_perf_dma.loads;
    } else {
      ne16_perf_dma.hw_store_cycles += hw;
      ne16_perf_dma.observed_store_cycles += now - submit_end;
      ++ne16_perf_dma.stores;
    }
    hw_done = submit_end + hw;
    descriptor_active = 0;
    descriptor_seen = 1;
  }
  return result;
}

void ne16_perf_dma_report(void) {
  const ne16_perf_dma_t *d = &ne16_perf_dma;
  printf("PERF6 descriptors_load=%u hw_load_mean=%u observed_load_mean=%u ",
         d->loads, d->loads ? (uint32_t)(d->hw_load_cycles / d->loads) : 0,
         d->loads ? (uint32_t)(d->observed_load_cycles / d->loads) : 0);
  printf("descriptors_store=%u hw_store_mean=%u observed_store_mean=%u ",
         d->stores, d->stores ? (uint32_t)(d->hw_store_cycles / d->stores) : 0,
         d->stores ? (uint32_t)(d->observed_store_cycles / d->stores) : 0);
  printf("hw_load_sum=%u hw_store_sum=%u gaps=%u starve_sum=%u pump_sum=%u\n",
         (uint32_t)d->hw_load_cycles, (uint32_t)d->hw_store_cycles, d->gaps,
         (uint32_t)d->starve_cycles, (uint32_t)d->pump_cycles);
}

/* The test's prepare callback brackets the layer's prepare with these. The
 * pipeline calls prepare first thing in enqueue_load, so prep_enter marks the
 * start of a load enqueue and prep_exit the end of building its descriptors. */
static uint64_t prep_enter[PERF_MAX], prep_exit[PERF_MAX];

void ne16_perf_prepare_enter(uint32_t index) {
  if (index < PERF_MAX) prep_enter[index] = ne16_perf_cycles();
}

void ne16_perf_prepare_exit(uint32_t index) {
  if (index < PERF_MAX) prep_exit[index] = ne16_perf_cycles();
}

/* Prints the per-transfer and per-handoff results. Values are 32-bit because
 * this firmware's printf does not handle the 64-bit length modifier. */
void ne16_perf_report(void) {
  uint32_t load_lat_sum = 0, load_lat_max = 0, loads = 0;
  uint32_t store_lat_sum = 0, store_lat_max = 0, stores = 0;
  for (uint32_t i = 0; i < load_n; ++i)
    if (load_done_set[i]) {
      uint32_t lat = (uint32_t)(load_done[i] - load_seal[i]);
      load_lat_sum += lat;
      if (lat > load_lat_max) load_lat_max = lat;
      ++loads;
    }
  for (uint32_t i = 0; i < store_n; ++i)
    if (store_done_set[i]) {
      uint32_t lat = (uint32_t)(store_done[i] - store_seal[i]);
      store_lat_sum += lat;
      if (lat > store_lat_max) store_lat_max = lat;
      ++stores;
    }
  /* Handoff j -> j+1: load j+1 is sealed before or after job j is dispatched,
   * and is done before or after job j's completion was detected. */
  uint32_t issued_late = 0, issued_late_sum = 0;
  uint32_t not_ready = 0, not_ready_sum = 0, handoffs = 0;
  uint32_t lead_sum = 0, lead_count = 0;
  for (uint32_t j = 0; j + 1 < load_n && j + 1 < PERF_MAX; ++j) {
    if (ne16_perf.tasks <= j + 1) break;
    ++handoffs;
    if (load_seal[j + 1] > task_dispatch[j]) {
      ++issued_late;
      issued_late_sum += (uint32_t)(load_seal[j + 1] - task_dispatch[j]);
    } else {
      lead_sum += (uint32_t)(task_dispatch[j] - load_seal[j + 1]);
      ++lead_count;
    }
    if (load_done_set[j + 1] && load_done[j + 1] > task_detect[j]) {
      ++not_ready;
      not_ready_sum += (uint32_t)(load_done[j + 1] - task_detect[j]);
    }
  }
  printf("PERF3 loads=%u load_lat_mean=%u load_lat_max=%u ", loads,
         loads ? load_lat_sum / loads : 0, load_lat_max);
  printf("stores=%u store_lat_mean=%u store_lat_max=%u ", stores,
         stores ? store_lat_sum / stores : 0, store_lat_max);
  printf("handoffs=%u next_load_sealed_after_dispatch=%u next_load_sealed_after_dispatch_sum=%u ",
         handoffs, issued_late, issued_late_sum);
  printf("next_load_lead_mean=%u next_load_lead_count=%u ",
         lead_count ? lead_sum / lead_count : 0, lead_count);
  printf("next_load_done_after_detect=%u next_load_done_after_detect_sum=%u\n",
         not_ready, not_ready_sum);

  /* Chain for load k, which reuses the slot of job k-2:
   *   dispatch(k-1) -> store(k-2) sealed -> store(k-2) done -> prepare(k)
   *   starts -> prepare(k) ends -> load(k) sealed.
   * Each interval is summed over k = 2 .. n-1; negative intervals are counted
   * and clamped to zero. */
  uint32_t n = 0, negative = 0;
  uint32_t store_start = 0, store_lat = 0, free_to_enqueue = 0;
  uint32_t prepare = 0, enqueue = 0, total = 0;
  for (uint32_t k = 2; k < load_n && k < store_n + 2 && k < PERF_MAX; ++k) {
    if (ne16_perf.tasks <= k - 1 || !store_done_set[k - 2]) break;
    int64_t a = (int64_t)store_seal[k - 2] - (int64_t)task_dispatch[k - 1];
    int64_t b = (int64_t)store_done[k - 2] - (int64_t)store_seal[k - 2];
    int64_t c = (int64_t)prep_enter[k] - (int64_t)store_done[k - 2];
    int64_t d = (int64_t)prep_exit[k] - (int64_t)prep_enter[k];
    int64_t e = (int64_t)load_seal[k] - (int64_t)prep_exit[k];
    int64_t t = (int64_t)load_seal[k] - (int64_t)task_dispatch[k - 1];
    if (a < 0 || b < 0 || c < 0 || d < 0 || e < 0) {
      ++negative;
      if (a < 0) a = 0;
      if (b < 0) b = 0;
      if (c < 0) c = 0;
      if (d < 0) d = 0;
      if (e < 0) e = 0;
    }
    if (t < 0) t = 0;
    store_start += (uint32_t)a;
    store_lat += (uint32_t)b;
    free_to_enqueue += (uint32_t)c;
    prepare += (uint32_t)d;
    enqueue += (uint32_t)e;
    total += (uint32_t)t;
    ++n;
  }
  printf("PERF4 chain_n=%u negative_intervals=%u ", n, negative);
  printf("dispatch_to_store_seal_mean=%u store_latency_mean=%u ",
         n ? store_start / n : 0, n ? store_lat / n : 0);
  printf("store_done_to_prepare_mean=%u prepare_mean=%u ",
         n ? free_to_enqueue / n : 0, n ? prepare / n : 0);
  printf("prepare_end_to_seal_mean=%u dispatch_to_load_seal_mean=%u\n",
         n ? enqueue / n : 0, n ? total / n : 0);
}
