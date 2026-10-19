#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dory_generated_layer.h"
#include "dory_tiled_layer.h"
#include "ne16_test_scratch.h"
#include "ne16-perf-wrap.h"

#define SENTINEL 0xa5u

_Static_assert(DORY_GENERATED_INPUT_BYTES + DORY_GENERATED_WEIGHTS_BYTES +
                   DORY_GENERATED_OUTPUT_BYTES > NE16_SCRATCH_BYTES,
               "generated fixture must require multiple tiles");
_Static_assert(DORY_GENERATED_OUTPUT_BYTES % 16u == 0,
               "coverage is tracked in complete channel groups");

static struct {
  uint8_t prefix[64], data[DORY_GENERATED_INPUT_BYTES], suffix[64];
} input __attribute__((aligned(64)));
static struct {
  uint8_t prefix[64], data[DORY_GENERATED_WEIGHTS_BYTES], suffix[64];
} weights __attribute__((aligned(64)));
static struct {
  uint8_t prefix[64], data[DORY_GENERATED_OUTPUT_BYTES], suffix[64];
} output __attribute__((aligned(64)));
static uint8_t scratch_expected[NE16_SCRATCH_BYTES];
static uint8_t coverage[DORY_GENERATED_OUTPUT_BYTES / 16u];
static uint8_t slot_of_job[DORY_GENERATED_JOBS];
static uint32_t prepared, uses[DORY_PIPELINE_SLOTS];
static dory_generated_layer_t layer;
static dory_pipeline_t pipeline;

static uint32_t checksum(const uint8_t *bytes, uint32_t count) {
  uint32_t hash = UINT32_C(2166136261);
  for (uint32_t i = 0; i < count; ++i)
    hash = (hash ^ bytes[i]) * UINT32_C(16777619);
  return hash;
}

static int pointer32(const void *pointer, uint32_t *address) {
  uintptr_t value = (uintptr_t)pointer;
  if (value < UINT32_C(0x80000000) || value > UINT32_MAX) return -1;
  *address = (uint32_t)value;
  return 0;
}

static int prepare(void *context, uint32_t index, unsigned int slot,
                   dory_pipeline_memory_t memory, dory_pipeline_job_t *job) {
  if (index != prepared) return DORY_PIPELINE_EPREPARE;
  ne16_perf_prepare_enter(index);
  int status = dory_generated_layer_prepare(context, index, slot, memory, job);
  ne16_perf_prepare_exit(index);
  if (status != 0) return status;
  slot_of_job[index] = (uint8_t)slot;
  ++prepared;
  ++uses[slot];
  return 0;
}

static int replay(uint32_t index, dory_pipeline_memory_t memory) {
  dory_pipeline_job_t job;
  int status = dory_generated_layer_prepare(&layer, index, slot_of_job[index],
                                           memory, &job);
  if (status != 0) return status;
  for (unsigned int load = 0; load < job.load_count; ++load) {
    const ne16_dma_config_t *config = &job.loads[load];
    for (uint32_t plane = 0; plane < config->planes; ++plane)
      for (uint32_t row = 0; row < config->rows; ++row) {
        uint32_t local = config->dst + plane * config->dst_plane_stride +
                         row * config->dst_row_stride - NE16_SCRATCH_BASE;
        uint32_t source = config->src + plane * config->src_plane_stride +
                          row * config->src_row_stride;
        if ((uint64_t)local + config->line_bytes > NE16_SCRATCH_BYTES)
          return DORY_PIPELINE_EPREPARE;
        memcpy(scratch_expected + local, (const void *)(uintptr_t)source,
               config->line_bytes);
      }
  }
  const ne16_dma_config_t *config = &job.store;
  if (config->line_bytes != 16u) return DORY_PIPELINE_EPREPARE;
  for (uint32_t plane = 0; plane < config->planes; ++plane)
    for (uint32_t row = 0; row < config->rows; ++row) {
      uint32_t local = config->src + plane * config->src_plane_stride +
                       row * config->src_row_stride - NE16_SCRATCH_BASE;
      uint32_t external = config->dst + plane * config->dst_plane_stride +
                          row * config->dst_row_stride - layer.output;
      if ((uint64_t)local + 16u > NE16_SCRATCH_BYTES ||
          (uint64_t)external + 16u > DORY_GENERATED_OUTPUT_BYTES ||
          external % 16u != 0 || coverage[external / 16u] != 0)
        return DORY_PIPELINE_EPREPARE;
      coverage[external / 16u] = 1;
      memcpy(scratch_expected + local, dory_generated_expected + external, 16);
    }
  /* Replaying in job order preserves unwritten tails from larger tiles. */
  return 0;
}

