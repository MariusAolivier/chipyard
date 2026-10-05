/* Descriptor/transport validation, not a software convolution or RTL model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dory_generated_layer.h"
#include "dory_tiled_layer.h"
#include "ne16_driver.h"

#define CHECK(test) do { if (!(test)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)
#define INPUT UINT32_C(0x80000003)
#define WEIGHTS UINT32_C(0x81000005)
#define OUTPUT UINT32_C(0x82000007)
#define GUARD 0xa5

static uint8_t input[DORY_GENERATED_INPUT_BYTES], weights[DORY_GENERATED_WEIGHTS_BYTES];
static uint8_t output[DORY_GENERATED_OUTPUT_BYTES], scratch[NE16_SCRATCH_BYTES];
static uint8_t expected_scratch[NE16_SCRATCH_BYTES], coverage[DORY_GENERATED_OUTPUT_BYTES];
static dory_generated_layer_t layer;
static dory_ne16_tiled_layer_t reference;
static const dory_pipeline_memory_t slots[2] = {
    {NE16_SCRATCH_BASE + 0x1000u, 16384u},
    {NE16_SCRATCH_BASE + 0x6000u, 16384u}};
static unsigned int compute_ticks, compute_delay, dispatched, completed, dma_ticks;
static unsigned int dma_count, stores, overlap, uses[2], prepared;
static ne16_task_t active_task;
static ne16_dma_config_t active_dma;

static uint8_t *memory(uint32_t address, uint32_t bytes) {
  const uint32_t bases[] = {INPUT, WEIGHTS, OUTPUT, NE16_SCRATCH_BASE};
  const uint32_t sizes[] = {sizeof(input), sizeof(weights), sizeof(output), sizeof(scratch)};
  uint8_t *data[] = {input, weights, output, scratch};
  for (unsigned int i = 0; i < 4; ++i)
    if (address >= bases[i] && (uint64_t)address + bytes <= (uint64_t)bases[i] + sizes[i])
      return data[i] + (address - bases[i]);
  CHECK(!"unmapped transport span");
  return NULL;
}

static uint8_t tag(uint32_t byte) { return (uint8_t)(byte * 37u + byte / 16u * 11u); }

void ne16_reset(void) { CHECK(!compute_ticks); }

int ne16_submit(const ne16_task_t *task) {
  CHECK(!compute_ticks && dispatched < DORY_GENERATED_JOBS);
  dory_ne16_tile_t tile;
  CHECK(dory_ne16_tiled_info(&reference, dispatched, &tile) == 0);
  uint32_t slot = (task->words[2] - DORY_NE16_TILED_OUTPUT_OFFSET - slots[0].base) /
                  (slots[1].base - slots[0].base);
  CHECK(slot < 2);
  uint8_t *packed = memory(slots[slot].base + DORY_NE16_TILED_INPUT_OFFSET,
                           tile.input_height * tile.input_width * 16u);
  for (uint32_t y = 0; y < tile.output_height; ++y)
    for (uint32_t x = 0; x < tile.output_width; ++x)
      for (int ky = 0; ky < 3; ++ky)
        for (int kx = 0; kx < 3; ++kx) {
          int gy = (int)(tile.output_y + y) + ky - 1;
          int gx = (int)(tile.output_x + x) + kx - 1;
          int ly = (int)y + ky - tile.padding_top;
          int lx = (int)x + kx - tile.padding_left;
          int global_valid = gy >= 0 && gx >= 0 && gy < (int)DORY_GENERATED_HEIGHT &&
                             gx < (int)DORY_GENERATED_WIDTH;
          int local_valid = ly >= 0 && lx >= 0 && ly < (int)tile.input_height &&
                            lx < (int)tile.input_width;
          CHECK(global_valid == local_valid);
          if (global_valid)
            CHECK(memcmp(packed + ((uint32_t)ly * tile.input_width + (uint32_t)lx) * 16u,
                         input + ((uint32_t)gy * DORY_GENERATED_WIDTH + (uint32_t)gx) * 16u,
                         16) == 0);
        }
  CHECK(memcmp(memory(slots[slot].base + DORY_NE16_TILED_WEIGHTS_OFFSET, 2304),
               weights + tile.output_channel * 144u, 2304) == 0);
  CHECK(memcmp(memory(slots[slot].base + DORY_NE16_TILED_SCALE_OFFSET, 64),
               weights + DORY_GENERATED_CHANNELS * 144u + tile.output_channel * 4u, 64) == 0);
  CHECK(memcmp(memory(slots[slot].base + DORY_NE16_TILED_BIAS_OFFSET, 64),
               weights + DORY_GENERATED_CHANNELS * 148u + tile.output_channel * 4u, 64) == 0);
  active_task = *task;
  compute_ticks = compute_delay;
  ++dispatched;
  return 0;
}

int ne16_idle(void) {
  if (!compute_ticks) return 1;
  if (--compute_ticks) return 0;
  dory_ne16_tile_t tile;
  CHECK(dory_ne16_tiled_info(&reference, completed, &tile) == 0);
  for (uint32_t y = 0; y < tile.output_height; ++y)
    for (uint32_t x = 0; x < tile.output_width; ++x)
      for (uint32_t channel = 0; channel < 16; ++channel) {
        uint32_t local = active_task.words[2] + (y * tile.output_width + x) * 16u + channel;
        uint32_t external = ((tile.output_y + y) * DORY_GENERATED_WIDTH +
                             tile.output_x + x) * DORY_GENERATED_CHANNELS +
                             tile.output_channel + channel;
        *memory(local, 1) = expected_scratch[local - NE16_SCRATCH_BASE] = tag(external);
      }
  ++completed;
  return 1;
}

int ne16_dma_submit(const ne16_dma_config_t *config) {
  CHECK(!dma_ticks && ne16_dma_validate(config) == 0);
  active_dma = *config;
  dma_ticks = 3;
  ++dma_count;
  return 0;
}

uint32_t ne16_dma_status(void) { return dma_ticks ? NE16_DMA_BUSY : 0; }

int ne16_dma_poll(void) {
  if (!dma_ticks) return 1;
  if (compute_ticks) ++overlap;
  if (--dma_ticks) return 0;
  for (uint32_t plane = 0; plane < active_dma.planes; ++plane)
    for (uint32_t row = 0; row < active_dma.rows; ++row) {
      uint32_t source = active_dma.src + plane * active_dma.src_plane_stride +
                        row * active_dma.src_row_stride;
      uint32_t destination = active_dma.dst + plane * active_dma.dst_plane_stride +
                             row * active_dma.dst_row_stride;
      memcpy(memory(destination, active_dma.line_bytes),
             memory(source, active_dma.line_bytes), active_dma.line_bytes);
      if (destination >= NE16_SCRATCH_BASE && destination < NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES)
        memcpy(expected_scratch + destination - NE16_SCRATCH_BASE,
               memory(source, active_dma.line_bytes), active_dma.line_bytes);
      else {
        CHECK(destination >= OUTPUT && active_dma.line_bytes == 16);
        for (uint32_t byte = 0; byte < active_dma.line_bytes; ++byte)
          CHECK(++coverage[destination - OUTPUT + byte] == 1);
      }
    }
  if (active_dma.dst >= OUTPUT && active_dma.dst < OUTPUT + sizeof(output)) ++stores;
  return 1;
}

static int prepare(void *context, uint32_t index, unsigned int slot,
                   dory_pipeline_memory_t allocation, dory_pipeline_job_t *job) {
  CHECK(index == prepared++);
  ++uses[slot];
  return dory_generated_layer_prepare(context, index, slot, allocation, job);
}

static void descriptor_checks(void) {
  dory_pipeline_job_t a, b;
  for (uint32_t index = 0; index < DORY_GENERATED_JOBS; ++index)
    for (unsigned int slot = 0; slot < 2; ++slot) {
      CHECK(dory_generated_layer_prepare(&layer, index, slot, slots[slot], &a) == 0);
      CHECK(dory_ne16_tiled_prepare(&reference, index, slot, slots[slot], &b) == 0);
      CHECK(memcmp(&a.task, &b.task, sizeof(a.task)) == 0);
      CHECK(a.load_count == b.load_count && memcmp(a.loads, b.loads, sizeof(a.loads)) == 0);
      CHECK(memcmp(&a.store, &b.store, sizeof(a.store)) == 0);
      for (unsigned int i = 0; i < a.load_count; ++i)
        CHECK(ne16_dma_validate(&a.loads[i]) == 0);
      CHECK(ne16_dma_validate(&a.store) == 0);
    }
  memset(&a, 0x6d, sizeof(a));
  b = a;
#define REJECT(ctx, idx, slot, allocation) do { \
  CHECK(dory_generated_layer_prepare(ctx, idx, slot, allocation, &a) < 0); \
  CHECK(memcmp(&a, &b, sizeof(a)) == 0); \
} while (0)
  REJECT(NULL, 0, 0, slots[0]);
  REJECT(&layer, DORY_GENERATED_JOBS, 0, slots[0]);
  REJECT(&layer, 0, 2, slots[0]);
  dory_pipeline_memory_t invalid = slots[0];
  --invalid.bytes;
  REJECT(&layer, 0, 0, invalid);
  invalid = (dory_pipeline_memory_t){NE16_SCRATCH_BASE - 1u, 16384u};
  REJECT(&layer, 0, 0, invalid);
  invalid = (dory_pipeline_memory_t){NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 16383u, 16384u};
  REJECT(&layer, 0, 0, invalid);
  CHECK(dory_generated_layer_prepare(&layer, 0, 0, slots[0], NULL) < 0);
  dory_generated_layer_t invalid_layer = layer;
  invalid_layer.input = UINT32_MAX;
  REJECT(&invalid_layer, 0, 0, slots[0]);
  dory_ne16_compat_set_error(-88);
  REJECT(&layer, 0, 0, slots[0]);
  dory_ne16_compat_reset_stats();
#undef REJECT
  CHECK(dory_generated_layer_init(NULL, INPUT, WEIGHTS, OUTPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, 0x7fffffffu, WEIGHTS, OUTPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, INPUT, INPUT, OUTPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, INPUT, WEIGHTS, INPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, INPUT, OUTPUT, OUTPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, INPUT, WEIGHTS, UINT32_MAX) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer,
      UINT32_MAX - DORY_GENERATED_INPUT_BYTES + 1u, WEIGHTS, OUTPUT) == 0);
}

static void transport_checks(unsigned int delay) {
  compute_delay = delay;
  dispatched = completed = dma_count = stores = overlap = prepared = 0;
  uses[0] = uses[1] = 0;
  memset(scratch, GUARD, sizeof(scratch));
  memset(expected_scratch, GUARD, sizeof(expected_scratch));
  memset(output, GUARD, sizeof(output));
  memset(coverage, 0, sizeof(coverage));
  dory_ne16_compat_reset_stats();
  nnx_init(8);
  dma_mutex_init();
  dory_pipeline_t pipeline;
  CHECK(dory_pipeline_init(&pipeline, slots, DORY_GENERATED_JOBS, prepare, &layer) == 0);
  CHECK(dory_pipeline_run(&pipeline) == 0);
  CHECK(dispatched == DORY_GENERATED_JOBS && completed == dispatched &&
        stores == dispatched && dma_count == dispatched * 5u &&
        uses[0] >= 2 && uses[1] >= 2 && overlap > 0);
  CHECK(!compute_ticks && !dma_ticks && dory_ne16_compat_error() == 0);
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  CHECK(stats.ne16_dispatch_count == DORY_GENERATED_JOBS &&
        stats.ne16_completion_count == DORY_GENERATED_JOBS &&
        stats.dma_l2_to_l1_count == DORY_GENERATED_JOBS * 4u &&
        stats.dma_l1_to_l2_count == DORY_GENERATED_JOBS);
  for (uint32_t i = 0; i < sizeof(output); ++i)
    CHECK(output[i] == tag(i) && coverage[i] == 1);
  CHECK(memcmp(scratch, expected_scratch, sizeof(scratch)) == 0);
  for (uint32_t i = 0; i < sizeof(input); ++i) CHECK(input[i] == (uint8_t)(i * 29u + 3u));
  for (uint32_t i = 0; i < sizeof(weights); ++i) CHECK(weights[i] == (uint8_t)(i * 17u + 5u));
}

int main(void) {
  CHECK(dory_generated_layer_init(&layer, INPUT, WEIGHTS, OUTPUT) == 0);
  CHECK(dory_ne16_tiled_init(&reference, INPUT, WEIGHTS, OUTPUT,
        DORY_GENERATED_HEIGHT, DORY_GENERATED_WIDTH, DORY_GENERATED_CHANNELS) == 0);
  CHECK(reference.jobs == DORY_GENERATED_JOBS);
  for (uint32_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i * 29u + 3u);
  for (uint32_t i = 0; i < sizeof(weights); ++i) weights[i] = (uint8_t)(i * 17u + 5u);
  descriptor_checks();
  transport_checks(8);
  transport_checks(80);
  puts("DORY generated binding descriptors/halos/transport PASS (no MAC simulation)");
  return 0;
}
