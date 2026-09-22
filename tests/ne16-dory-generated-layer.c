#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dory_generated_network_data.h"
#include "ne16_driver.h"
#include "net_utils.h"
#include "pulp_nnx.h"

#ifndef DORY_LAYER_INDEX
#error "DORY_LAYER_INDEX must select one generated layer"
#endif

#if DORY_LAYER_INDEX == 0
#include "BNReluConvolution0.h"
#define LAYER_NAME "layer0"
#define LAYER_INPUT dory_BNReluConvolution0_input
#define LAYER_WEIGHTS dory_BNReluConvolution0_parameters
#define LAYER_EXPECTED dory_BNReluConvolution0_reference
#define LAYER_INPUT_BYTES DORY_LAYER_0_INPUT_BYTES
#define LAYER_WEIGHTS_BYTES DORY_LAYER_0_PARAMETER_BYTES
#define LAYER_OUTPUT_BYTES DORY_LAYER_0_OUTPUT_BYTES
#define LAYER_TILES DORY_LAYER_0_TILE_COUNT
#define LAYER_FUNCTION BNReluConvolution0
#elif DORY_LAYER_INDEX == 1
#include "BNReluConvolution1.h"
#define LAYER_NAME "layer1"
#define LAYER_INPUT dory_BNReluConvolution1_input
#define LAYER_WEIGHTS dory_BNReluConvolution1_parameters
#define LAYER_EXPECTED dory_BNReluConvolution1_reference
#define LAYER_INPUT_BYTES DORY_LAYER_1_INPUT_BYTES
#define LAYER_WEIGHTS_BYTES DORY_LAYER_1_PARAMETER_BYTES
#define LAYER_OUTPUT_BYTES DORY_LAYER_1_OUTPUT_BYTES
#define LAYER_TILES DORY_LAYER_1_TILE_COUNT
#define LAYER_FUNCTION BNReluConvolution1
#elif DORY_LAYER_INDEX == 2
#include "BNReluConvolution2.h"
#define LAYER_NAME "layer2"
#define LAYER_INPUT dory_BNReluConvolution2_input
#define LAYER_WEIGHTS dory_BNReluConvolution2_parameters
#define LAYER_EXPECTED dory_BNReluConvolution2_reference
#define LAYER_INPUT_BYTES DORY_LAYER_2_INPUT_BYTES
#define LAYER_WEIGHTS_BYTES DORY_LAYER_2_PARAMETER_BYTES
#define LAYER_OUTPUT_BYTES DORY_LAYER_2_OUTPUT_BYTES
#define LAYER_TILES DORY_LAYER_2_TILE_COUNT
#define LAYER_FUNCTION BNReluConvolution2
#else
#error "DORY_LAYER_INDEX must be 0, 1, or 2"
#endif

static uint8_t input[LAYER_INPUT_BYTES];
static uint8_t weights[LAYER_WEIGHTS_BYTES];
static uint8_t output[LAYER_OUTPUT_BYTES];

static int pointer32(const void *pointer, uint32_t *value) {
  uintptr_t address = (uintptr_t)pointer;
  if (address > UINT32_MAX) return -1;
  *value = (uint32_t)address;
  return 0;
}

static int check_guards(void) {
  uint8_t buffer[256];
  for (uint32_t offset = 0; offset < DORY_NETWORK_L1_PADDING_GUARD;
       offset += sizeof(buffer)) {
    uint32_t length = DORY_NETWORK_L1_PADDING_GUARD - offset;
    if (length > sizeof(buffer)) length = sizeof(buffer);
    if (ne16_scratchpad_read(buffer, offset, length) != 0) return -1;
    for (uint32_t index = 0; index < length; ++index)
      if (buffer[index] != 0xa5) return -1;
  }
  for (uint32_t offset = DORY_NETWORK_L1_PADDING_GUARD +
                         DORY_NETWORK_L1_LIVE_LIMIT;
       offset < DORY_NETWORK_L1_CAPACITY; offset += sizeof(buffer)) {
    uint32_t length = DORY_NETWORK_L1_CAPACITY - offset;
    if (length > sizeof(buffer)) length = sizeof(buffer);
    if (ne16_scratchpad_read(buffer, offset, length) != 0) return -1;
    for (uint32_t index = 0; index < length; ++index)
      if (buffer[index] != 0xa5) return -1;
  }
  return 0;
}

int main(void) {
  uint8_t sentinel[256];
  uint32_t input_ptr, weights_ptr, output_ptr;
  memcpy(input, LAYER_INPUT, sizeof(input));
  memcpy(weights, LAYER_WEIGHTS, sizeof(weights));
  memset(output, 0, sizeof(output));
  memset(sentinel, 0xa5, sizeof(sentinel));
  for (uint32_t offset = 0; offset < DORY_NETWORK_L1_PADDING_GUARD;
       offset += sizeof(sentinel)) {
    uint32_t length = DORY_NETWORK_L1_PADDING_GUARD - offset;
    if (length > sizeof(sentinel)) length = sizeof(sentinel);
    if (ne16_scratchpad_write(offset, sentinel, length) != 0) return 1;
  }
  for (uint32_t offset = DORY_NETWORK_L1_PADDING_GUARD +
                         DORY_NETWORK_L1_LIVE_LIMIT;
       offset < DORY_NETWORK_L1_CAPACITY; offset += sizeof(sentinel)) {
    uint32_t length = DORY_NETWORK_L1_CAPACITY - offset;
    if (length > sizeof(sentinel)) length = sizeof(sentinel);
    if (ne16_scratchpad_write(offset, sentinel, length) != 0) return 1;
  }
  if (pointer32(input, &input_ptr) != 0 ||
      pointer32(weights, &weights_ptr) != 0 ||
      pointer32(output, &output_ptr) != 0) {
    printf("%s L2 buffer does not fit in generated uint32_t ABI\n",
           LAYER_NAME);
    return 1;
  }

  layer_args_t args = {
      .L2_input = input_ptr,
      .L2_weights = weights_ptr,
      .L2_output = output_ptr,
      .L1_buffer = NE16_SCRATCH_BASE + DORY_NETWORK_L1_PADDING_GUARD,
      .padding = NET_UTILS_PAD_TOP | NET_UTILS_PAD_RIGHT |
                 NET_UTILS_PAD_BOTTOM | NET_UTILS_PAD_LEFT,
  };
  ne16_reset();
  dory_ne16_compat_reset_stats();
  LAYER_FUNCTION(&args);
  if (dory_ne16_compat_error() != 0) {
    printf("%s reported adapter error %d\n", LAYER_NAME,
           dory_ne16_compat_error());
    return 1;
  }
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  if (stats.dma_l2_to_l1_count == 0 || stats.dma_l1_to_l2_count == 0 ||
      stats.ne16_dispatch_count != LAYER_TILES ||
      stats.ne16_completion_count != LAYER_TILES) {
    printf("%s unexpected stats: dma l2->l1=%u l1->l2=%u dispatch=%u "
           "completion=%u\n",
           LAYER_NAME, stats.dma_l2_to_l1_count, stats.dma_l1_to_l2_count,
           stats.ne16_dispatch_count, stats.ne16_completion_count);
    return 1;
  }
  if (memcmp(output, LAYER_EXPECTED, sizeof(output)) != 0) {
    printf("%s output mismatch\n", LAYER_NAME);
    return 1;
  }
  if (check_guards() != 0) {
    printf("%s scratchpad guard corruption\n", LAYER_NAME);
    return 1;
  }
  printf("DORY generated NE16 %s PASS: output and guards match\n", LAYER_NAME);
  return 0;
}
