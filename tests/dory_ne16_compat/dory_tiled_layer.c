#include "dory_tiled_layer.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "ne16_driver.h"

_Static_assert(DORY_NE16_TILED_INPUT_OFFSET >=
                   (DORY_NE16_TILE_WIDTH + 3u) * DORY_NE16_TILE_CHANNELS,
               "insufficient padding prefix");
_Static_assert(DORY_NE16_TILED_INPUT_OFFSET +
                   (DORY_NE16_TILE_HEIGHT + 2u) *
                   (DORY_NE16_TILE_WIDTH + 2u) * DORY_NE16_TILE_CHANNELS + 36u
                   <= DORY_NE16_TILED_OUTPUT_OFFSET,
               "input read-ahead overlaps output");
_Static_assert(DORY_NE16_TILED_OUTPUT_OFFSET +
                   DORY_NE16_TILE_HEIGHT * DORY_NE16_TILE_WIDTH *
                   DORY_NE16_TILE_CHANNELS + 36u
                   <= DORY_NE16_TILED_WEIGHTS_OFFSET,
               "output overlaps weights");
_Static_assert(DORY_NE16_TILED_BIAS_OFFSET + 64u + 36u <=
                   DORY_NE16_TILED_SLOT_BYTES,
               "normalization read-ahead exceeds slot");

static int external_range(uint32_t address, uint64_t bytes) {
  return address >= UINT32_C(0x80000000) && bytes != 0 &&
         (uint64_t)address + bytes <= UINT64_C(0x100000000);
}

static int overlaps(uint32_t a, uint64_t size_a,
                     uint32_t b, uint64_t size_b) {
  return (uint64_t)a < (uint64_t)b + size_b &&
         (uint64_t)b < (uint64_t)a + size_a;
}

int dory_ne16_tiled_init(dory_ne16_tiled_layer_t *layer,
                         uint32_t input, uint32_t weights, uint32_t output,
                         uint32_t height, uint32_t width,
                         uint32_t output_channels) {
  if (layer == NULL || height == 0 || width == 0 ||
      height > UINT16_MAX || width > UINT16_MAX ||
      output_channels == 0 || output_channels > UINT16_MAX ||
      output_channels % DORY_NE16_TILE_CHANNELS != 0)
    return DORY_PIPELINE_EINVAL;
  uint64_t pixels = (uint64_t)height * width;
  uint64_t input_bytes = pixels * DORY_NE16_TILE_CHANNELS;
  uint64_t output_bytes = pixels * output_channels;
  uint64_t weight_bytes = (uint64_t)output_channels * (144u + 4u + 4u);
  uint64_t jobs = (uint64_t)((height + DORY_NE16_TILE_HEIGHT - 1u) /
                            DORY_NE16_TILE_HEIGHT) *
                  ((width + DORY_NE16_TILE_WIDTH - 1u) /
                   DORY_NE16_TILE_WIDTH) *
                  (output_channels / DORY_NE16_TILE_CHANNELS);
  if (!external_range(input, input_bytes) ||
      !external_range(output, output_bytes) ||
      !external_range(weights, weight_bytes) || jobs > UINT32_MAX ||
      overlaps(input, input_bytes, output, output_bytes) ||
      overlaps(input, input_bytes, weights, weight_bytes) ||
      overlaps(output, output_bytes, weights, weight_bytes))
    return DORY_PIPELINE_EINVAL;
  *layer = (dory_ne16_tiled_layer_t){
      .input = input, .weights = weights, .output = output,
      .height = height, .width = width,
      .output_channels = output_channels, .jobs = (uint32_t)jobs};
  return 0;
}

static uint32_t minimum(uint32_t a, uint32_t b) {
  return a < b ? a : b;
}

