#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ne16_dma.h"
#include "ne16_driver.h"

#define TRANSFER_BYTES 16u
#define REGION_BYTES 64u
#define SCRATCH_OFFSET 0x200u
#define PAYLOAD_OFFSET 16u
#define MAX_POLLS 16384u
#define SENTINEL 0xa5u

static uint8_t source[TRANSFER_BYTES] __attribute__((aligned(64)));
static uint8_t expected[REGION_BYTES];
static uint8_t observed[REGION_BYTES];

static int counters(uint32_t *bytes, uint32_t *grants) {
  if (ne16_dma_counter_read(NE16_DMA_BYTES_COPIED, bytes) != 0 ||
      ne16_dma_counter_read(NE16_DMA_DMA_GRANTS, grants) != 0) {
    printf("NE16 DMA smoke FAIL: counter read\n");
    return -1;
  }
  return 0;
}

int main(void) {
  if (setvbuf(stdout, NULL, _IONBF, 0) != 0) {
    printf("NE16 DMA smoke FAIL: unbuffered stdout setup\n");
    return 1;
  }
  printf("NE16 DMA smoke checkpoint: entered main; one 16-byte load only\n");

  uintptr_t address = (uintptr_t)source;
  if (address < UINT32_C(0x80000000) ||
      address > UINT32_MAX - (TRANSFER_BYTES - 1u)) {
    printf("NE16 DMA smoke FAIL: source outside 32-bit DRAM\n");
    return 1;
  }
  for (uint32_t i = 0; i < TRANSFER_BYTES; ++i)
    source[i] = (uint8_t)(0x31u + 7u * i);
  ne16_dma_config_t config = {
      .src = (uint32_t)address,
      .dst = NE16_SCRATCH_BASE + SCRATCH_OFFSET + PAYLOAD_OFFSET,
      .line_bytes = TRANSFER_BYTES, .rows = 1, .planes = 1};
  if (ne16_dma_validate(&config) != 0) {
    printf("NE16 DMA smoke FAIL: invalid descriptor\n");
    return 1;
  }

  printf("NE16 DMA smoke checkpoint: before initial status MMIO\n");
  uint32_t status = ne16_dma_status();
  printf("NE16 DMA smoke initial status=0x%x\n", status);
  if (status != 0) {
    printf("NE16 DMA smoke FAIL: engine not freshly idle; no START or ACK\n");
    return 1;
  }

  memset(expected, SENTINEL, sizeof(expected));
  printf("NE16 DMA smoke checkpoint: before scratch guard initialization\n");
  if (ne16_scratchpad_write(SCRATCH_OFFSET, expected, sizeof(expected)) != 0 ||
      ne16_scratchpad_read(observed, SCRATCH_OFFSET, sizeof(observed)) != 0 ||
      memcmp(observed, expected, sizeof(expected)) != 0) {
    printf("NE16 DMA smoke FAIL: scratch initialization/readback\n");
    return 1;
  }
  uint32_t bytes, grants_before;
  if (counters(&bytes, &grants_before) != 0) return 1;
  printf("NE16 DMA smoke checkpoint: before submit src=0x%x dst=0x%x "
         "bytes=%u grants=%u\n",
         config.src, config.dst, TRANSFER_BYTES, grants_before);
  int result = ne16_dma_submit(&config);
  printf("NE16 DMA smoke checkpoint: submit returned %d\n", result);
  if (result != 0) {
    printf("NE16 DMA smoke FAIL: submit; no retry\n");
    return 1;
  }

  uint32_t polls = 0;
  while (polls < MAX_POLLS) {
    ++polls;
    result = ne16_dma_poll();
    if (result != 0) break;
    if (polls == 1 || polls == 1024 || polls == 4096)
      printf("NE16 DMA smoke checkpoint: busy after %u polls\n", polls);
  }
  if (result == 0) result = NE16_DMA_ETIMEOUT;
  printf("NE16 DMA smoke checkpoint: poll result=%d polls=%u; "
         "before final status MMIO\n", result, polls);
  status = ne16_dma_status();
  uint32_t error = ne16_dma_error();
  uint32_t grants_after;
  if (counters(&bytes, &grants_after) != 0) return 1;
  printf("NE16 DMA smoke final status=0x%x error=%u bytes=%u grants=%u "
         "delta=%u\n", status, error, bytes, grants_after,
         grants_after - grants_before);
  if (result != 1 || status != NE16_DMA_DONE || error != 0 ||
      bytes != TRANSFER_BYTES || grants_after == grants_before) {
    printf("NE16 DMA smoke FAIL: incomplete/error transfer; no ACK, reset, "
           "retry or source-buffer reuse; engine may remain live\n");
    return 1;
  }

  printf("NE16 DMA smoke checkpoint: before payload/guard readback\n");
  memcpy(expected + PAYLOAD_OFFSET, source, sizeof(source));
  if (ne16_scratchpad_read(observed, SCRATCH_OFFSET, sizeof(observed)) != 0) {
    printf("NE16 DMA smoke FAIL: scratch readback\n");
    return 1;
  }
  for (uint32_t i = 0; i < REGION_BYTES; ++i) {
    if (observed[i] != expected[i]) {
      printf("NE16 DMA smoke FAIL: byte %u got=%u expected=%u\n",
             i, observed[i], expected[i]);
      return 1;
    }
  }
  printf("NE16 DMA smoke PASS: one 16-byte load, exact data and 48 guard bytes\n");
  return 0;
}
