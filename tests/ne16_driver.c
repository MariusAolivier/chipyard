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

uint32_t ne16_status(void) {
  return ne16_read32(NE16_CONTROL_BASE + NE16_STATUS);
}

int ne16_queue_full(void) {
  return ne16_read32(NE16_CONTROL_BASE + NE16_ACQUIRE) >= NE16_STATUS_FULL;
}

int ne16_idle(void) {
  return ne16_status() == 0;
}

void ne16_memory_fence(void) {
  __asm__ volatile("fence rw, rw" ::: "memory");
}

void ne16_reset(void) {
  ne16_write32(NE16_CONTROL_BASE + NE16_SOFT_CLEAR, 0);
  ne16_memory_fence();
  for (uint32_t settle = 0; settle < 4096u; ++settle) {
    (void)ne16_status();
  }
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

static uint32_t join_halves(uint32_t high, uint32_t low) {
  return (high << 16) | (low & 0xffffu);
}

static uint32_t ceil_div(uint32_t value, uint32_t divisor) {
  return (value + divisor - 1u) / divisor;
}

static uint32_t tile_remainder(uint32_t value, uint32_t divisor) {
  uint32_t remainder = value % divisor;
  return remainder == 0 ? divisor : remainder;
}

int ne16_task_init(ne16_task_t *task, const ne16_task_config_t *config) {
  if (task == NULL || config == NULL || config->kernel_shape == 0 ||
      config->kernel_shape > 3 || config->input_bits == 0 ||
      config->output_bits == 0 || config->weights_bits == 0 ||
      config->stride == 0 || config->stride > 1 ||
      config->quant_shift > 31 || config->weights_bits > 16) {
    return -1;
  }

  const uint32_t mode = config->kernel_shape == 1
                            ? (2u << 5)
                            : config->depthwise ? (1u << 5) : (0u << 5);
  const uint32_t bit_mode = config->input_bits == 16 ? NE16_FLAG_MODE16 :
                            config->input_bits == 8 ? NE16_FLAG_MODE_BASIC :
                                                        UINT32_MAX;
  if (bit_mode == UINT32_MAX || config->output_bits % 8 != 0) {
    return -1;
  }

  *task = (ne16_task_t){0};
  task->words[23] = NE16_FLAG_NORM_QUANT | config->quant_function |
                     config->quant_mode | (config->quant_shift << 16) |
                     (config->quant_rounding << 11) | config->norm_mode |
                     ((uint32_t)config->norm_bias << 25) |
                     ((uint32_t)config->norm_shift << 24) |
                     config->weights_offset_mode | mode | bit_mode |
                     (uint32_t)(config->weights_bits - 1u);
  task->words[21] = config->weights_offset_factor;
  return 0;
}

int ne16_task_set_ptrs(ne16_task_t *task, uint32_t input_ptr,
                       uint32_t input_width, uint32_t input_channels,
                       uint8_t input_bits, uint8_t padding_top,
                       uint8_t padding_left, uint32_t output_ptr,
                       uint32_t weights_ptr, uint32_t scale_ptr,
                       uint32_t shift_ptr, uint32_t bias_ptr) {
  if (task == NULL || input_width == 0 || input_channels == 0 ||
      input_bits == 0 || (input_bits % 8) != 0 || padding_top > 2 ||
      padding_left > 2) {
    return -1;
  }
  uint64_t adjustment =
      ((uint64_t)padding_top * input_width + padding_left) * input_channels *
      (input_bits / 8u);
  if (adjustment > UINT32_MAX || input_ptr < adjustment) {
    return -1;
  }
  task->words[0] = weights_ptr;
  task->words[1] = input_ptr - (uint32_t)adjustment;
  task->words[2] = output_ptr;
  task->words[3] = scale_ptr;
  task->words[4] = shift_ptr;
  task->words[5] = bias_ptr;
  return 0;
}

int ne16_task_set_dims(ne16_task_t *task, uint32_t input_width,
                       uint32_t input_channels, uint32_t input_width_stride,
                       uint32_t input_channel_stride, uint32_t output_height,
                       uint32_t output_width, uint32_t output_channels,
                       uint32_t output_width_stride,
                       uint32_t output_channel_stride, uint8_t padding_top,
                       uint8_t padding_bottom, uint8_t padding_right,
                       uint8_t padding_left) {
  if (task == NULL || input_width == 0 || input_channels == 0 ||
      output_height == 0 || output_width == 0 || output_channels == 0 ||
      padding_top > 2 || padding_bottom > 2 || padding_right > 2 ||
      padding_left > 2) {
    return -1;
  }
  const uint32_t num_ki = ceil_div(input_channels, 16);
  const uint32_t output_bytes = (task->words[23] & (1u << 21)) ? 2u :
                                (task->words[23] & (2u << 21)) ? 4u : 1u;
  const uint32_t kernel_shape = (task->words[23] >> 5) & 3u;
  const uint32_t depthwise = kernel_shape == 1;

  task->words[6] = input_channel_stride;
  task->words[7] = input_channel_stride * input_width_stride;
  task->words[8] = depthwise ? input_channel_stride * 25 : 0;
  task->words[9] = 32;
  task->words[10] = output_channel_stride * output_bytes;
  task->words[11] = output_channel_stride * output_width_stride * output_bytes;
  task->words[12] = kernel_shape == 2 ? 2 : 18;
  task->words[13] = task->words[12] * 8u * num_ki;
  task->words[14] = 0;

  const uint32_t output_throughput = 32;
  const uint32_t rem_ki = tile_remainder(input_channels, 16);
  const uint32_t rem_ko = tile_remainder(output_channels, output_throughput);
  const uint32_t rem_ho = tile_remainder(output_height, 3);
  const uint32_t rem_wo = tile_remainder(output_width, 3);
  const uint32_t rem_hi = (kernel_shape == 2 ? rem_ho : rem_ho + 2) -
                          padding_bottom;
  const uint32_t rem_wi = (kernel_shape == 2 ? rem_wo : rem_wo + 2) -
                          padding_right;
  task->words[15] = join_halves(rem_ko, rem_ki);
  task->words[16] = join_halves(rem_ho, rem_wo);
  task->words[17] = join_halves(rem_hi, rem_wi);
  task->words[18] = join_halves(ceil_div(output_channels, output_throughput),
                                num_ki);
  task->words[19] = join_halves(ceil_div(output_height, 3),
                                ceil_div(output_width, 3));
  return ne16_task_set_padding(task, padding_top, padding_bottom, padding_left,
                               padding_right, 0);
}

int ne16_task_set_dims_stride2x2(ne16_task_t *task) {
  (void)task;
  return -2;
}

int ne16_task_set_padding(ne16_task_t *task, uint8_t top, uint8_t bottom,
                          uint8_t left, uint8_t right, uint8_t value) {
  if (task == NULL || top > 2 || bottom > 2 || left > 2 || right > 2) {
    return -1;
  }
  task->words[20] = ((uint32_t)(top & 0xf) << 28) |
                    ((uint32_t)(right & 0xf) << 24) |
                    ((uint32_t)(bottom & 0xf) << 20) |
                    ((uint32_t)(left & 0xf) << 16) | value;
  return 0;
}

int ne16_submit(const ne16_task_t *task) {
  if (task == NULL || ne16_queue_full()) {
    return task == NULL ? -1 : -2;
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
    if (ne16_idle()) {
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

  const ne16_task_config_t task_config = {
      .kernel_shape = 3,
      .depthwise = 0,
      .input_bits = 8,
      .output_bits = 8,
      .weights_bits = 8,
      .stride = 1,
      .weights_offset_mode = NE16_FLAG_WEIGHT_OFFSET_LAYER_WISE,
      .weights_offset_factor = (uint32_t)config->weight_offset_factor,
      .quant_shift = 11,
      .quant_mode = NE16_QUANT_MODE_8BIT,
      .quant_function = NE16_FLAG_QUANT_FUNCTION_RELU,
      .quant_rounding = 0,
      .norm_mode = NE16_NORM_MODE_32BIT,
      .norm_bias = 1,
      .norm_shift = 0,
  };
  if (ne16_task_init(task, &task_config) != 0 ||
      ne16_task_set_ptrs(task, config->input, config->input_width,
                         config->input_channels, 8, config->padding_top,
                         config->padding_left, config->output, config->weights,
                         config->scale, config->shift, config->bias) != 0 ||
      ne16_task_set_dims(task, config->input_width, config->input_channels,
                         config->input_width, config->input_channels,
                         config->output_height, config->output_width,
                         config->output_channels, config->output_width,
                         config->output_channels, config->padding_top,
                         config->padding_bottom, config->padding_right,
                         config->padding_left) != 0) {
    return -1;
  }
  return 0;
}
