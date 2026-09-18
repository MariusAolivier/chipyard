#include <stdint.h>
#include <stdio.h>
#include "ne16_driver.h"

#define INPUT0_OFFSET 0x0000
#define INPUT1_OFFSET 0x0400
#define WEIGHTS_OFFSET 0x1000
#define OUTPUT0_OFFSET 0x2000
#define OUTPUT1_OFFSET 0x3000
#define INPUT_HEIGHT 5
#define INPUT_WIDTH 5
#define INPUT_CHANNELS 16
#define OUTPUT_HEIGHT 3
#define OUTPUT_WIDTH 3

static int dispatch(uint32_t input, uint32_t weights, uint32_t output) {
  ne16_task_t task = {.words = {
      weights, input, output, 0, 0, 0,
      INPUT_CHANNELS, INPUT_WIDTH * INPUT_CHANNELS, 0,
      32, sizeof(int32_t), OUTPUT_WIDTH * sizeof(int32_t),
      3 * 3 * 2, 3 * 3 * 2 * 8, 0,
      0x00010010, 0x00030003, 0x00050005, 0x00010001,
      0x00010001, 0, 0xffffff80, 0, 0x00408007,
  }};
  return ne16_submit(&task);
}

static int check_output(volatile const uint8_t *input,
                        volatile const int32_t *output, const char *name) {
  int errors = 0;
  for (int oy = 0; oy < OUTPUT_HEIGHT; ++oy) {
    for (int ox = 0; ox < OUTPUT_WIDTH; ++ox) {
      int32_t reference = 0;
      for (int ky = 0; ky < 3; ++ky) {
        for (int kx = 0; kx < 3; ++kx) {
          for (int channel = 0; channel < INPUT_CHANNELS; ++channel) {
            int index =
                ((oy + ky) * INPUT_WIDTH + ox + kx) * INPUT_CHANNELS + channel;
            reference += input[index];
          }
        }
      }

      int output_index = oy * OUTPUT_WIDTH + ox;
      if (output[output_index] != reference) {
        printf("%s[%d,%d]: hardware=%d software=%d\n", name, oy, ox,
               output[output_index], reference);
        ++errors;
      }
    }
  }
  return errors;
}

int main(void) {
  volatile uint8_t *input0 =
      (volatile uint8_t *)(NE16_SCRATCH_BASE + INPUT0_OFFSET);
  volatile uint8_t *input1 =
      (volatile uint8_t *)(NE16_SCRATCH_BASE + INPUT1_OFFSET);
  volatile uint8_t *weights =
      (volatile uint8_t *)(NE16_SCRATCH_BASE + WEIGHTS_OFFSET);
  volatile int32_t *output0 =
      (volatile int32_t *)(NE16_SCRATCH_BASE + OUTPUT0_OFFSET);
  volatile int32_t *output1 =
      (volatile int32_t *)(NE16_SCRATCH_BASE + OUTPUT1_OFFSET);

  for (int i = 0; i < INPUT_HEIGHT * INPUT_WIDTH * INPUT_CHANNELS; ++i) {
    input0[i] = (uint8_t)(1 + i % 13);
    input1[i] = (uint8_t)(2 + i % 7);
  }

  for (int i = 0; i < 3 * 3 * 2 * 8; ++i) {
    weights[i] = 0;
  }
  for (int spatial = 0; spatial < 3 * 3; ++spatial) {
    weights[spatial * 2] = 0xff;
    weights[spatial * 2 + 1] = 0xff;
    weights[(7 * 3 * 3 + spatial) * 2] = 0xff;
    weights[(7 * 3 * 3 + spatial) * 2 + 1] = 0xff;
  }

  for (int i = 0; i < OUTPUT_HEIGHT * OUTPUT_WIDTH; ++i) {
    output0[i] = (int32_t)0xdeadbeef;
    output1[i] = (int32_t)0xdeadbeef;
  }
  ne16_memory_fence();

  ne16_reset();
  if (dispatch(NE16_SCRATCH_BASE + INPUT0_OFFSET,
               NE16_SCRATCH_BASE + WEIGHTS_OFFSET,
               NE16_SCRATCH_BASE + OUTPUT0_OFFSET) ||
      dispatch(NE16_SCRATCH_BASE + INPUT1_OFFSET,
               NE16_SCRATCH_BASE + WEIGHTS_OFFSET,
               NE16_SCRATCH_BASE + OUTPUT1_OFFSET)) {
    return 1;
  }

  if (ne16_wait_idle(10000000) != 0) {
    printf("NE16 3x3 timed out\n");
    return 1;
  }

  int errors = check_output(input0, output0, "operation 0");
  errors += check_output(input1, output1, "operation 1");
  if (errors != 0) {
    printf("NE16 3x3 convolution failed with %d mismatches\n", errors);
    return 1;
  }

  printf("NE16 3x3 convolution PASS: two queued operations match software\n");
  return 0;
}
