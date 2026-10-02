/* Invoke the real helpers at their fixed MMIO address in private host memory.
 * Only the memory fence and the helper's diagnostic printf are mocked. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#error "Linux scratch host validation requires MAP_FIXED_NOREPLACE"
#endif
#else
#error "Fixed-address scratch host validation supports Windows and Linux"
#endif

#define CHECK(test) do { \
  if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
    exit(EXIT_FAILURE); \
  } \
} while (0)

static int helper_printf(const char *format, ...);
#define printf helper_printf
#include "ne16_test_scratch.h"
#undef printf

#define WORDS (NE16_SCRATCH_BYTES / sizeof(uint64_t))
#define EXPECTED_GUARD 0x6du

static uint64_t aligned_storage[WORDS];
static uint64_t unaligned_storage[WORDS + 2u];
static uint64_t fences, diagnostics, checks, fills;
static unsigned int diagnosed_offset, diagnosed_actual, diagnosed_expected;
static void *mapping;

void ne16_memory_fence(void) { ++fences; }

static int helper_printf(const char *format, ...) {
  CHECK(strcmp(format,
        "NE16 scratch data/guard mismatch at 0x%x: %u expected %u\n") == 0);
  va_list arguments;
  va_start(arguments, format);
  diagnosed_offset = va_arg(arguments, unsigned int);
  int actual = va_arg(arguments, int);
  int expected = va_arg(arguments, int);
  va_end(arguments);
  CHECK(actual >= 0 && actual <= 255 && expected >= 0 && expected <= 255);
  diagnosed_actual = (unsigned int)actual;
  diagnosed_expected = (unsigned int)expected;
  ++diagnostics;
  return 0;
}

static int map_scratch(void) {
  void *address = (void *)(uintptr_t)NE16_SCRATCH_BASE;
#if defined(_WIN32)
  mapping = VirtualAlloc(address, NE16_SCRATCH_BYTES,
                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (mapping == NULL) {
    fprintf(stderr, "VirtualAlloc at 0x%" PRIxPTR " failed: error=%lu\n",
            (uintptr_t)address, (unsigned long)GetLastError());
    return -1;
  }
  if (mapping != address) {
    fprintf(stderr, "VirtualAlloc returned %p instead of %p\n", mapping, address);
    CHECK(VirtualFree(mapping, 0, MEM_RELEASE) != 0);
    return -1;
  }
#else
  mapping = mmap(address, NE16_SCRATCH_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (mapping == MAP_FAILED) {
    fprintf(stderr, "mmap at 0x%" PRIxPTR " failed: %s\n",
            (uintptr_t)address, strerror(errno));
    return -1;
  }
  /* Older kernels can ignore an unknown flag; never accept relocation or use
   * MAP_FIXED, which could overwrite an existing mapping. */
  if (mapping != address) {
    fprintf(stderr, "mmap returned %p instead of %p\n", mapping, address);
    CHECK(munmap(mapping, NE16_SCRATCH_BYTES) == 0);
    return -1;
  }
#endif
  return 0;
}

static int unmap_scratch(void) {
#if defined(_WIN32)
  if (!VirtualFree(mapping, 0, MEM_RELEASE)) {
    fprintf(stderr, "VirtualFree failed: error=%lu\n",
            (unsigned long)GetLastError());
    return -1;
  }
#else
  if (munmap(mapping, NE16_SCRATCH_BYTES) != 0) {
    fprintf(stderr, "munmap failed: %s\n", strerror(errno));
    return -1;
  }
#endif
  return 0;
}

static void check_success(const uint8_t *expected) {
  uint64_t previous_diagnostics = diagnostics, previous_fences = fences;
  CHECK(ne16_test_check_scratch(expected) == 0);
  CHECK(diagnostics == previous_diagnostics && fences > previous_fences);
  ++checks;
}

static void check_failure(const uint8_t *expected, uint32_t offset,
                           uint8_t actual) {
  uint64_t previous_diagnostics = diagnostics, previous_fences = fences;
  CHECK(ne16_test_check_scratch(expected) == -1);
  CHECK(diagnostics == previous_diagnostics + 1u && fences > previous_fences);
  CHECK(diagnosed_offset == offset);
  CHECK(diagnosed_actual == actual && diagnosed_expected == expected[offset]);
  ++checks;
}

static void check_expected_guards(void) {
  const uint8_t *bytes = (const uint8_t *)unaligned_storage;
  CHECK(bytes[0] == EXPECTED_GUARD);
  for (size_t i = 1u + NE16_SCRATCH_BYTES; i < sizeof(unaligned_storage); ++i)
    CHECK(bytes[i] == EXPECTED_GUARD);
}

