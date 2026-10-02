#ifndef NE16_TEST_SCRATCH_H
#define NE16_TEST_SCRATCH_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ne16_driver.h"

_Static_assert(NE16_SCRATCH_BASE % sizeof(uint64_t) == 0 &&
               NE16_SCRATCH_BYTES % sizeof(uint64_t) == 0,
               "scratch guard I/O requires aligned whole words");

static inline void ne16_test_fill_scratch(uint8_t value) {
  volatile uint64_t *scratch = (volatile uint64_t *)(uintptr_t)NE16_SCRATCH_BASE;
  uint64_t word = UINT64_C(0x0101010101010101) * value;
  for (uint32_t i = 0; i < NE16_SCRATCH_BYTES / sizeof(word); ++i)
    scratch[i] = word;
  ne16_memory_fence();
}

static inline int ne16_test_check_scratch(const uint8_t *expected) {
  const volatile uint64_t *scratch =
      (const volatile uint64_t *)(uintptr_t)NE16_SCRATCH_BASE;
  ne16_memory_fence();
  for (uint32_t i = 0; i < NE16_SCRATCH_BYTES; i += sizeof(uint64_t)) {
    uint64_t word = scratch[i / sizeof(word)];
    uint64_t reference;
    memcpy(&reference, expected + i, sizeof(reference));
    if (word == reference) continue;
    for (uint32_t byte = 0; byte < sizeof(word); ++byte) {
      uint8_t actual = (uint8_t)(word >> (byte * 8u));
      if (actual != expected[i + byte]) {
        printf("NE16 scratch data/guard mismatch at 0x%x: %u expected %u\n",
               i + byte, actual, expected[i + byte]);
        return -1;
      }
    }
  }
  ne16_memory_fence();
  return 0;
}

#endif
