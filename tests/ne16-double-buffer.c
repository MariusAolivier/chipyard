#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dory_pipeline.h"
#include "dory_generated_tile_data.h"
#include "ne16_dma.h"
#include "ne16_driver.h"

#define JOBS 6u
#define SLOT_BYTES (16u * 1024u)
#define INPUT_OFFSET 2048u
#define OUTPUT_OFFSET 4096u
#define WEIGHTS_OFFSET 6144u
#define SCALE_OFFSET (WEIGHTS_OFFSET + 2304u)
#define BIAS_OFFSET (SCALE_OFFSET + 64u)
#define SENTINEL 0xa5u

_Static_assert(BIAS_OFFSET + 64u < SLOT_BYTES, "slot footprint exceeds allocation");
_Static_assert(DORY_TILE_INPUT_BYTES == 1024 &&
               DORY_TILE_OUTPUT_BYTES == 1024 &&
               DORY_TILE_WEIGHTS_BYTES == 2432, "unexpected DORY fixture");
_Static_assert(NE16_SCRATCH_BASE % sizeof(uint64_t) == 0 &&
               NE16_SCRATCH_BYTES % sizeof(uint64_t) == 0,
               "scratch guard I/O requires aligned whole words");

typedef struct {
  uint8_t prefix[64];
  uint8_t data[DORY_TILE_OUTPUT_BYTES];
  uint8_t suffix[64];
} guarded_output_t;

static uint8_t input[DORY_TILE_INPUT_BYTES] __attribute__((aligned(64)));
static uint8_t weights[JOBS][DORY_TILE_WEIGHTS_BYTES] __attribute__((aligned(64)));
static uint8_t references[JOBS][DORY_TILE_OUTPUT_BYTES];
static guarded_output_t outputs[JOBS] __attribute__((aligned(64)));
static uint8_t scratch_expected[NE16_SCRATCH_BYTES];
static dory_pipeline_t pipeline;

typedef struct {
  uint32_t input_ptr;
  uint32_t weights_ptr[JOBS], output_ptr[JOBS];
  unsigned int slot_uses[DORY_PIPELINE_SLOTS];
  uint32_t last_job[DORY_PIPELINE_SLOTS], prepared;
} fixture_context_t;

static fixture_context_t context;

static void initialize_scratch(volatile uint64_t *scratch) {
  const uint64_t guard = UINT64_C(0x0101010101010101) * SENTINEL;
  for (uint32_t i = 0; i < NE16_SCRATCH_BYTES / sizeof(*scratch); ++i)
    scratch[i] = guard;
  ne16_memory_fence();
}

static int check_scratch_bytes(const volatile uint64_t *scratch) {
  ne16_memory_fence();
  for (uint32_t i = 0; i < NE16_SCRATCH_BYTES; i += sizeof(uint64_t)) {
    const uint64_t word = scratch[i / sizeof(word)];
    for (uint32_t byte = 0; byte < sizeof(word); ++byte) {
      uint8_t actual = (uint8_t)(word >> (byte * 8u));
      if (actual != scratch_expected[i + byte]) {
        printf("pipeline scratch data/guard mismatch at 0x%x: %u expected %u\n",
               i + byte, actual, scratch_expected[i + byte]);
        return -1;
      }
    }
  }
  ne16_memory_fence();
  return 0;
}

static int pointer32(const void *p, uint32_t *address) {
  uintptr_t value = (uintptr_t)p;
  if (value < UINT32_C(0x80000000) || value > UINT32_MAX) return -1;
  *address = (uint32_t)value;
  return 0;
}

static DmaTransferConf load_conf(uint32_t ext, uint32_t loc, int bytes) {
  return (DmaTransferConf){.ext = ext, .loc = loc,
                           .length_1d_copy = bytes,
                           .dir = DORY_DMA_DIR_EXT2LOC};
}

