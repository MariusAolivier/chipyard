#ifndef NE16_DRIVER_H
#define NE16_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#define NE16_CONTROL_BASE 0x10030000UL
#define NE16_SCRATCH_BASE 0x20000000UL
#define NE16_SCRATCH_BYTES (64u * 1024u)
#define NE16_TASK_WORDS 24

#define NE16_TRIGGER 0x00
#define NE16_ACQUIRE 0x04
#define NE16_STATUS 0x0c
#define NE16_SOFT_CLEAR 0x14
#define NE16_TASK_REGS 0x20
#define NE16_STATUS_FULL 0x101

#define NE16_FLAG_WEIGHT_OFFSET_LAYER_WISE (1u << 15)
#define NE16_FLAG_MODE_3X3 (0u << 5)
#define NE16_FLAG_MODE_BASIC (0u << 3)
#define NE16_FLAG_NORM_QUANT (1u << 4)
#define NE16_FLAG_NORM_BIAS (1u << 25)
#define NE16_NORM_MODE_32BIT (2u << 12)
#define NE16_QUANT_MODE_8BIT (0u << 21)
#define NE16_FLAG_QUANT_FUNCTION_RELU (0u << 23)

#define NE16_DORY_3X3_CONF0 \
  (NE16_FLAG_NORM_QUANT | NE16_FLAG_NORM_BIAS | NE16_NORM_MODE_32BIT | \
   (11u << 16) | NE16_FLAG_WEIGHT_OFFSET_LAYER_WISE | NE16_FLAG_MODE_3X3 | \
   NE16_FLAG_MODE_BASIC | 7u)

typedef struct {
  uint32_t words[NE16_TASK_WORDS];
} ne16_task_t;

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

void ne16_memory_fence(void);
void ne16_reset(void);
int ne16_scratchpad_write(uint32_t offset, const void *source, size_t size);
int ne16_scratchpad_read(void *destination, uint32_t offset, size_t size);
int ne16_submit(const ne16_task_t *task);
int ne16_wait_idle(uint32_t timeout);
int ne16_build_dory_3x3_task(ne16_task_t *task,
                             const ne16_dory_3x3_config_t *config);

#endif
