#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "BNReluConvolution0.h"
#include "BNReluConvolution1.h"
#include "BNReluConvolution2.h"
#include "dory_generated_network_data.h"
#include "ne16_driver.h"
#include "net_utils.h"
#include "pulp_nnx.h"

_Static_assert(DORY_NETWORK_LAYER_COUNT == 3u,
               "the checked-in harness expects three generated layers");
_Static_assert(DORY_NETWORK_L1_CAPACITY == NE16_SCRATCH_BYTES,
               "generated L1 capacity does not match Chipyard");
_Static_assert(DORY_NETWORK_L1_PADDING_GUARD +
                   DORY_NETWORK_L1_LIVE_LIMIT +
                   DORY_NETWORK_L1_SUFFIX_GUARD <=
                   DORY_NETWORK_L1_CAPACITY,
               "generated guarded L1 allocation exceeds scratchpad");

static uint8_t layer0_input[DORY_LAYER_0_INPUT_BYTES];
static uint8_t layer0_weights[DORY_LAYER_0_PARAMETER_BYTES];
static uint8_t layer0_output[DORY_LAYER_0_OUTPUT_BYTES];
static uint8_t layer1_weights[DORY_LAYER_1_PARAMETER_BYTES];
static uint8_t layer1_output[DORY_LAYER_1_OUTPUT_BYTES];
static uint8_t layer2_weights[DORY_LAYER_2_PARAMETER_BYTES];
static uint8_t layer2_output[DORY_LAYER_2_OUTPUT_BYTES];

typedef void (*generated_layer_fn)(void *);

static int pointer32(const void *pointer, uint32_t *value) {
  uintptr_t address = (uintptr_t)pointer;
  if (address > UINT32_MAX) return -1;
  *value = (uint32_t)address;
  return 0;
}

static int check_range(uint32_t offset, uint32_t length, uint8_t expected) {
  uint8_t buffer[256];
  while (length != 0) {
    uint32_t chunk = length > sizeof(buffer) ? sizeof(buffer) : length;
    if (ne16_scratchpad_read(buffer, offset, chunk) != 0) return -1;
    for (uint32_t index = 0; index < chunk; ++index) {
      if (buffer[index] != expected) {
        printf("scratch sentinel changed at 0x%x: 0x%02x\n",
               offset + index, buffer[index]);
        return -1;
      }
    }
    offset += chunk;
    length -= chunk;
  }
  return 0;
}

static int initialize_guards(void) {
  uint8_t sentinel[256];
  memset(sentinel, 0xa5, sizeof(sentinel));
  for (uint32_t offset = 0;
       offset < DORY_NETWORK_L1_PADDING_GUARD;) {
    uint32_t length = DORY_NETWORK_L1_PADDING_GUARD - offset;
    if (length > sizeof(sentinel)) length = sizeof(sentinel);
    if (ne16_scratchpad_write(offset, sentinel, length) != 0) return -1;
    offset += length;
  }
  for (uint32_t offset = DORY_NETWORK_L1_PADDING_GUARD +
                         DORY_NETWORK_L1_LIVE_LIMIT;
       offset < DORY_NETWORK_L1_CAPACITY;) {
    uint32_t length = DORY_NETWORK_L1_CAPACITY - offset;
    if (length > sizeof(sentinel)) length = sizeof(sentinel);
    if (ne16_scratchpad_write(offset, sentinel, length) != 0) return -1;
    offset += length;
  }
  return 0;
}

static int check_guards(void) {
  return check_range(0, DORY_NETWORK_L1_PADDING_GUARD, 0xa5) == 0 &&
                 check_range(DORY_NETWORK_L1_PADDING_GUARD +
                                 DORY_NETWORK_L1_LIVE_LIMIT,
                             DORY_NETWORK_L1_SUFFIX_GUARD, 0xa5) == 0
             ? 0
             : -1;
}

static int check_output(const char *name, const uint8_t *actual,
                        const uint8_t *expected, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    if (actual[index] != expected[index]) {
      printf("%s output mismatch at %zu: actual=%u expected=%u\n", name,
             index, actual[index], expected[index]);
      return -1;
    }
  }
  return 0;
}

