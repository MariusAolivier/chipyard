#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dory_chain_l0_layer.h"
#include "dory_chain_l1_layer.h"
#include "dory_tiled_layer.h"
#include "ne16_test_scratch.h"

#define SENTINEL 0xa5u
#define GUARDED(name, bytes) static struct { \
  uint8_t prefix[64], data[bytes], suffix[64]; \
} name __attribute__((aligned(64)))

_Static_assert(DORY_CHAIN_L0_OUTPUT_BYTES == DORY_CHAIN_L1_INPUT_BYTES,
               "chain intermediate must be consumed without repacking");
_Static_assert(DORY_CHAIN_L0_INPUT_BYTES + DORY_CHAIN_L0_WEIGHTS_BYTES +
                   DORY_CHAIN_L0_OUTPUT_BYTES > NE16_SCRATCH_BYTES &&
               DORY_CHAIN_L1_INPUT_BYTES + DORY_CHAIN_L1_WEIGHTS_BYTES +
                   DORY_CHAIN_L1_OUTPUT_BYTES > NE16_SCRATCH_BYTES,
               "both generated layers must require tiling");
_Static_assert(DORY_CHAIN_L0_QUANT_SHIFT == 11u && DORY_CHAIN_L1_QUANT_SHIFT == 9u,
               "this fixture must exercise distinct generated shifts");

GUARDED(input, DORY_CHAIN_L0_INPUT_BYTES);
GUARDED(weights0, DORY_CHAIN_L0_WEIGHTS_BYTES);
GUARDED(weights1, DORY_CHAIN_L1_WEIGHTS_BYTES);
GUARDED(intermediate, DORY_CHAIN_L0_OUTPUT_BYTES);
GUARDED(output, DORY_CHAIN_L1_OUTPUT_BYTES);
static uint8_t scratch_expected[NE16_SCRATCH_BYTES];
static uint8_t coverage0[DORY_CHAIN_L0_OUTPUT_BYTES / 16u];
static uint8_t coverage1[DORY_CHAIN_L1_OUTPUT_BYTES / 16u];
static uint8_t slots0[DORY_CHAIN_L0_JOBS], slots1[DORY_CHAIN_L1_JOBS];
static dory_chain_l0_layer_t layer0;
static dory_chain_l1_layer_t layer1;
static dory_pipeline_t pipeline;
static const dory_pipeline_memory_t memory[DORY_PIPELINE_SLOTS] = {
    {NE16_SCRATCH_BASE + 0x1000u, DORY_NE16_TILED_SLOT_BYTES},
    {NE16_SCRATCH_BASE + 0x6000u, DORY_NE16_TILED_SLOT_BYTES}};

typedef struct {
  void *context;
  dory_pipeline_prepare_t prepare;
  uint32_t input, output, bytes, jobs, shift, prepared, uses[DORY_PIPELINE_SLOTS];
  uint8_t *slots, *coverage;
  const uint8_t *expected;
} chain_layer_t;

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
                   dory_pipeline_memory_t allocation, dory_pipeline_job_t *job) {
  chain_layer_t *layer = context;
  if (index != layer->prepared || index >= layer->jobs ||
      slot >= DORY_PIPELINE_SLOTS) return DORY_PIPELINE_EPREPARE;
  int status = layer->prepare(layer->context, index, slot, allocation, job);
  if (status != 0) return status;
  if (((job->task.words[23] >> 16) & 31u) != layer->shift ||
      job->load_count != 4u || job->loads[0].src < layer->input)
    return DORY_PIPELINE_EPREPARE;
  layer->slots[index] = (uint8_t)slot;
  ++layer->prepared;
  ++layer->uses[slot];
  return 0;
}

static int check_bytes(const uint8_t *actual, const uint8_t *expected,
                       uint32_t bytes, const char *name) {
  for (uint32_t i = 0; i < bytes; i += 8u) {
    uint64_t a, b;
    memcpy(&a, actual + i, sizeof(a));
    memcpy(&b, expected + i, sizeof(b));
    if (a == b) continue;
    for (uint32_t j = 0; j < 8u; ++j)
      if (actual[i + j] != expected[i + j]) {
        printf("DORY chain %s mismatch at %u: %u expected %u\n",
               name, i + j, actual[i + j], expected[i + j]);
        return -1;
      }
  }
  return 0;
}

static int check_guards(void) {
  for (uint32_t i = 0; i < 64u; ++i)
    if (input.prefix[i] != SENTINEL || input.suffix[i] != SENTINEL ||
        weights0.prefix[i] != SENTINEL || weights0.suffix[i] != SENTINEL ||
        weights1.prefix[i] != SENTINEL || weights1.suffix[i] != SENTINEL ||
        intermediate.prefix[i] != SENTINEL || intermediate.suffix[i] != SENTINEL ||
        output.prefix[i] != SENTINEL || output.suffix[i] != SENTINEL) {
      printf("DORY chain DRAM guard mismatch at %u\n", i);
      return -1;
    }
  return 0;
}