int main(void) {
  CHECK(NE16_SCRATCH_BASE == UINT32_C(0x20000000));
  CHECK(NE16_SCRATCH_BYTES == 65536u && sizeof(uint64_t) == 8u);
  if (map_scratch() != 0) return EXIT_FAILURE;
  volatile uint8_t *scratch = (volatile uint8_t *)mapping;
  volatile uint64_t *scratch_words = (volatile uint64_t *)mapping;
  uint8_t *aligned = (uint8_t *)aligned_storage;
  uint8_t *expected = (uint8_t *)unaligned_storage + 1u;
  CHECK((uintptr_t)aligned % 8u == 0 && (uintptr_t)expected % 8u == 1u);
  memset(unaligned_storage, EXPECTED_GUARD, sizeof(unaligned_storage));
  printf("scratch host: mapped 65536 bytes at 0x%" PRIxPTR
         "; testing all fill values and every single-byte fault\n",
         (uintptr_t)mapping);
  fflush(stdout);

  for (unsigned int value = 0; value < 256; ++value) {
    memset(aligned, (int)value, NE16_SCRATCH_BYTES);
    memset(expected, (int)value, NE16_SCRATCH_BYTES);
    uint64_t previous_fences = fences;
    ne16_test_fill_scratch((uint8_t)value);
    CHECK(fences > previous_fences);
    ++fills;
    for (uint32_t offset = 0; offset < NE16_SCRATCH_BYTES; ++offset)
      CHECK(scratch[offset] == value);
    check_success(aligned);
    check_success(expected);
    check_expected_guards();
  }

  for (uint32_t offset = 0; offset < NE16_SCRATCH_BYTES; ++offset)
    expected[offset] = (uint8_t)(offset * 37u + (offset / 8u) * 11u + 3u);
  memcpy(aligned, expected, NE16_SCRATCH_BYTES);
  for (uint32_t word = 0; word < WORDS; ++word)
    scratch_words[word] = aligned_storage[word];
  check_success(aligned);
  check_success(expected);

  uint32_t faults = 0;
  for (uint32_t offset = 0; offset < NE16_SCRATCH_BYTES; ++offset) {
    uint8_t corrupt = expected[offset] ^ 0x80u;
    scratch[offset] = corrupt;
    check_failure(expected, offset, corrupt);
    if (offset > 0) CHECK(scratch[offset - 1u] == expected[offset - 1u]);
    if (offset + 1u < NE16_SCRATCH_BYTES)
      CHECK(scratch[offset + 1u] == expected[offset + 1u]);
    uint32_t word = offset / 8u;
    if (word > 0) CHECK(scratch_words[word - 1u] == aligned_storage[word - 1u]);
    if (word + 1u < WORDS)
      CHECK(scratch_words[word + 1u] == aligned_storage[word + 1u]);
    scratch[offset] = expected[offset];
    check_success(expected);
    ++faults;
  }
  CHECK(faults == 65536u && diagnostics == 65536u);

  const uint32_t boundaries[][2] = {
      {0, 1}, {7, 8}, {15, 16}, {65527, 65528}, {65534, 65535}};
  for (size_t pair = 0; pair < sizeof(boundaries) / sizeof(boundaries[0]); ++pair) {
    uint32_t first = boundaries[pair][0], second = boundaries[pair][1];
    uint8_t first_fault = expected[first] ^ 0x55u;
    uint8_t second_fault = expected[second] ^ 0xaau;
    scratch[first] = first_fault;
    scratch[second] = second_fault;
    check_failure(expected, first, first_fault);
    scratch[first] = expected[first];
    check_failure(expected, second, second_fault);
    scratch[second] = expected[second];
    check_success(expected);
  }
  for (uint32_t offset = 0; offset < NE16_SCRATCH_BYTES; ++offset)
    CHECK(scratch[offset] == expected[offset] && aligned[offset] == expected[offset]);
  check_success(aligned);
  check_success(expected);
  check_expected_guards();
  CHECK(fills == 256u && diagnostics == 65546u && checks == 131603u);
  if (unmap_scratch() != 0) return EXIT_FAILURE;
  printf("NE16 scratch helper host PASS: 256 fills, %" PRIu32
         "/65536 individual byte faults and restorations, 5 adjacent-boundary "
         "pairs, aligned/unaligned expected buffers, %" PRIu64
         " real checker calls (diagnostic output suppressed)\n", faults, checks);
  return EXIT_SUCCESS;
}
