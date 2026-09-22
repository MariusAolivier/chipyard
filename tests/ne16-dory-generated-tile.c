#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "BNReluConvolution0.h"
#include "dory_generated_tile_data.h"
#include "ne16_driver.h"
#include "net_utils.h"
#include "pulp_nnx.h"

#define DORY_L1_PADDING_GUARD 2048u
#define DORY_L1_REQUIREMENT 4544u
#define DORY_L1_TOTAL (DORY_L1_PADDING_GUARD + DORY_L1_REQUIREMENT)
#define DORY_L1_SUFFIX_GUARD 256u
#define DORY_SCRATCH_SENTINEL 0xa5u

_Static_assert(DORY_L1_TOTAL <= NE16_SCRATCH_BYTES,
               "guarded generated L1 allocation exceeds scratchpad");
_Static_assert(DORY_TILE_INPUT_BYTES == 8u * 8u * 16u,
               "unexpected generated input shape");
_Static_assert(DORY_TILE_OUTPUT_BYTES == 8u * 8u * 16u,
               "unexpected generated output shape");

static uint8_t l2_input[DORY_TILE_INPUT_BYTES];
static uint8_t l2_weights[DORY_TILE_WEIGHTS_BYTES];
static uint8_t l2_output[DORY_TILE_OUTPUT_BYTES];

static int pointer32(const void *pointer, uint32_t *value) {
  uintptr_t address = (uintptr_t)pointer;
  if (address > UINT32_MAX) return -1;
  *value = (uint32_t)address;
  return 0;
}

static int check_sentinel_range(uint32_t offset, uint32_t length) {
  uint8_t buffer[256];
  while (length != 0) {
    uint32_t chunk = length > sizeof(buffer) ? sizeof(buffer) : length;
    if (ne16_scratchpad_read(buffer, offset, chunk) != 0) return -1;
    for (uint32_t index = 0; index < chunk; ++index) {
      if (buffer[index] != DORY_SCRATCH_SENTINEL) {
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

static int check_sentinels(void) {
  return check_sentinel_range(0, DORY_L1_PADDING_GUARD) == 0 &&
         check_sentinel_range(DORY_L1_TOTAL, DORY_L1_SUFFIX_GUARD) == 0
             ? 0
             : -1;
}

int main(void) {
  uint32_t input_ptr, weights_ptr, output_ptr;
  if (pointer32(l2_input, &input_ptr) != 0 ||
      pointer32(l2_weights, &weights_ptr) != 0 ||
      pointer32(l2_output, &output_ptr) != 0) {
    printf("DORY L2 buffers do not fit in generated uint32_t ABI\n");
    return 1;
  }
  memcpy(l2_input, dory_tile_input, sizeof(l2_input));
  memcpy(l2_weights, dory_tile_weights, sizeof(l2_weights));
  memset(l2_output, 0, sizeof(l2_output));

  uint8_t sentinel[256];
  memset(sentinel, DORY_SCRATCH_SENTINEL, sizeof(sentinel));
  for (uint32_t offset = 0; offset < DORY_L1_PADDING_GUARD;
       offset += sizeof(sentinel)) {
    uint32_t length = DORY_L1_PADDING_GUARD - offset;
    if (length > sizeof(sentinel)) length = sizeof(sentinel);
    if (ne16_scratchpad_write(offset, sentinel, length) != 0) return 1;
  }
  for (uint32_t offset = DORY_L1_TOTAL;
       offset < DORY_L1_TOTAL + DORY_L1_SUFFIX_GUARD;
       offset += sizeof(sentinel)) {
    uint32_t length = DORY_L1_TOTAL + DORY_L1_SUFFIX_GUARD - offset;
    if (length > sizeof(sentinel)) length = sizeof(sentinel);
    if (ne16_scratchpad_write(offset, sentinel, length) != 0) return 1;
  }
  // The generated loader overwrites every live L1 region before execution.
  layer_args_t args = {
      .L2_input = input_ptr,
      .L2_weights = weights_ptr,
      .L2_output = output_ptr,
      .L1_buffer = NE16_SCRATCH_BASE + DORY_L1_PADDING_GUARD,
      .padding = NET_UTILS_PAD_TOP | NET_UTILS_PAD_RIGHT |
                 NET_UTILS_PAD_BOTTOM | NET_UTILS_PAD_LEFT,
  };

  ne16_reset();
  dory_ne16_compat_reset_stats();
  BNReluConvolution0(&args);

  if (dory_ne16_compat_error() != 0) {
    printf("generated DORY tile reported adapter error %d\n",
           dory_ne16_compat_error());
    return 1;
  }
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  if (stats.dma_l2_to_l1_count < 4 || stats.dma_l1_to_l2_count != 1 ||
      stats.ne16_dispatch_count != 1 || stats.ne16_completion_count != 1) {
    printf("unexpected DORY stats: dma l2->l1=%u l1->l2=%u dispatch=%u completion=%u\n",
           stats.dma_l2_to_l1_count, stats.dma_l1_to_l2_count,
           stats.ne16_dispatch_count, stats.ne16_completion_count);
    return 1;
  }
  if (memcmp(l2_output, dory_tile_expected, sizeof(l2_output)) != 0) {
    for (size_t index = 0; index < sizeof(l2_output); ++index) {
      if (l2_output[index] != dory_tile_expected[index]) {
        printf("generated DORY output mismatch at %zu: actual=%u expected=%u\n",
               index, l2_output[index], dory_tile_expected[index]);
        for (size_t dump = 0; dump < sizeof(l2_output); ++dump)
          printf("DORY_ACTUAL %zu %u\n", dump, l2_output[dump]);
        break;
      }
    }
    return 1;
  }
  if (check_sentinels() != 0) return 1;

  printf("DORY generated NE16 tile PASS: one NNX task, DMA transfers, and output match\n");
  return 0;
}