static int run_layer(chain_layer_t *layer, unsigned int number) {
  if (!dma_transfer_idle() || !ne16_idle()) {
    printf("DORY chain layer %u: predecessor still owns DMA/NE16\n", number);
    return -1;
  }
  dory_ne16_compat_reset_stats();
  nnx_init(8);
  dma_mutex_init();
  uint32_t overlap = ne16_dma_counter(NE16_DMA_NE16_OVERLAP);
  int status = dory_pipeline_init(&pipeline, memory, layer->jobs, prepare, layer);
  if (status != 0) {
    printf("DORY chain pipeline %u initialization failed: %d\n", number, status);
    return -1;
  }
  printf("DORY chain layer %u: %u jobs, quantization shift=%u\n",
         number, layer->jobs, layer->shift);
  fflush(stdout);
  uint32_t completed = 0;
  do {
    status = dory_pipeline_poll(&pipeline);
    if (status < 0) {
      printf("DORY chain layer %u failure=%d completed=%u; drain=%d\n",
             number, status, pipeline.completed, dory_pipeline_drain(&pipeline));
      return -1;
    }
    if (pipeline.completed != completed) {
      completed = pipeline.completed;
      if (completed % 4u == 0 || completed == layer->jobs) {
        printf("DORY chain layer %u: completed=%u/%u\n", number, completed, layer->jobs);
        fflush(stdout);
      }
    }
  } while (status == 0);
  nnx_term();
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  uint32_t delta = ne16_dma_counter(NE16_DMA_NE16_OVERLAP) - overlap;
  if (completed != layer->jobs || layer->prepared != layer->jobs ||
      layer->uses[0] < 2u || layer->uses[1] < 2u || delta == 0 ||
      stats.dma_l2_to_l1_count != layer->jobs * 4u ||
      stats.dma_l1_to_l2_count != layer->jobs ||
      stats.ne16_dispatch_count != layer->jobs ||
      stats.ne16_completion_count != layer->jobs ||
      dory_ne16_compat_error() != 0 || !dma_transfer_idle() || !ne16_idle()) {
    printf("DORY chain layer %u stats failed: slots=%u/%u overlap=%u\n",
           number, layer->uses[0], layer->uses[1], delta);
    return -1;
  }
  printf("DORY chain layer %u stores complete: slots=%u/%u overlap grants=%u\n",
         number, layer->uses[0], layer->uses[1], delta);
  fflush(stdout);
  return 0;
}

static int replay(chain_layer_t *layer, uint32_t index) {
  dory_pipeline_job_t job;
  int status = layer->prepare(layer->context, index, layer->slots[index],
                             memory[layer->slots[index]], &job);
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
        memcpy(scratch_expected + local, (const void *)(uintptr_t)source, config->line_bytes);
      }
  }
  const ne16_dma_config_t *store = &job.store;
  if (store->line_bytes != 16u) return DORY_PIPELINE_EPREPARE;
  for (uint32_t plane = 0; plane < store->planes; ++plane)
    for (uint32_t row = 0; row < store->rows; ++row) {
      uint32_t local = store->src + plane * store->src_plane_stride +
                       row * store->src_row_stride - NE16_SCRATCH_BASE;
      uint32_t external = store->dst + plane * store->dst_plane_stride +
                          row * store->dst_row_stride - layer->output;
      if ((uint64_t)local + 16u > NE16_SCRATCH_BYTES ||
          (uint64_t)external + 16u > layer->bytes ||
          external % 16u != 0 || layer->coverage[external / 16u] != 0)
        return DORY_PIPELINE_EPREPARE;
      layer->coverage[external / 16u] = 1;
      memcpy(scratch_expected + local, layer->expected + external, 16);
    }
  return 0;
}

