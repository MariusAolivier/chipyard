#ifndef NE16_DRIVER_H
#define NE16_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#define NE16_CONTROL_BASE 0x10030000UL
#define NE16_SCRATCH_BASE 0x10040000UL
#define NE16_SCRATCH_BYTES (64u * 1024u)
#define NE16_TASK_WORDS 24

#define NE16_TRIGGER 0x00
#define NE16_ACQUIRE 0x04
#define NE16_STATUS 0x0c
#define NE16_RUNNING_JOB 0x10
#define NE16_SOFT_CLEAR 0x14
#define NE16_TASK_REGS 0x20
#define NE16_STATUS_FULL 0x101

#define NE16_FLAG_WEIGHT_OFFSET_LAYER_WISE (1u << 15)
#define NE16_FLAG_MODE_3X3 (0u << 5)
#define NE16_FLAG_MODE_BASIC (0u << 3)
#define NE16_FLAG_NORM_QUANT (1u << 4)
#define NE16_FLAG_NORM_BIAS (1u << 25)
#define NE16_FLAG_NORM_SHIFT (1u << 24)
#define NE16_NORM_MODE_32BIT (2u << 12)
#define NE16_QUANT_MODE_8BIT (0u << 21)
#define NE16_FLAG_QUANT_FUNCTION_RELU (0u << 23)
#define NE16_FLAG_ROUND (1u << 11)
#define NE16_FLAG_STRIDE_2X2 (1u << 8)
#define NE16_FLAG_MODE16 (1u << 3)

#define NE16_DORY_3X3_CONF0 \
  (NE16_FLAG_NORM_QUANT | NE16_FLAG_NORM_BIAS | NE16_NORM_MODE_32BIT | \
   (11u << 16) | NE16_FLAG_WEIGHT_OFFSET_LAYER_WISE | NE16_FLAG_MODE_3X3 | \
   NE16_FLAG_MODE_BASIC | 7u)

typedef struct {
  uint32_t words[NE16_TASK_WORDS];
} ne16_task_t;

_Static_assert(sizeof(ne16_task_t) == 24 * sizeof(uint32_t),
               "unexpected NE16 descriptor size");

typedef struct {
  uint8_t kernel_shape;
  uint8_t depthwise;
  uint8_t input_bits;
  uint8_t output_bits;
  uint8_t weights_bits;
  uint8_t stride;
  uint32_t weights_offset_mode;
  uint32_t weights_offset_factor;
  uint32_t quant_shift;
  uint32_t quant_mode;
  uint32_t quant_function;
  uint32_t quant_rounding;
  uint32_t norm_mode;
  uint8_t norm_bias;
  uint8_t norm_shift;
} ne16_task_config_t;

typedef struct {
  uint32_t input;
  uint32_t weights;
  uint32_t output;
  uint32_t scale;
  uint32_t shift;
  uint32_t bias;
  uint32_t input_width;
  uint32_t input_channels;
  uint32_t output_height;
  uint32_t output_width;
  uint32_t output_channels;
  uint8_t padding_top;
  uint8_t padding_right;
  uint8_t padding_bottom;
  uint8_t padding_left;
  int32_t weight_offset_factor;
} ne16_dory_3x3_config_t;

uint32_t ne16_status(void);
int ne16_queue_full(void);
int ne16_idle(void);
void ne16_memory_fence(void);
void ne16_reset(void);
int ne16_scratchpad_write(uint32_t offset, const void *source, size_t size);
int ne16_scratchpad_read(void *destination, uint32_t offset, size_t size);
int ne16_submit(const ne16_task_t *task);
int ne16_wait_idle(uint32_t timeout);

int ne16_task_init(ne16_task_t *task, const ne16_task_config_t *config);
int ne16_task_set_ptrs(ne16_task_t *task, uint32_t input_ptr,
                       uint32_t input_width, uint32_t input_channels,
                       uint8_t input_bits, uint8_t padding_top,
                       uint8_t padding_left, uint32_t output_ptr,
                       uint32_t weights_ptr, uint32_t scale_ptr,
                       uint32_t shift_ptr, uint32_t bias_ptr);
int ne16_task_set_dims(ne16_task_t *task, uint32_t input_width,
                       uint32_t input_channels, uint32_t input_width_stride,
                       uint32_t input_channel_stride, uint32_t output_height,
                       uint32_t output_width, uint32_t output_channels,
                       uint32_t output_width_stride,
                       uint32_t output_channel_stride, uint8_t padding_top,
                       uint8_t padding_bottom, uint8_t padding_right,
                       uint8_t padding_left);
int ne16_task_set_dims_stride2x2(ne16_task_t *task);
int ne16_task_set_padding(ne16_task_t *task, uint8_t top, uint8_t bottom,
                          uint8_t left, uint8_t right, uint8_t value);

int ne16_build_dory_3x3_task(ne16_task_t *task,
                             const ne16_dory_3x3_config_t *config);

#endif
