#include "ne16_driver.h"

#include <stdio.h>

static inline void ne16_write32(uintptr_t address, uint32_t value) {
  *(volatile uint32_t *)address = value;
}

static inline uint32_t ne16_read32(uintptr_t address) {
  return *(volatile uint32_t *)address;
}

static inline void ne16_write8(uintptr_t address, uint8_t value) {
  *(volatile uint8_t *)address = value;
}

static inline uint8_t ne16_read8(uintptr_t address) {
  return *(volatile uint8_t *)address;
}

static int valid_range(uint32_t offset, size_t size) {
  return offset <= NE16_SCRATCH_BYTES && size <= NE16_SCRATCH_BYTES - offset;
}

void ne16_memory_fence(void) {
  __asm__ volatile("fence rw, rw" ::: "memory");
}

void ne16_reset(void) {
  ne16_write32(NE16_CONTROL_BASE + NE16_SOFT_CLEAR, 0);
  ne16_memory_fence();
}

int ne16_scratchpad_write(uint32_t offset, const void *source, size_t size) {
  if (!valid_range(offset, size) || source == NULL) {
    return -1;
  }

  const uint8_t *src = (const uint8_t *)source;
  uintptr_t address = NE16_SCRATCH_BASE + offset;
  while (size != 0 && (address & 3u) != 0) {
    ne16_write8(address, *src);
    ++src;
    ++address;
    --size;
  }
  while (size >= sizeof(uint32_t)) {
    uint32_t word;
    __builtin_memcpy(&word, src, sizeof(word));
    ne16_write32(address, word);
    src += sizeof(word);
    address += sizeof(word);
    size -= sizeof(word);
  }
  while (size != 0) {
    ne16_write8(address, *src);
    ++src;
    ++address;
    --size;
  }
  ne16_memory_fence();
  return 0;
}

int ne16_scratchpad_read(void *destination, uint32_t offset, size_t size) {
  if (!valid_range(offset, size) || destination == NULL) {
    return -1;
  }

  uint8_t *dst = (uint8_t *)destination;
  uintptr_t address = NE16_SCRATCH_BASE + offset;
  while (size != 0 && (address & 3u) != 0) {
    *dst = ne16_read8(address);
    ++dst;
    ++address;
    --size;
  }
  while (size >= sizeof(uint32_t)) {
    uint32_t word = ne16_read32(address);
    __builtin_memcpy(dst, &word, sizeof(word));
    dst += sizeof(word);
    address += sizeof(word);
    size -= sizeof(word);
  }
  while (size != 0) {
    *dst = ne16_read8(address);
    ++dst;
    ++address;
    --size;
  }
  ne16_memory_fence();
  return 0;
}

int ne16_submit(const ne16_task_t *task) {
  if (task == NULL) {
    return -1;
  }

  uint32_t job = ne16_read32(NE16_CONTROL_BASE + NE16_ACQUIRE);
  if (job >= NE16_STATUS_FULL) {
    return -2;
  }

  for (int i = 0; i < NE16_TASK_WORDS; ++i) {
    ne16_write32(NE16_CONTROL_BASE + NE16_TASK_REGS + 4u * (uint32_t)i,
                 task->words[i]);
  }
  ne16_memory_fence();
  ne16_write32(NE16_CONTROL_BASE + NE16_TRIGGER, 0);
  return 0;
}

int ne16_wait_idle(uint32_t timeout) {
  for (;;) {
    if (ne16_read32(NE16_CONTROL_BASE + NE16_STATUS) == 0) {
      ne16_memory_fence();
      return 0;
    }
    if (timeout == 0) {
      ne16_memory_fence();
      return -1;
    }
    --timeout;
  }
}

static uint32_t join_halves(uint32_t high, uint32_t low) {
  return (high << 16) | (low & 0xffffu);
}

int ne16_build_dory_3x3_task(ne16_task_t *task,
                             const ne16_dory_3x3_config_t *config) {
  if (task == NULL || config == NULL || config->input_width == 0 ||
      config->input_channels == 0 || config->output_height == 0 ||
      config->output_width == 0 || config->output_channels == 0 ||
      config->input_channels > 0xffffu || config->output_channels > 0xffffu ||
      config->padding_top > 2 || config->padding_right > 2 ||
      config->padding_bottom > 2 || config->padding_left > 2) {
    return -1;
  }

  const uint32_t input_pointer =
      config->input -
      (config->padding_top * config->input_width + config->padding_left) *
          config->input_channels;
  const uint32_t num_ki = (config->input_channels + 15u) / 16u;
  const uint32_t num_ko = (config->output_channels + 31u) / 32u;
  const uint32_t num_ho = (config->output_height + 2u) / 3u;
  const uint32_t num_wo = (config->output_width + 2u) / 3u;
  const uint32_t rem_ki = config->input_channels % 16u == 0
                              ? 16u
                              : config->input_channels % 16u;
  const uint32_t rem_ko = config->output_channels % 32u == 0
                              ? 32u
                              : config->output_channels % 32u;
  const uint32_t rem_ho = config->output_height % 3u == 0
                              ? 3u
                              : config->output_height % 3u;
  const uint32_t rem_wo = config->output_width % 3u == 0
                              ? 3u
                              : config->output_width % 3u;
  const uint32_t rem_hi = rem_ho + 2u - config->padding_bottom;
  const uint32_t rem_wi = rem_wo + 2u - config->padding_right;

  *task = (ne16_task_t){
      .words = {
          config->weights,
          input_pointer,
          config->output,
          config->scale,
          config->shift,
          config->bias,
          config->input_channels,
          config->input_channels * config->input_width,
          0,
          32,
          config->output_channels,
          config->output_channels * config->output_width,
          18,
          18u * 8u * num_ki,
          0,
          join_halves(rem_ko, rem_ki),
          join_halves(rem_ho, rem_wo),
          join_halves(rem_hi, rem_wi),
          join_halves(num_ko, num_ki),
          join_halves(num_ho, num_wo),
          (uint32_t)(config->padding_top << 28) |
              (uint32_t)(config->padding_right << 24) |
              (uint32_t)(config->padding_bottom << 20) |
              (uint32_t)(config->padding_left << 16),
          (uint32_t)config->weight_offset_factor,
          0,
          NE16_DORY_3X3_CONF0,
      }};
  return 0;
}
