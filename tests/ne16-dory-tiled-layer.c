#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dory_tiled_layer.h"
#include "ne16_dory_layer_data.h"
#include "ne16_test_scratch.h"

#define HEIGHT 32u
#define WIDTH 32u
#define CHANNELS 48u
#define GROUPS (CHANNELS / DORY_NE16_TILE_CHANNELS)
#define OUTPUT_BYTES (HEIGHT * WIDTH * CHANNELS)
#define WEIGHT_BYTES (CHANNELS * (144u + 4u + 4u))
#define JOBS 27u
#define SENTINEL 0xa5u

_Static_assert(NE16_DORY_INPUT_BYTES == HEIGHT * WIDTH * 16u &&
               NE16_DORY_OUTPUT_BYTES == HEIGHT * WIDTH * 16u,
               "unexpected DORY reference dimensions");
_Static_assert(NE16_DORY_INPUT_BYTES + OUTPUT_BYTES + WEIGHT_BYTES >
                   NE16_SCRATCH_BYTES,
               "layer must require tiling, even without padding guards");

static const unsigned int rotations[GROUPS] = {0, 5, 11};
static struct {
  uint8_t prefix[64], data[WEIGHT_BYTES], suffix[64];
} weights __attribute__((aligned(64)));
static struct {
  uint8_t prefix[64], data[OUTPUT_BYTES], suffix[64];
} output __attribute__((aligned(64)));
static uint8_t scratch_expected[NE16_SCRATCH_BYTES];
static dory_pipeline_t pipeline;
static struct {
  dory_ne16_tiled_layer_t layer;
  uint8_t coverage[HEIGHT * WIDTH * GROUPS];
  uint8_t slot_of_job[JOBS];
  uint32_t prepared, uses[DORY_PIPELINE_SLOTS];
} context;

static uint8_t expected(uint32_t y, uint32_t x, uint32_t channel) {
  unsigned int original =
      (channel % 16u + rotations[channel / 16u]) % 16u;
  return ne16_dory_expected[(y * WIDTH + x) * 16u + original];
}

static int pointer32(const void *pointer, uint32_t *address) {
  uintptr_t value = (uintptr_t)pointer;
  if (value < UINT32_C(0x80000000) || value > UINT32_MAX) return -1;
  *address = (uint32_t)value;
  return 0;
}

static uint32_t checksum(const volatile uint8_t *bytes, uint32_t count) {
  uint32_t result = UINT32_C(2166136261);
  for (uint32_t i = 0; i < count; ++i)
    result = (result ^ bytes[i]) * UINT32_C(16777619);
  return result;
}

static int initialize_fixture(void) {
  memset(&weights, SENTINEL, sizeof(weights));
  memset(&output, SENTINEL, sizeof(output));
  /* Output-channel permutations extend the committed DORY convolution to
   * 48 filters. Its original full-image reference remains an independent
   * oracle: no tiled kernel or CPU convolution generates expected results. */
  for (uint32_t channel = 0; channel < CHANNELS; ++channel) {
    unsigned int original =
        (channel % 16u + rotations[channel / 16u]) % 16u;
    memcpy(weights.data + channel * 144u,
           ne16_dory_weights + original * 144u, 144);
    memcpy(weights.data + CHANNELS * 144u + channel * 4u,
           ne16_dory_scale + original * 4u, 4);
    memcpy(weights.data + CHANNELS * 148u + channel * 4u,
           ne16_dory_bias + original * 4u, 4);
  }
  uint32_t input_address, weight_address, output_address;
  if (pointer32(ne16_dory_input, &input_address) != 0 ||
      pointer32(weights.data, &weight_address) != 0 ||
      pointer32(output.data, &output_address) != 0 ||
      dory_ne16_tiled_init(&context.layer, input_address, weight_address,
                          output_address, HEIGHT, WIDTH, CHANNELS) != 0 ||
      context.layer.jobs != JOBS)
    return -1;
  memset(scratch_expected, SENTINEL, sizeof(scratch_expected));
  ne16_test_fill_scratch(SENTINEL);
  return 0;
}

