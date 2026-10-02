#ifndef CHIPYARD_DORY_TILED_LAYER_H
#define CHIPYARD_DORY_TILED_LAYER_H

#include "dory_pipeline.h"

#define DORY_NE16_TILE_HEIGHT 12u
#define DORY_NE16_TILE_WIDTH 12u
#define DORY_NE16_TILE_CHANNELS 16u
#define DORY_NE16_TILED_SLOT_BYTES 16384u
#define DORY_NE16_TILED_INPUT_OFFSET 2048u
#define DORY_NE16_TILED_OUTPUT_OFFSET 8192u
#define DORY_NE16_TILED_WEIGHTS_OFFSET 12288u
#define DORY_NE16_TILED_SCALE_OFFSET (DORY_NE16_TILED_WEIGHTS_OFFSET + 2304u)
#define DORY_NE16_TILED_BIAS_OFFSET (DORY_NE16_TILED_SCALE_OFFSET + 64u)

typedef struct {
  uint32_t input, weights, output;
  uint32_t height, width, output_channels, jobs;
} dory_ne16_tiled_layer_t;

typedef struct {
  uint32_t output_y, output_x, output_channel;
  uint32_t output_height, output_width;
  uint32_t input_y, input_x, input_height, input_width;
  uint8_t padding_top, padding_bottom, padding_left, padding_right;
} dory_ne16_tile_t;

/* One complete HWC 3x3 stride-1 same-padded BN/ReLU convolution: uint8 input
 * with 16 channels, uint8 output with a multiple of 16 channels, packed 8-bit
 * DORY NE16 filters (144 bytes/output channel), then int32 scales and biases.
 * Quantization is the existing DORY shift-11, weight-offset-minus-128 mode.
 * All three DRAM tensors must be disjoint and remain alive through draining.
 * Initialize NNX/DMA separately; these geometry/preparation APIs do no MMIO.
 * Jobs are row-major spatial tiles, each with successive 16-channel groups.
 * Each slot includes prefix/read-ahead guards; caller initializes those once.
 * Only global edges are padded; internal tiles load their real input halos. */
int dory_ne16_tiled_init(dory_ne16_tiled_layer_t *layer,
                         uint32_t input, uint32_t weights, uint32_t output,
                         uint32_t height, uint32_t width,
                         uint32_t output_channels);
int dory_ne16_tiled_info(const dory_ne16_tiled_layer_t *layer, uint32_t index,
                         dory_ne16_tile_t *tile);
int dory_ne16_tiled_prepare(void *context, uint32_t index, unsigned int slot,
                            dory_pipeline_memory_t memory,
                            dory_pipeline_job_t *job);

#endif