int dory_ne16_tiled_info(const dory_ne16_tiled_layer_t *layer, uint32_t index,
                         dory_ne16_tile_t *tile) {
  if (layer == NULL || tile == NULL) return DORY_PIPELINE_EINVAL;
  dory_ne16_tiled_layer_t checked;
  if (dory_ne16_tiled_init(&checked, layer->input, layer->weights, layer->output,
                          layer->height, layer->width,
                          layer->output_channels) != 0 ||
      checked.jobs != layer->jobs || index >= layer->jobs)
    return DORY_PIPELINE_EINVAL;
  uint32_t groups = layer->output_channels / DORY_NE16_TILE_CHANNELS;
  uint32_t columns = (layer->width + DORY_NE16_TILE_WIDTH - 1u) /
                     DORY_NE16_TILE_WIDTH;
  uint32_t spatial = index / groups;
  dory_ne16_tile_t result = {
      .output_y = (spatial / columns) * DORY_NE16_TILE_HEIGHT,
      .output_x = (spatial % columns) * DORY_NE16_TILE_WIDTH,
      .output_channel = (index % groups) * DORY_NE16_TILE_CHANNELS};
  result.output_height = minimum(DORY_NE16_TILE_HEIGHT,
                                  layer->height - result.output_y);
  result.output_width = minimum(DORY_NE16_TILE_WIDTH,
                                 layer->width - result.output_x);
  result.padding_top = result.output_y == 0;
  result.padding_left = result.output_x == 0;
  result.padding_bottom = result.output_y + result.output_height == layer->height;
  result.padding_right = result.output_x + result.output_width == layer->width;
  result.input_y = result.output_y - !result.padding_top;
  result.input_x = result.output_x - !result.padding_left;
  result.input_height = result.output_height + 2u -
                        result.padding_top - result.padding_bottom;
  result.input_width = result.output_width + 2u -
                       result.padding_left - result.padding_right;
  *tile = result;
  return 0;
}

static ne16_dma_config_t load(uint32_t source, uint32_t destination,
                              uint32_t bytes) {
  return (ne16_dma_config_t){.src = source, .dst = destination,
                             .line_bytes = bytes, .rows = 1, .planes = 1};
}

int dory_ne16_tiled_prepare(void *context, uint32_t index, unsigned int slot,
                            dory_pipeline_memory_t memory,
                            dory_pipeline_job_t *job) {
  const dory_ne16_tiled_layer_t *layer = context;
  dory_ne16_tile_t tile;
  if (job == NULL || slot >= DORY_PIPELINE_SLOTS ||
      memory.bytes < DORY_NE16_TILED_SLOT_BYTES ||
      memory.base < NE16_SCRATCH_BASE ||
      (uint64_t)memory.base + memory.bytes >
          NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES ||
      dory_ne16_tiled_info(layer, index, &tile) != 0)
    return DORY_PIPELINE_EINVAL;
  ne16_dory_3x3_config_t config = {
      .input = memory.base + DORY_NE16_TILED_INPUT_OFFSET,
      .output = memory.base + DORY_NE16_TILED_OUTPUT_OFFSET,
      .weights = memory.base + DORY_NE16_TILED_WEIGHTS_OFFSET,
      .scale = memory.base + DORY_NE16_TILED_SCALE_OFFSET,
      .bias = memory.base + DORY_NE16_TILED_BIAS_OFFSET,
      .input_width = tile.input_width, .input_channels = DORY_NE16_TILE_CHANNELS,
      .output_height = tile.output_height, .output_width = tile.output_width,
      .output_channels = DORY_NE16_TILE_CHANNELS,
      .padding_top = tile.padding_top, .padding_right = tile.padding_right,
      .padding_bottom = tile.padding_bottom, .padding_left = tile.padding_left,
      .weight_offset_factor = -128};
  ne16_task_t task;
  if (ne16_build_dory_3x3_task(&task, &config) != 0)
    return DORY_PIPELINE_EPREPARE;
  dory_pipeline_job_t result = {.load_count = 4};
  memcpy(&result.task, &task, sizeof(task));
  result.loads[0] = (ne16_dma_config_t){
      .src = layer->input +
             (tile.input_y * layer->width + tile.input_x) *
             DORY_NE16_TILE_CHANNELS,
      .dst = config.input,
      .line_bytes = tile.input_width * DORY_NE16_TILE_CHANNELS,
      .rows = tile.input_height, .planes = 1,
      .src_row_stride = layer->width * DORY_NE16_TILE_CHANNELS,
      .dst_row_stride = tile.input_width * DORY_NE16_TILE_CHANNELS};
  result.loads[1] = load(layer->weights + tile.output_channel * 144u,
                         config.weights, 2304);
  result.loads[2] = load(layer->weights + layer->output_channels * 144u +
                         tile.output_channel * 4u, config.scale, 64);
  result.loads[3] = load(layer->weights + layer->output_channels * 148u +
                         tile.output_channel * 4u, config.bias, 64);
  result.store = (ne16_dma_config_t){
      .src = config.output,
      .dst = layer->output +
             (tile.output_y * layer->width + tile.output_x) *
             layer->output_channels + tile.output_channel,
      .line_bytes = DORY_NE16_TILE_CHANNELS,
      .rows = tile.output_width, .planes = tile.output_height,
      .src_row_stride = DORY_NE16_TILE_CHANNELS,
      .dst_row_stride = layer->output_channels,
      .src_plane_stride = tile.output_width * DORY_NE16_TILE_CHANNELS,
      .dst_plane_stride = layer->width * layer->output_channels};
  *job = result;
  return 0;
}
