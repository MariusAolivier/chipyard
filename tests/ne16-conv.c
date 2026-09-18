#include <stdint.h>
#include <stdio.h>
#include "ne16_driver.h"

#define INPUT0_OFFSET 0x0000
#define INPUT1_OFFSET 0x0400
#define WEIGHTS_OFFSET 0x1000
#define OUTPUT0_OFFSET 0x2000
#define OUTPUT1_OFFSET 0x3000

static int dispatch(uint32_t input, uint32_t weights, uint32_t output) {
  ne16_task_t task = {.words = {
      weights, input, output, 0, 0, 0,
      16, 48, 0, 32, 4, 12, 16, 16, 0,
      0x00010010, 0x00030003, 0x00030003, 0x00010001,
      0x00010001, 0, 0xffffff80, 0, 0x00408047,
  }};
  return ne16_submit(&task);
}

static int check_output(volatile const uint8_t *input,
                        volatile const int32_t *output, const char *name) {
  int errors = 0;
  for (int pixel = 0; pixel < 9; ++pixel) {
    int32_t reference = 0;
    for (int channel = 0; channel < 16; ++channel) {
      reference += input[pixel * 16 + channel];
    }
    if (output[pixel] != reference) {
      printf("%s[%d]: hardware=%d software=%d\n", name, pixel, output[pixel],
             reference);
      ++errors;
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

  for (int i = 0; i < 9 * 16; ++i) {
    input0[i] = 1;
    input1[i] = 2;
  }
  for (int i = 0; i < 16; ++i) {
    weights[i] = 0;
  }
  weights[0] = 0xff;
  weights[1] = 0xff;
  weights[14] = 0xff;
  weights[15] = 0xff;
  for (int i = 0; i < 9; ++i) {
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
    printf("NE16 timed out\n");
    return 1;
  }

  int errors = check_output(input0, output0, "operation 0");
  errors += check_output(input1, output1, "operation 1");
  if (errors != 0) {
    printf("NE16 convolution failed with %d mismatches\n", errors);
    return 1;
  }

  printf("NE16 convolution PASS: two queued operations match software\n");
  return 0;
}