static int prepare(void *arg, uint32_t index, unsigned int slot,
                    dory_pipeline_memory_t memory, dory_pipeline_job_t *job) {
  (void)arg;
  if (index != context.prepared) return DORY_PIPELINE_EPREPARE;
  int result = dory_ne16_tiled_prepare(&context.layer, index, slot, memory, job);
  if (result != 0) return result;
  context.slot_of_job[index] = (uint8_t)slot;
  ++context.prepared;
  ++context.uses[slot];
  return 0;
}

static int remember_scratch(uint32_t index, dory_pipeline_memory_t memory) {
  dory_ne16_tile_t tile;
  int result = dory_ne16_tiled_info(&context.layer, index, &tile);
  if (result != 0) return result;
  uint32_t base = memory.base - NE16_SCRATCH_BASE;
  for (uint32_t row = 0; row < tile.input_height; ++row)
    memcpy(scratch_expected + base + DORY_NE16_TILED_INPUT_OFFSET +
               row * tile.input_width * 16u,
           ne16_dory_input +
               ((tile.input_y + row) * WIDTH + tile.input_x) * 16u,
           tile.input_width * 16u);
  memcpy(scratch_expected + base + DORY_NE16_TILED_WEIGHTS_OFFSET,
         weights.data + tile.output_channel * 144u, 2304);
  memcpy(scratch_expected + base + DORY_NE16_TILED_SCALE_OFFSET,
         weights.data + CHANNELS * 144u + tile.output_channel * 4u, 64);
  memcpy(scratch_expected + base + DORY_NE16_TILED_BIAS_OFFSET,
         weights.data + CHANNELS * 148u + tile.output_channel * 4u, 64);
  for (uint32_t y = 0; y < tile.output_height; ++y) {
    for (uint32_t x = 0; x < tile.output_width; ++x) {
      uint32_t pixel = (tile.output_y + y) * WIDTH + tile.output_x + x;
      uint32_t coverage = pixel * GROUPS + tile.output_channel / 16u;
      if (context.coverage[coverage] != 0) {
        printf("tiled layer overlaps pixel %u, channel group %u\n",
               pixel, tile.output_channel / 16u);
        return DORY_PIPELINE_EPREPARE;
      }
      context.coverage[coverage] = 1;
      for (uint32_t channel = 0; channel < 16; ++channel)
        scratch_expected[base + DORY_NE16_TILED_OUTPUT_OFFSET +
                         (y * tile.output_width + x) * 16u + channel] =
            expected(tile.output_y + y, tile.output_x + x,
                     tile.output_channel + channel);
    }
  }
  /* Keep previous larger-tile tails: slot reuse never clears unwritten bytes. */
  return 0;
}

static int check_complete_output(void) {
  const volatile uint8_t *actual = output.data;
  for (uint32_t pixel = 0; pixel < HEIGHT * WIDTH; ++pixel) {
    for (uint32_t group = 0; group < GROUPS; ++group) {
      if (context.coverage[pixel * GROUPS + group] != 1) {
        printf("tiled layer coverage hole pixel=%u group=%u\n", pixel, group);
        return -1;
      }
    }
    for (uint32_t channel = 0; channel < CHANNELS; ++channel) {
      if (actual[pixel * CHANNELS + channel] !=
          expected(pixel / WIDTH, pixel % WIDTH, channel)) {
        printf("tiled layer final output mismatch pixel=%u channel=%u\n",
               pixel, channel);
        return -1;
      }
    }
  }
  for (uint32_t i = 0; i < sizeof(output.prefix); ++i) {
    if (output.prefix[i] != SENTINEL || output.suffix[i] != SENTINEL ||
        weights.prefix[i] != SENTINEL || weights.suffix[i] != SENTINEL) {
      printf("tiled layer DRAM guard mismatch at %u\n", i);
      return -1;
    }
  }
  return 0;
}

