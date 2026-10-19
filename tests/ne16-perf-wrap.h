#ifndef NE16_PERF_WRAP_H
#define NE16_PERF_WRAP_H

#include <stdint.h>

/* Cycle accounting for the NE16 pipeline. Filled by ne16-perf-wrap.c, which
 * wraps nnx_dispatch_task and nnx_resolve_check at link time with --wrap.
 * busy_cycles runs from dispatch return to the first poll that observes
 * completion, so it includes poll latency.
 *
 * cpu_lag_upper_cycles: per task, the gap between the last check that saw the
 * NE16 still busy and the check that saw it done. The true completion lies in
 * that gap, so the sum is an upper bound on the delay the CPU added.
 * dma_wait_cycles: per task, idle time from detection to the next dispatch
 * when a poll boundary lies between them, i.e. the next job's data was not
 * ready. poll_seq is advanced by the test before each dory_pipeline_poll. */
typedef struct {
  uint64_t dispatch_cycles, busy_cycles;
  uint64_t cpu_lag_upper_cycles, dma_wait_cycles;
  uint32_t tasks, resolve_calls, dma_wait_events;
  uint32_t poll_seq;
} ne16_perf_t;

extern ne16_perf_t ne16_perf;

/* Bracket the layer's prepare callback so the report can split the time from
 * a freed slot to the next load being sealed. */
void ne16_perf_prepare_enter(uint32_t index);
void ne16_perf_prepare_exit(uint32_t index);

/* Per hardware descriptor, from wrapping ne16_dma_submit/ne16_dma_poll.
 * hw_* is the engine's own busy count (COMMAND_CYCLES); observed_* runs from
 * submit to the poll that sees completion. The engine is idle between one
 * descriptor's estimated hardware finish and the next submit. That gap is
 * split into starve (the next descriptor was not enqueued yet) and pump (it
 * was enqueued, but the CPU had not submitted it). */
typedef struct {
  uint64_t hw_load_cycles, hw_store_cycles;
  uint64_t observed_load_cycles, observed_store_cycles;
  uint64_t starve_cycles, pump_cycles;
  uint32_t loads, stores, gaps;
} ne16_perf_dma_t;

extern ne16_perf_dma_t ne16_perf_dma;

/* Prints the per-transfer latencies, load handoff timing and the load
 * enqueue chain (PERF3 and PERF4 lines). */
void ne16_perf_report(void);

/* Prints the per-descriptor hardware/idle split (PERF6 line). */
void ne16_perf_dma_report(void);

static inline uint64_t ne16_perf_cycles(void) {
  uint64_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
}

#endif