static int prepare(void *arg, uint32_t index, unsigned int slot,
                    dory_pipeline_memory_t memory, dory_pipeline_job_t *job) {
  fixture_context_t *c = arg;
  if (index >= JOBS || slot >= DORY_PIPELINE_SLOTS ||
      memory.bytes != SLOT_BYTES || index != c->prepared) return -1;
  ne16_task_t task;
  const ne16_dory_3x3_config_t config = {
      .input = memory.base + INPUT_OFFSET,
      .output = memory.base + OUTPUT_OFFSET,
      .weights = memory.base + WEIGHTS_OFFSET,
      .scale = memory.base + SCALE_OFFSET,
      .bias = memory.base + BIAS_OFFSET,
      .input_width = 8, .input_channels = 16,
      .output_height = 8, .output_width = 8, .output_channels = 16,
      .padding_top = 1, .padding_right = 1,
      .padding_bottom = 1, .padding_left = 1,
      .weight_offset_factor = -128};
  if (ne16_build_dory_3x3_task(&task, &config) != 0) return -1;
  memcpy(&job->task, &task, sizeof(task));
  job->load_count = 4;
  job->loads[0] = load_conf(c->input_ptr, config.input, DORY_TILE_INPUT_BYTES);
  job->loads[1] = load_conf(c->weights_ptr[index], config.weights, 2304);
  job->loads[2] = load_conf(c->weights_ptr[index] + 2304, config.scale, 64);
  job->loads[3] = load_conf(c->weights_ptr[index] + 2368, config.bias, 64);
  job->store = (DmaTransferConf){
      .ext = c->output_ptr[index], .loc = config.output,
      .length_1d_copy = DORY_TILE_OUTPUT_BYTES, .dir = DORY_DMA_DIR_LOC2EXT};
  ++c->slot_uses[slot];
  c->last_job[slot] = index;
  ++c->prepared;
  return 0;
}

static int initialize_fixture(void) {
  memcpy(input, dory_tile_input, sizeof(input));
  if (pointer32(input, &context.input_ptr) != 0) return -1;
  memset(outputs, SENTINEL, sizeof(outputs));
  for (uint32_t job = 0; job < JOBS; ++job) {
    /* NE16 has a 144-byte packed filter per output channel. Permuting whole
     * filters and their scale/bias preserves the committed fixture reference,
     * with the same permutation of each pixel's 16 output channels. Job zero
     * is the original fixture; others have genuinely distinct output bytes. */
    unsigned int rotation = job == 0 ? 0 : job * 2u - 1u;
    for (unsigned int channel = 0; channel < 16; ++channel) {
      unsigned int original = (channel + rotation) % 16;
      memcpy(weights[job] + channel * 144u,
             dory_tile_weights + original * 144u, 144);
      memcpy(weights[job] + 2304u + channel * 4u,
             dory_tile_weights + 2304u + original * 4u, 4);
      memcpy(weights[job] + 2368u + channel * 4u,
             dory_tile_weights + 2368u + original * 4u, 4);
      for (unsigned int pixel = 0; pixel < 64; ++pixel)
        references[job][pixel * 16u + channel] =
            dory_tile_expected[pixel * 16u + original];
    }
    for (uint32_t previous = 0; previous < job; ++previous)
      if (memcmp(references[job], references[previous],
                 DORY_TILE_OUTPUT_BYTES) == 0) {
        printf("fixture permutation produced nondistinct outputs\n");
        return -1;
      }
    if (pointer32(weights[job], &context.weights_ptr[job]) != 0 ||
        pointer32(outputs[job].data, &context.output_ptr[job]) != 0) return -1;
  }
  memset(scratch_expected, SENTINEL, sizeof(scratch_expected));
  initialize_scratch((volatile uint64_t *)(uintptr_t)NE16_SCRATCH_BASE);
  return 0;
}

static int check_output(uint32_t job) {
  for (unsigned int i = 0; i < DORY_TILE_OUTPUT_BYTES; ++i) {
    if (outputs[job].data[i] != references[job][i]) {
      printf("pipeline job %u mismatch at %u: %u expected %u\n",
             job, i, outputs[job].data[i], references[job][i]);
      return -1;
    }
  }
  for (unsigned int i = 0; i < sizeof(outputs[job].prefix); ++i) {
    if (outputs[job].prefix[i] != SENTINEL ||
        outputs[job].suffix[i] != SENTINEL) {
      printf("pipeline DRAM guard changed for job %u at %u\n", job, i);
      return -1;
    }
  }
  return 0;
}

static int check_scratch(const dory_pipeline_memory_t memory[2]) {
  for (unsigned int slot = 0; slot < DORY_PIPELINE_SLOTS; ++slot) {
    if (context.slot_uses[slot] < 2) {
      printf("pipeline slot %u was not reused\n", slot);
      return -1;
    }
    uint32_t offset = memory[slot].base - NE16_SCRATCH_BASE;
    uint32_t last = context.last_job[slot];
    memcpy(scratch_expected + offset + INPUT_OFFSET, input, sizeof(input));
    memcpy(scratch_expected + offset + WEIGHTS_OFFSET, weights[last],
           sizeof(weights[last]));
    memcpy(scratch_expected + offset + OUTPUT_OFFSET, references[last],
           sizeof(references[last]));
  }
  return check_scratch_bytes(
      (const volatile uint64_t *)(uintptr_t)NE16_SCRATCH_BASE);
}