int main(void) {
  printf("DORY generated chain: initializing tensors and guards\n");
  fflush(stdout);
  memset(&input, SENTINEL, sizeof(input));
  memset(&weights0, SENTINEL, sizeof(weights0));
  memset(&weights1, SENTINEL, sizeof(weights1));
  memset(&intermediate, SENTINEL, sizeof(intermediate));
  memset(&output, SENTINEL, sizeof(output));
  memcpy(input.data, dory_chain_l0_input, sizeof(input.data));
  memcpy(weights0.data, dory_chain_l0_weights, sizeof(weights0.data));
  memcpy(weights1.data, dory_chain_l1_weights, sizeof(weights1.data));
  memset(scratch_expected, SENTINEL, sizeof(scratch_expected));
  ne16_test_fill_scratch(SENTINEL);
  uint32_t in, w0, w1, mid, out;
  if (pointer32(input.data, &in) != 0 || pointer32(weights0.data, &w0) != 0 ||
      pointer32(weights1.data, &w1) != 0 || pointer32(intermediate.data, &mid) != 0 ||
      pointer32(output.data, &out) != 0 ||
      dory_chain_l0_layer_init(&layer0, in, w0, mid) != 0 ||
      dory_chain_l1_layer_init(&layer1, mid, w1, out) != 0 ||
      layer0.output != layer1.input) {
    printf("DORY generated chain initialization failed\n");
    return 1;
  }
  chain_layer_t layers[2] = {
      {.context = &layer0, .prepare = dory_chain_l0_layer_prepare,
       .input = in, .output = mid, .bytes = DORY_CHAIN_L0_OUTPUT_BYTES,
       .jobs = DORY_CHAIN_L0_JOBS, .shift = DORY_CHAIN_L0_QUANT_SHIFT,
       .slots = slots0, .coverage = coverage0, .expected = dory_chain_l0_expected},
      {.context = &layer1, .prepare = dory_chain_l1_layer_prepare,
       .input = mid, .output = out, .bytes = DORY_CHAIN_L1_OUTPUT_BYTES,
       .jobs = DORY_CHAIN_L1_JOBS, .shift = DORY_CHAIN_L1_QUANT_SHIFT,
       .slots = slots1, .coverage = coverage1, .expected = dory_chain_l1_expected}};
  uint32_t hashes[] = {
      checksum(input.data, sizeof(input.data)),
      checksum(weights0.data, sizeof(weights0.data)),
      checksum(weights1.data, sizeof(weights1.data)),
      checksum(dory_chain_l0_expected, DORY_CHAIN_L0_OUTPUT_BYTES),
      checksum(dory_chain_l1_expected, DORY_CHAIN_L1_OUTPUT_BYTES)};
  if (run_layer(&layers[0], 0) != 0 ||
      check_bytes(intermediate.data, dory_chain_l0_expected,
                  DORY_CHAIN_L0_OUTPUT_BYTES, "intermediate") != 0 ||
      check_guards() != 0) return 1;
  printf("DORY chain intermediate verified; layer 1 reads the same DRAM buffer\n");
  fflush(stdout);
  if (run_layer(&layers[1], 1) != 0) return 1;
  if (checksum(input.data, sizeof(input.data)) != hashes[0] ||
      checksum(weights0.data, sizeof(weights0.data)) != hashes[1] ||
      checksum(weights1.data, sizeof(weights1.data)) != hashes[2] ||
      checksum(dory_chain_l0_expected, DORY_CHAIN_L0_OUTPUT_BYTES) != hashes[3] ||
      checksum(dory_chain_l1_expected, DORY_CHAIN_L1_OUTPUT_BYTES) != hashes[4]) {
    printf("DORY chain source/reference integrity failed\n");
    return 1;
  }
  /* Replay both layers without clearing scratch: retain stale tails across layers. */
  for (unsigned int number = 0; number < 2u; ++number) {
    chain_layer_t *layer = &layers[number];
    for (uint32_t index = 0; index < layer->jobs; ++index)
      if (replay(layer, index) != 0) {
        printf("DORY chain layer %u scratch/coverage replay failed at %u\n", number, index);
        return 1;
      }
    for (uint32_t index = 0; index < layer->bytes / 16u; ++index)
      if (layer->coverage[index] != 1) {
        printf("DORY chain layer %u coverage hole at %u\n", number, index);
        return 1;
      }
  }
  if (check_bytes(intermediate.data, dory_chain_l0_expected,
                  DORY_CHAIN_L0_OUTPUT_BYTES, "intermediate integrity") != 0 ||
      check_bytes(output.data, dory_chain_l1_expected,
                  DORY_CHAIN_L1_OUTPUT_BYTES, "final output") != 0 ||
      check_guards() != 0 || ne16_test_check_scratch(scratch_expected) != 0) return 1;
  printf("DORY generated chain PASS: %u intermediate and %u final bytes, "
         "%u/%u jobs, shifts=%u/%u; shared DRAM, guards and stale tails verified\n",
         DORY_CHAIN_L0_OUTPUT_BYTES, DORY_CHAIN_L1_OUTPUT_BYTES,
         DORY_CHAIN_L0_JOBS, DORY_CHAIN_L1_JOBS,
         DORY_CHAIN_L0_QUANT_SHIFT, DORY_CHAIN_L1_QUANT_SHIFT);
  return 0;
}
