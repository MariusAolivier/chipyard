#include <stdint.h>
#include <stdio.h>

#define NE16_CONTROL_BASE 0x10030000UL
#define NE16_SCRATCH_BASE 0x20000000UL

#define NE16_TRIGGER 0x00
#define NE16_ACQUIRE 0x04
#define NE16_STATUS 0x0c
#define NE16_SOFT_CLEAR 0x14
#define NE16_TASK_REGS 0x20

#define INPUT0_OFFSET 0x0000
#define INPUT1_OFFSET 0x0400
#define WEIGHTS_OFFSET 0x1000
#define OUTPUT0_OFFSET 0x2000
#define OUTPUT1_OFFSET 0x3000

static inline void write32(uintptr_t address, uint32_t value) {
  *(volatile uint32_t *)address = value;
}

static inline uint32_t read32(uintptr_t address) {
  return *(volatile uint32_t *)address;
}

static inline void memory_fence(void) {
  __asm__ volatile("fence rw, rw" ::: "memory");
}

static int dispatch(uint32_t input, uint32_t weights, uint32_t output) {
  uint32_t task[24] = {
      weights,
      input,
      output,
      0,
      0,
      0,
      16,
      48,
      0,
      32,
      4,
      12,
      16,
      16,
      0,
      0x00010010,
      0x00030003,
      0x00030003,
      0x00010001,
      0x00010001,
      0,
      0xffffff80,
      0,
      0x00408047,
  };

  uint32_t job = read32(NE16_CONTROL_BASE + NE16_ACQUIRE);
  if (job >= 0x100) {
    printf("NE16 acquire failed: 0x%x\n", job);
    return 1;
  }

  for (int i = 0; i < 24; ++i) {
    write32(NE16_CONTROL_BASE + NE16_TASK_REGS + 4 * i, task[i]);
  }
  write32(NE16_CONTROL_BASE + NE16_TRIGGER, 0);
  return 0;
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
  memory_fence();

  write32(NE16_CONTROL_BASE + NE16_SOFT_CLEAR, 0);
  if (dispatch(NE16_SCRATCH_BASE + INPUT0_OFFSET,
               NE16_SCRATCH_BASE + WEIGHTS_OFFSET,
               NE16_SCRATCH_BASE + OUTPUT0_OFFSET) ||
      dispatch(NE16_SCRATCH_BASE + INPUT1_OFFSET,
               NE16_SCRATCH_BASE + WEIGHTS_OFFSET,
               NE16_SCRATCH_BASE + OUTPUT1_OFFSET)) {
    return 1;
  }

  uint32_t timeout = 10000000;
  while (read32(NE16_CONTROL_BASE + NE16_STATUS) != 0 && --timeout != 0) {
  }
  if (timeout == 0) {
    printf("NE16 timed out with status 0x%x\n",
           read32(NE16_CONTROL_BASE + NE16_STATUS));
    return 1;
  }
  memory_fence();

  int errors = check_output(input0, output0, "operation 0");
  errors += check_output(input1, output1, "operation 1");
  if (errors != 0) {
    printf("NE16 convolution failed with %d mismatches\n", errors);
    return 1;
  }

  printf("NE16 convolution PASS: two queued operations match software\n");
  return 0;
}