static int check_guards(void) {
  for (unsigned int i = 0; i < 64; ++i)
    if (input.prefix[i] != SENTINEL || input.suffix[i] != SENTINEL ||
        weights.prefix[i] != SENTINEL || weights.suffix[i] != SENTINEL ||
        output.prefix[i] != SENTINEL || output.suffix[i] != SENTINEL) {
      printf("generated layer DRAM guard mismatch at %u\n", i);
      return -1;
    }
  return 0;
}

static int check_output(void) {
  for (uint32_t i = 0; i < sizeof(coverage); ++i)
    if (coverage[i] != 1) {
      printf("generated layer coverage hole at channel group %u\n", i);
      return -1;
    }
  for (uint32_t i = 0; i < DORY_GENERATED_OUTPUT_BYTES; i += 8u) {
    uint64_t actual, expected;
    memcpy(&actual, output.data + i, sizeof(actual));
    memcpy(&expected, dory_generated_expected + i, sizeof(expected));
    if (actual == expected) continue;
    for (uint32_t byte = 0; byte < 8u; ++byte)
      if (output.data[i + byte] != dory_generated_expected[i + byte]) {
        printf("generated layer output mismatch at %u: %u expected %u\n",
               i + byte, output.data[i + byte], dory_generated_expected[i + byte]);
        return -1;
      }
  }
  return 0;
}