static int run_layer(const char *name, generated_layer_fn function,
                     uint8_t *input, uint8_t *weights, uint8_t *output,
                     const uint8_t *expected, uint32_t input_bytes,
                     uint32_t weights_bytes, uint32_t output_bytes,
                     uint32_t expected_tiles, dory_ne16_stats_t *previous) {
  uint32_t input_ptr, weights_ptr, output_ptr;
  printf("DORY generated network starting %s\n", name);
  fflush(stdout);
  if (pointer32(input, &input_ptr) != 0 ||
      pointer32(weights, &weights_ptr) != 0 ||
      pointer32(output, &output_ptr) != 0) {
    printf("%s L2 buffer does not fit in generated uint32_t ABI\n", name);
    return -1;
  }
  (void)input_bytes;
  (void)weights_bytes;
  memset(output, 0, output_bytes);
  layer_args_t args = {
      .L2_input = input_ptr,
      .L2_weights = weights_ptr,
      .L2_output = output_ptr,
      .L1_buffer = NE16_SCRATCH_BASE + DORY_NETWORK_L1_PADDING_GUARD,
      .padding = NET_UTILS_PAD_TOP | NET_UTILS_PAD_RIGHT |
                 NET_UTILS_PAD_BOTTOM | NET_UTILS_PAD_LEFT,
  };

  if (previous->ne16_dispatch_count != 0) ne16_reset();
  function(&args);
  printf("DORY generated network returned from %s\n", name);
  fflush(stdout);
  if (dory_ne16_compat_error() != 0) {
    printf("%s reported adapter error %d\n", name,
           dory_ne16_compat_error());
    return -1;
  }
  dory_ne16_stats_t current = dory_ne16_compat_stats();
  if (current.dma_l2_to_l1_count <= previous->dma_l2_to_l1_count ||
      current.dma_l1_to_l2_count <= previous->dma_l1_to_l2_count ||
      current.ne16_dispatch_count - previous->ne16_dispatch_count !=
          expected_tiles ||
      current.ne16_completion_count - previous->ne16_completion_count !=
          expected_tiles) {
    printf("%s unexpected stats: dma l2->l1=%u l1->l2=%u dispatch=%u "
           "completion=%u\n",
           name, current.dma_l2_to_l1_count - previous->dma_l2_to_l1_count,
           current.dma_l1_to_l2_count - previous->dma_l1_to_l2_count,
           current.ne16_dispatch_count - previous->ne16_dispatch_count,
           current.ne16_completion_count - previous->ne16_completion_count);
    return -1;
  }
  if (check_output(name, output, expected, output_bytes) != 0 ||
      check_guards() != 0) {
    return -1;
  }
  printf("DORY generated network finished %s\n", name);
  fflush(stdout);
  *previous = current;
  return 0;
}

int main(void) {
  memcpy(layer0_input, dory_BNReluConvolution0_input,
         sizeof(layer0_input));
  memcpy(layer0_weights, dory_BNReluConvolution0_parameters,
         sizeof(layer0_weights));
  memcpy(layer1_weights, dory_BNReluConvolution1_parameters,
         sizeof(layer1_weights));
  memcpy(layer2_weights, dory_BNReluConvolution2_parameters,
         sizeof(layer2_weights));
  if (initialize_guards() != 0) return 1;

  ne16_reset();
  dory_ne16_compat_reset_stats();
  dory_ne16_stats_t previous = {0};
  if (run_layer("layer0", BNReluConvolution0, layer0_input, layer0_weights,
                layer0_output, dory_BNReluConvolution0_reference,
                sizeof(layer0_input), sizeof(layer0_weights),
                sizeof(layer0_output), DORY_LAYER_0_TILE_COUNT,
                &previous) != 0 ||
      run_layer("layer1", BNReluConvolution1, layer0_output, layer1_weights,
                layer1_output, dory_BNReluConvolution1_reference,
                sizeof(layer0_output), sizeof(layer1_weights),
                sizeof(layer1_output), DORY_LAYER_1_TILE_COUNT,
                &previous) != 0 ||
      run_layer("layer2", BNReluConvolution2, layer1_output, layer2_weights,
                layer2_output, dory_BNReluConvolution2_reference,
                sizeof(layer1_output), sizeof(layer2_weights),
                sizeof(layer2_output), DORY_LAYER_2_TILE_COUNT,
                &previous) != 0) {
    return 1;
  }

  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  if (stats.ne16_dispatch_count != DORY_LAYER_0_TILE_COUNT +
                                      DORY_LAYER_1_TILE_COUNT +
                                      DORY_LAYER_2_TILE_COUNT ||
      stats.ne16_completion_count != stats.ne16_dispatch_count) {
    printf("unexpected total dispatch/completion counts: %u/%u\n",
           stats.ne16_dispatch_count, stats.ne16_completion_count);
    return 1;
  }
  printf("DORY generated three-layer NE16 network PASS: 3 layers, all "
         "intermediate and final outputs match\n");
  return 0;
}
