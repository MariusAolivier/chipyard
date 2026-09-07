#include <stdint.h>
#include <stdio.h>

#define ITA_CONTROL_BASE 0x10030000UL
#define ITA_SCRATCH_BASE 0x20000000UL

#define INPUT0_OFFSET 0x0000
#define WEIGHT_OFFSET 0x1000
#define OUTPUT0_OFFSET 0x2000
#define INPUT1_OFFSET 0x3000
#define OUTPUT1_OFFSET 0x4000

static inline void write32(uintptr_t address, uint32_t value) {
  *(volatile uint32_t *)address = value;
}

static inline uint32_t read32(uintptr_t address) {
  return *(volatile uint32_t *)address;
}

static inline void memory_fence(void) {
  __asm__ volatile("fence rw, rw" ::: "memory");
}

static int dispatch_linear(uint32_t input, uint32_t output,
                           uint8_t right_shift) {
  uint32_t acquired = read32(ITA_CONTROL_BASE + 0x04);
  if ((int32_t)acquired < 0) {
    printf("ITA context acquisition failed: 0x%x\n", acquired);
    return 1;
  }

  write32(ITA_CONTROL_BASE + 0x20 + 4 * 0, input);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 1,
          ITA_SCRATCH_BASE + WEIGHT_OFFSET);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 2,
          ITA_SCRATCH_BASE + WEIGHT_OFFSET);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 3, 0);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 4, output);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 5, 64);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 6, 0x00001111);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 7, 0x01010101);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 8, 0x01010101);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 9, 0x01010101 * right_shift);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 10, 0x01010101 * right_shift);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 11, 0);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 12, 0);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 13, 2);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 14, 5);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 15, 0);
  write32(ITA_CONTROL_BASE + 0x20 + 4 * 16, 0);
  write32(ITA_CONTROL_BASE + 0x00, 0);
  return 0;
}

static int check_output(volatile const uint8_t *output, int period,
                        int scale, const char *name) {
  int errors = 0;
  for (int row = 0; row < 64; ++row) {
    uint8_t expected = (uint8_t)(scale * (1 + row % period));
    for (int column = 0; column < 64; ++column) {
      int index = row * 64 + column;
      if (output[index] != expected) {
        printf("%s[%d,%d]: hardware=%u software=%u\n", name, row, column,
               output[index], expected);
        ++errors;
      }
    }
  }
  return errors;
}

int main(void) {
  volatile uint8_t *input0 =
      (volatile uint8_t *)(ITA_SCRATCH_BASE + INPUT0_OFFSET);
  volatile uint8_t *weight =
      (volatile uint8_t *)(ITA_SCRATCH_BASE + WEIGHT_OFFSET);
  volatile uint8_t *output0 =
      (volatile uint8_t *)(ITA_SCRATCH_BASE + OUTPUT0_OFFSET);
  volatile uint8_t *input1 =
      (volatile uint8_t *)(ITA_SCRATCH_BASE + INPUT1_OFFSET);
  volatile uint8_t *output1 =
      (volatile uint8_t *)(ITA_SCRATCH_BASE + OUTPUT1_OFFSET);

  for (int row = 0; row < 64; ++row) {
    for (int column = 0; column < 64; ++column) {
      int index = row * 64 + column;
      input0[index] = (uint8_t)(1 + row % 3);
      input1[index] = (uint8_t)(1 + row % 5);
      weight[index] = 1;
      output0[index] = 0xde;
      output1[index] = 0xad;
    }
  }
  memory_fence();

  write32(ITA_CONTROL_BASE + 0x14, 0);
  if (dispatch_linear(ITA_SCRATCH_BASE + INPUT0_OFFSET,
                      ITA_SCRATCH_BASE + OUTPUT0_OFFSET, 2) ||
      dispatch_linear(ITA_SCRATCH_BASE + INPUT1_OFFSET,
                      ITA_SCRATCH_BASE + OUTPUT1_OFFSET, 3)) {
    return 1;
  }

  uint32_t status = 0xffffffff;
  uint32_t timeout = 10000000;
  while (status != 0 && --timeout != 0) {
    status = read32(ITA_CONTROL_BASE + 0x0c);
  }
  if (timeout == 0) {
    printf("ITA timed out with status 0x%x\n", status);
    return 1;
  }
  memory_fence();

  int errors = check_output(output0, 3, 16, "operation 0");
  errors += check_output(output1, 5, 8, "operation 1");
  if (errors != 0) {
    printf("ITA linear regression failed with %d mismatches\n", errors);
    return 1;
  }

  printf("ITA linear PASS: two queued 64x64 operations match software\n");
  return 0;
}