int main(void) {
  printf("DORY generated layer: initializing tensors and all guards\n");
  fflush(stdout);
  memset(&input, SENTINEL, sizeof(input));
  memset(&weights, SENTINEL, sizeof(weights));
  memset(&output, SENTINEL, sizeof(output));
  memcpy(input.data, dory_generated_input, sizeof(input.data));
  memcpy(weights.data, dory_generated_weights, sizeof(weights.data));
  memset(scratch_expected, SENTINEL, sizeof(scratch_expected));
  ne16_test_fill_scratch(SENTINEL);
  uint32_t input_address, weights_address, output_address;
  if (pointer32(input.data, &input_address) != 0 ||
      pointer32(weights.data, &weights_address) != 0 ||
      pointer32(output.data, &output_address) != 0 ||
      dory_generated_layer_init(&layer, input_address, weights_address,
                                output_address) != 0) {
    printf("DORY generated layer initialization failed\n");
    return 1;
  }
  uint32_t input_hash = checksum(input.data, sizeof(input.data));
  uint32_t weights_hash = checksum(weights.data, sizeof(weights.data));
  uint32_t expected_hash = checksum(dory_generated_expected,
                                    DORY_GENERATED_OUTPUT_BYTES);
  dory_ne16_compat_reset_stats();
  nnx_init(8);
  dma_mutex_init();
  const dory_pipeline_memory_t memory[DORY_PIPELINE_SLOTS] = {
      {NE16_SCRATCH_BASE + 0x1000u, DORY_NE16_TILED_SLOT_BYTES},
      {NE16_SCRATCH_BASE + 0x6000u, DORY_NE16_TILED_SLOT_BYTES}};
  uint32_t overlap = ne16_dma_counter(NE16_DMA_NE16_OVERLAP);
  int status = dory_pipeline_init(&pipeline, memory, DORY_GENERATED_JOBS,
                                  prepare, &layer);
  if (status != 0) {
    printf("DORY generated pipeline initialization failed: %d\n", status);
    return 1;
  }
  printf("DORY generated layer: %ux%ux%u, %u cooperative jobs\n",
         DORY_GENERATED_HEIGHT, DORY_GENERATED_WIDTH, DORY_GENERATED_CHANNELS,
         DORY_GENERATED_JOBS);
  fflush(stdout);
  uint32_t completed = 0;
  uint64_t poll_cycles = 0;
  uint32_t polls = 0;
  uint32_t hw_dma0 = ne16_dma_counter(NE16_DMA_BUSY_CYCLES);
  uint32_t hw_ne160 = ne16_dma_counter(NE16_DMA_NE16_BUSY_CYCLES);
  uint32_t hw_both0 = ne16_dma_counter(NE16_DMA_BOTH_BUSY_CYCLES);
  uint64_t run_start = ne16_perf_cycles();
  do {
    uint64_t poll_start = ne16_perf_cycles();
    ++ne16_perf.poll_seq;
    status = dory_pipeline_poll(&pipeline);
    poll_cycles += ne16_perf_cycles() - poll_start;
    ++polls;
    if (status < 0) {
      printf("DORY generated layer failure=%d completed=%u; drain=%d\n",
             status, pipeline.completed, dory_pipeline_drain(&pipeline));
      return 1;
    }
    if (pipeline.completed != completed) {
      completed = pipeline.completed;
      if (completed % 3u == 0 || completed == DORY_GENERATED_JOBS) {
        printf("DORY generated layer: completed=%u/%u\n",
               completed, DORY_GENERATED_JOBS);
        fflush(stdout);
      }
    }
  } while (status == 0);
  uint64_t run_cycles = ne16_perf_cycles() - run_start;
  uint32_t hw_dma = ne16_dma_counter(NE16_DMA_BUSY_CYCLES) - hw_dma0;
  uint32_t hw_ne16 = ne16_dma_counter(NE16_DMA_NE16_BUSY_CYCLES) - hw_ne160;
  uint32_t hw_both = ne16_dma_counter(NE16_DMA_BOTH_BUSY_CYCLES) - hw_both0;
  nnx_term();
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  uint32_t overlap_delta = ne16_dma_counter(NE16_DMA_NE16_OVERLAP) - overlap;
  if (completed != DORY_GENERATED_JOBS || prepared != DORY_GENERATED_JOBS ||
      uses[0] < 2 || uses[1] < 2 || overlap_delta == 0 ||
      stats.dma_l2_to_l1_count != DORY_GENERATED_JOBS * 4u ||
      stats.dma_l1_to_l2_count != DORY_GENERATED_JOBS ||
      stats.ne16_dispatch_count != DORY_GENERATED_JOBS ||
      stats.ne16_completion_count != DORY_GENERATED_JOBS ||
      dory_ne16_compat_error() != 0 || !dma_transfer_idle() || !ne16_idle() ||
      checksum(input.data, sizeof(input.data)) != input_hash ||
      checksum(weights.data, sizeof(weights.data)) != weights_hash ||
      checksum(dory_generated_expected, DORY_GENERATED_OUTPUT_BYTES) != expected_hash) {
    printf("DORY generated layer stats/integrity failed: slots=%u/%u overlap=%u\n",
           uses[0], uses[1], overlap_delta);
    return 1;
  }
  for (uint32_t index = 0; index < DORY_GENERATED_JOBS; ++index)
    if (replay(index, memory[slot_of_job[index]]) != 0) {
      printf("DORY generated layer scratch/coverage replay failed at %u\n", index);
      return 1;
    }
  if (check_output() != 0 || check_guards() != 0 ||
      ne16_test_check_scratch(scratch_expected) != 0) return 1;
  printf("DORY generated layer PASS: %u output bytes, %u jobs, slots reused %u/%u, "
         "DMA/NE16 overlap grants=%u\n", DORY_GENERATED_OUTPUT_BYTES,
         DORY_GENERATED_JOBS, uses[0], uses[1], overlap_delta);
  /* This firmware's printf does not handle %ll, so each 64-bit counter is
   * printed as high and low 32-bit halves: value = hi * 2^32 + lo. */
  uint64_t idle_cycles = run_cycles - ne16_perf.busy_cycles;
  printf("PERF run_cycles_hi=%u run_cycles_lo=%u ", (uint32_t)(run_cycles >> 32),
         (uint32_t)run_cycles);
  printf("poll_cycles_hi=%u poll_cycles_lo=%u polls=%u ", (uint32_t)(poll_cycles >> 32),
         (uint32_t)poll_cycles, polls);
  printf("tasks=%u dispatch_cycles_hi=%u dispatch_cycles_lo=%u ", ne16_perf.tasks,
         (uint32_t)(ne16_perf.dispatch_cycles >> 32), (uint32_t)ne16_perf.dispatch_cycles);
  printf("ne16_busy_cycles_hi=%u ne16_busy_cycles_lo=%u ",
         (uint32_t)(ne16_perf.busy_cycles >> 32), (uint32_t)ne16_perf.busy_cycles);
  printf("ne16_idle_cycles_hi=%u ne16_idle_cycles_lo=%u ", (uint32_t)(idle_cycles >> 32),
         (uint32_t)idle_cycles);
  printf("resolve_calls=%u dma_l2_to_l1=%u dma_l1_to_l2=%u overlap_grants=%u\n",
         ne16_perf.resolve_calls, stats.dma_l2_to_l1_count, stats.dma_l1_to_l2_count,
         overlap_delta);
  printf("PERF2 cpu_lag_upper_hi=%u cpu_lag_upper_lo=%u ",
         (uint32_t)(ne16_perf.cpu_lag_upper_cycles >> 32),
         (uint32_t)ne16_perf.cpu_lag_upper_cycles);
  printf("dma_wait_hi=%u dma_wait_lo=%u dma_wait_events=%u\n",
         (uint32_t)(ne16_perf.dma_wait_cycles >> 32),
         (uint32_t)ne16_perf.dma_wait_cycles, ne16_perf.dma_wait_events);
  ne16_perf_report();
  /* Hardware busy counters (same 500 MHz clock as rdcycle). */
  printf("PERF5 hw_dma_busy=%u hw_ne16_busy=%u hw_both_busy=%u hw_neither_busy=%u\n",
         hw_dma, hw_ne16, hw_both,
         (uint32_t)run_cycles - hw_dma - hw_ne16 + hw_both);
  ne16_perf_dma_report();
  return 0;
}
