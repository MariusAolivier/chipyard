#include <stdint.h>
#include <stdio.h>

#include "ne16_driver.h"
#include "ne16_dory_layer_data.h"

#define INPUT_OFFSET 0x1000u
#define OUTPUT_OFFSET 0x5000u
#define WEIGHTS_OFFSET 0x9000u
#define SCALE_OFFSET 0x9900u
#define BIAS_OFFSET 0x9940u

static int check_descriptor(const ne16_task_t *task) {
  const uint32_t expected[NE16_TASK_WORDS] = {
      NE16_SCRATCH_BASE + WEIGHTS_OFFSET,
      NE16_SCRATCH_BASE + 0x0df0u,
      NE16_SCRATCH_BASE + OUTPUT_OFFSET,
      NE16_SCRATCH_BASE + SCALE_OFFSET,
      0,
      NE16_SCRATCH_BASE + BIAS_OFFSET,
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
      0xffffff80,
      0,
      NE16_DORY_3X3_CONF0,
  };
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

static int check_round_trip(void) {
  uint8_t buffer[256];
  for (size_t offset = 0; offset < NE16_DORY_INPUT_BYTES; offset += sizeof(buffer)) {
    size_t size = NE16_DORY_INPUT_BYTES - offset;
    if (size > sizeof(buffer)) size = sizeof(buffer);
    if (ne16_scratchpad_read(buffer, INPUT_OFFSET + offset, size) != 0) {
      printf("scratchpad input read failed\n");
      return 1;
    }
    for (size_t i = 0; i < size; ++i) {
      if (buffer[i] != ne16_dory_input[offset + i]) {
        printf("scratchpad input mismatch at %zu: actual=%u expected=%u\n", offset + i, buffer[i], ne16_dory_input[offset + i]);
        return 1;
      }
    }
  }
  return 0;
}

static int check_output(void) {
  uint8_t output[256];
  int errors = 0;
  uint32_t actual_sum = 0;
  uint32_t expected_sum = 0;
  for (size_t offset = 0; offset < NE16_DORY_OUTPUT_BYTES; offset += sizeof(output)) {
    size_t size = NE16_DORY_OUTPUT_BYTES - offset;
    if (size > sizeof(output)) size = sizeof(output);
    if (ne16_scratchpad_read(output, OUTPUT_OFFSET + offset, size) != 0) {
      printf("scratchpad output read failed\n");
      return 1;
    }
    for (size_t i = 0; i < size; ++i) {
      actual_sum += output[i];
      expected_sum += ne16_dory_expected[offset + i];
      if (output[i] != ne16_dory_expected[offset + i]) {
        if (errors < 16) printf("output[%zu]: actual=%u expected=%u\n", offset + i, output[i], ne16_dory_expected[offset + i]);
        ++errors;
      }
    }
  }
  if (errors != 0) printf("DORY output failed: %d mismatches, actual_sum=%u expected_sum=%u\n", errors, actual_sum, expected_sum);
  return errors;
}

static int fill_scratchpad(uint32_t offset, uint8_t value, size_t size) {
  uint8_t buffer[256];
  for (size_t i = 0; i < sizeof(buffer); ++i) buffer[i] = value;
  while (size != 0) {
    size_t chunk = size < sizeof(buffer) ? size : sizeof(buffer);
    if (ne16_scratchpad_write(offset, buffer, chunk) != 0) return -1;
    offset += chunk;
    size -= chunk;
  }
  return 0;
}

int main(void) {
  printf("DORY layer init zero begin\n");
  fflush(stdout);
  if (fill_scratchpad(0, 0, 0x1000) != 0) {
    printf("DORY layer init zero failed\n");
    return 1;
  }
  printf("DORY layer init input begin\n");
  fflush(stdout);
  if (ne16_scratchpad_write(INPUT_OFFSET, ne16_dory_input,
                            NE16_DORY_INPUT_BYTES) != 0) {
    printf("DORY layer init input failed\n");
    return 1;
  }
  printf("DORY layer init output begin\n");
  fflush(stdout);
  if (fill_scratchpad(OUTPUT_OFFSET, 0xa5, NE16_DORY_OUTPUT_BYTES) != 0) {
    printf("DORY layer init output failed\n");
    return 1;
  }
  printf("DORY layer init weights begin\n");
  fflush(stdout);
  for (size_t offset = 0; offset < NE16_DORY_WEIGHTS_BYTES; offset += 256u) {
    size_t size = NE16_DORY_WEIGHTS_BYTES - offset;
    if (size > 256u) size = 256u;
    printf("DORY layer init weights chunk 0x%x begin\n", (unsigned)offset);
    fflush(stdout);
    if (ne16_scratchpad_write(WEIGHTS_OFFSET + (uint32_t)offset,
                              ne16_dory_weights + offset, size) != 0) {
      printf("DORY layer init weights chunk 0x%x failed\n", (unsigned)offset);
      return 1;
    }
    printf("DORY layer init weights chunk 0x%x done\n", (unsigned)offset);
    fflush(stdout);
  }
  printf("DORY layer init scale begin\n");
  fflush(stdout);
  if (ne16_scratchpad_write(SCALE_OFFSET, ne16_dory_scale,
                            NE16_DORY_SCALE_BYTES) != 0) {
    printf("DORY layer init scale failed\n");
    return 1;
  }
  printf("DORY layer init bias begin\n");
  fflush(stdout);
  if (ne16_scratchpad_write(BIAS_OFFSET, ne16_dory_bias,
                            NE16_DORY_BIAS_BYTES) != 0) {
    printf("DORY layer init bias failed\n");
    return 1;
  }
  printf("DORY layer init done\n");
  fflush(stdout);

  if (check_round_trip() != 0) {
    return 1;
  }

  ne16_dory_3x3_config_t config = {
      .input = NE16_SCRATCH_BASE + INPUT_OFFSET,
      .weights = NE16_SCRATCH_BASE + WEIGHTS_OFFSET,
      .output = NE16_SCRATCH_BASE + OUTPUT_OFFSET,
      .scale = NE16_SCRATCH_BASE + SCALE_OFFSET,
      .shift = 0,
      .bias = NE16_SCRATCH_BASE + BIAS_OFFSET,
      .input_width = 32,
      .input_channels = 16,
      .output_height = 32,
      .output_width = 32,
      .output_channels = 16,
      .padding_top = 1,
      .padding_right = 1,
      .padding_bottom = 1,
      .padding_left = 1,
      .weight_offset_factor = -128,
  };
  ne16_task_t task;
  if (ne16_build_dory_3x3_task(&task, &config) != 0) {
    printf("DORY task construction failed\n");
    return 1;
  }
  if (check_descriptor(&task) != 0) {
    printf("DORY descriptor validation failed\n");
    return 1;
  }

  ne16_reset();
  if (ne16_submit(&task) != 0) {
    printf("DORY task submission failed\n");
    return 1;
  }
  if (ne16_wait_idle(10000000) != 0) {
    printf("DORY NE16 layer timed out\n");
    return 1;
  }

  int errors = check_output();
  if (errors != 0) {
    printf("DORY NE16 layer FAIL\n");
    return 1;
  }
  printf("DORY NE16 layer PASS: layout and output match DORY reference\n");
  return 0;
}