int main(void) {
  printf("DORY tiled layer: initializing full-image fixture and scratch guards\n");
  fflush(stdout);
  if (initialize_fixture() != 0) {
    printf("DORY tiled layer fixture initialization failed\n");
    return 1;
  }
  uint32_t input_hash = checksum(ne16_dory_input, NE16_DORY_INPUT_BYTES);
  uint32_t weight_hash = checksum(weights.data, sizeof(weights.data));
  dory_ne16_compat_reset_stats();
  nnx_init(8);
  dma_mutex_init();
  const dory_pipeline_memory_t memory[DORY_PIPELINE_SLOTS] = {
      {.base = NE16_SCRATCH_BASE + 0x1000u,
       .bytes = DORY_NE16_TILED_SLOT_BYTES},
      {.base = NE16_SCRATCH_BASE + 0x6000u,
       .bytes = DORY_NE16_TILED_SLOT_BYTES}};
  uint32_t overlap = ne16_dma_counter(NE16_DMA_NE16_OVERLAP);
  int status = dory_pipeline_init(&pipeline, memory, context.layer.jobs,
                                 prepare, &context);
  if (status != 0) {
    printf("DORY tiled layer pipeline initialization failed: %d\n", status);
    return 1;
  }
  printf("DORY tiled layer: 32x32x48, 72832 tensor bytes, 27 jobs\n");
  fflush(stdout);
  uint32_t checked = 0;
  uint32_t loaded = 0, dispatched = 0;
  uint64_t polls = 0;
  do {
    status = dory_pipeline_poll(&pipeline);
    ++polls;
    if (pipeline.next_load != loaded || pipeline.next_compute != dispatched ||
        (polls & UINT64_C(255)) == 0) {
      loaded = pipeline.next_load;
      dispatched = pipeline.next_compute;
      printf("DORY tiled progress: loads=%u tasks=%u completed=%u slots=%u/%u "
             "compute=%d DMA=%u copied=%u error=%u NNX=%u grants=%u/%u\n",
             loaded, dispatched, pipeline.completed,
             (unsigned int)pipeline.slots[0].state,
             (unsigned int)pipeline.slots[1].state, pipeline.computing_slot,
             ne16_dma_status(), ne16_dma_counter(NE16_DMA_BYTES_COPIED),
             ne16_dma_error(), ne16_status(),
             ne16_dma_counter(NE16_DMA_DMA_GRANTS),
             ne16_dma_counter(NE16_DMA_NE16_GRANTS));
      fflush(stdout);
    }
    if (status < 0) {
      printf("DORY tiled layer failure=%d completed=%u next=%u/%u; draining\n",
             status, pipeline.completed, pipeline.next_load,
             pipeline.next_compute);
      fflush(stdout);
      printf("DORY tiled layer drain=%d\n", dory_pipeline_drain(&pipeline));
      return 1;
    }
    while (checked < pipeline.completed) {
      ++checked;
      if (checked % 3u == 0 || checked == JOBS) {
        printf("DORY tiled layer: completed=%u/%u\n", checked, JOBS);
        fflush(stdout);
      }
    }
  } while (status == 0);
  nnx_term();
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  uint32_t overlap_delta = ne16_dma_counter(NE16_DMA_NE16_OVERLAP) - overlap;
  if (checked != JOBS || context.prepared != JOBS ||
      context.uses[0] < 2 || context.uses[1] < 2 ||
      stats.dma_l2_to_l1_count != JOBS * 4u ||
      stats.dma_l1_to_l2_count != JOBS ||
      stats.ne16_dispatch_count != JOBS ||
      stats.ne16_completion_count != JOBS || overlap_delta == 0 ||
      dory_ne16_compat_error() != 0 || !dma_transfer_idle() || !ne16_idle() ||
      checksum(ne16_dory_input, NE16_DORY_INPUT_BYTES) != input_hash ||
      checksum(weights.data, sizeof(weights.data)) != weight_hash) {
    printf("DORY tiled layer stats/data failed: jobs=%u uses=%u/%u overlap=%u\n",
           checked, context.uses[0], context.uses[1], overlap_delta);
    return 1;
  }
  /* Replay expected writes after completion, not in the scheduling callback:
   * reference computation must not delay the DMA/compute overlap being tested. */
  for (uint32_t index = 0; index < JOBS; ++index)
    if (remember_scratch(index, memory[context.slot_of_job[index]]) != 0)
      return 1;
  if (check_complete_output() != 0) return 1;
  printf("DORY tiled layer: checking every scratch byte including stale tails\n");
  fflush(stdout);
  if (ne16_test_check_scratch(scratch_expected) != 0) return 1;
  printf("DORY tiled layer PASS: 49152 output bytes, 27 halo/channel tiles, "
         "slots reused %u/%u, DMA/NE16 overlap grants=%u\n",
         context.uses[0], context.uses[1], overlap_delta);
  return 0;
}
