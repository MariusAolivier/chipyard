#include <stdint.h>
#include <stdio.h>

#include "ne16_driver.h"

static int check_words(const ne16_task_t *task, const uint32_t *expected) {
  int errors = 0;
  for (int i = 0; i < NE16_TASK_WORDS; ++i) {
    if (task->words[i] != expected[i]) {
      printf("descriptor[%d]: actual=0x%08x expected=0x%08x\n", i,
             task->words[i], expected[i]);
      ++errors;
    }
  }
  return errors;
}

int main(void) {
  const ne16_task_config_t config = {
      .kernel_shape = 3,
      .depthwise = 0,
      .input_bits = 8,
      .output_bits = 8,
      .weights_bits = 8,
      .stride = 1,
      .weights_offset_mode = NE16_FLAG_WEIGHT_OFFSET_LAYER_WISE,
      .weights_offset_factor = (uint32_t)-128,
      .quant_shift = 11,
      .quant_mode = NE16_QUANT_MODE_8BIT,
      .quant_function = NE16_FLAG_QUANT_FUNCTION_RELU,
      .quant_rounding = 0,
      .norm_mode = NE16_NORM_MODE_32BIT,
      .norm_bias = 1,
      .norm_shift = 0,
  };
  ne16_task_t task;
  if (ne16_task_init(&task, &config) != 0 ||
      ne16_task_set_ptrs(&task, NE16_SCRATCH_BASE + 0x1000u, 32, 16, 8, 1, 1,
                         NE16_SCRATCH_BASE + 0x5000u,
                         NE16_SCRATCH_BASE + 0x9000u,
                         NE16_SCRATCH_BASE + 0x9900u, 0,
                         NE16_SCRATCH_BASE + 0x9940u) != 0 ||
      ne16_task_set_dims(&task, 32, 16, 32, 16, 32, 32, 16, 32, 16, 1, 1,
                         1, 1) != 0) {
    printf("generic descriptor construction failed\n");
    return 1;
  }

  const uint32_t expected[NE16_TASK_WORDS] = {
      NE16_SCRATCH_BASE + 0x9000u,
      NE16_SCRATCH_BASE + 0x0df0u,
      NE16_SCRATCH_BASE + 0x5000u,
      NE16_SCRATCH_BASE + 0x9900u,
      0,
      NE16_SCRATCH_BASE + 0x9940u,
      16,
      512,
      0,
      32,
      16,
      512,
      18,
      144,
      0,
      0x00100010,
      0x00020002,
      0x00030003,
      0x00010001,
      0x000b000b,
      0x11110000,
      0xffffff80u,
      0,
      NE16_DORY_3X3_CONF0,
  };
  if (check_words(&task, expected) != 0) {
    return 1;
  }

  ne16_task_config_t invalid = config;
  invalid.stride = 2;
  if (ne16_task_init(&task, &invalid) == 0 ||
      ne16_task_set_dims_stride2x2(&task) == 0 ||
      ne16_task_set_padding(&task, 3, 0, 0, 0, 0) == 0) {
    printf("invalid descriptor input was accepted\n");
    return 1;
  }
  printf("NE16 descriptor PASS\n");
  return 0;
}