int main(void) {
  printf("NE16 pipeline: initializing fixture and 64-KiB scratch guards\n");
  fflush(stdout);
  if (initialize_fixture() != 0) {
    printf("pipeline fixture initialization failed\n");
    return 1;
  }
  printf("NE16 pipeline: fixture initialized\n");
  fflush(stdout);
  dory_ne16_compat_reset_stats();
  nnx_init(8);
  dma_mutex_init();
  const dory_pipeline_memory_t memory[2] = {
      {.base = NE16_SCRATCH_BASE + 0x1000u, .bytes = SLOT_BYTES},
      {.base = NE16_SCRATCH_BASE + 0x6000u, .bytes = SLOT_BYTES}};
  dory_pipeline_memory_t invalid[2] = {memory[0], memory[0]};
  if (dory_pipeline_init(&pipeline, invalid, JOBS, prepare, &context) !=
      DORY_PIPELINE_EINVAL) return 1;
  dory_ne16_compat_reset_stats();
  invalid[1] = memory[1];
  invalid[1].base = NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 16u;
  if (dory_pipeline_init(&pipeline, invalid, JOBS, prepare, &context) !=
      DORY_PIPELINE_EINVAL) return 1;
  dory_ne16_compat_reset_stats();
  uint32_t overlap = ne16_dma_counter(NE16_DMA_NE16_OVERLAP);
  if (dory_pipeline_init(&pipeline, memory, JOBS, prepare, &context) != 0)
    return 1;
  int status;
  uint32_t checked = 0;
  uint64_t polls = 0;
  do {
    status = dory_pipeline_poll(&pipeline);
    ++polls;
    if ((polls & UINT64_C(16383)) == 0) {
      printf("NE16 pipeline: polls=%llu completed=%u slots=%u/%u compute=%d "
             "next_load=%u next_compute=%u stalled=%u DMA=%u error=%u NNX=%u\n",
             (unsigned long long)polls, pipeline.completed,
             (unsigned int)pipeline.slots[0].state,
             (unsigned int)pipeline.slots[1].state, pipeline.computing_slot,
             pipeline.next_load, pipeline.next_compute, pipeline.stalled_polls,
             ne16_dma_status(), ne16_dma_error(), ne16_status());
      fflush(stdout);
    }
    if (status < 0) {
      printf("NE16 pipeline: failure=%d completed=%u slots=%u/%u compute=%d "
             "next_load=%u next_compute=%u DMA=%u error=%u NNX=%u; draining\n",
             status, pipeline.completed,
             (unsigned int)pipeline.slots[0].state,
             (unsigned int)pipeline.slots[1].state, pipeline.computing_slot,
             pipeline.next_load, pipeline.next_compute,
             ne16_dma_status(), ne16_dma_error(), ne16_status());
      fflush(stdout);
      int drained = dory_pipeline_drain(&pipeline);
      printf("pipeline failed %d, drain=%d\n", status, drained);
      return 1;
    }
    if (checked < pipeline.completed) {
      printf("NE16 pipeline: completed=%u/%u\n", pipeline.completed, JOBS);
      fflush(stdout);
    }
    /* Completion watermark is store order, not load/compute order. */
    while (checked < pipeline.completed) {
      if (check_output(checked) != 0) return 1;
      ++checked;
    }
  } while (status == 0);
  nnx_term();
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  uint32_t overlap_delta = ne16_dma_counter(NE16_DMA_NE16_OVERLAP) - overlap;
  if (checked != JOBS || context.prepared != JOBS ||
      stats.dma_l2_to_l1_count != JOBS * 4u ||
      stats.dma_l1_to_l2_count != JOBS ||
      stats.ne16_dispatch_count != JOBS ||
      stats.ne16_completion_count != JOBS ||
      overlap_delta == 0 || dory_ne16_compat_error() != 0 ||
      !dma_transfer_idle() || !ne16_idle() || check_scratch(memory) != 0) {
    printf("pipeline stats/guards failed: loads=%u stores=%u tasks=%u/%u "
           "overlap=%u slots=%u/%u error=%d\n",
           stats.dma_l2_to_l1_count, stats.dma_l1_to_l2_count,
           stats.ne16_dispatch_count, stats.ne16_completion_count,
           overlap_delta, context.slot_uses[0], context.slot_uses[1],
           dory_ne16_compat_error());
    return 1;
  }
  printf("NE16 double buffer PASS: %u distinct jobs, slots reused %u/%u, "
         "DMA/NE16 overlap grants=%u\n",
         JOBS, context.slot_uses[0], context.slot_uses[1], overlap_delta);
  return 0;
}
