#ifndef CHIPYARD_DORY_PULP_NNX_H
#define CHIPYARD_DORY_PULP_NNX_H

#include <stdint.h>
#include "ne16_hal.h"

typedef struct {
  uint32_t words[24];
} nnx_task_t;

void nnx_init(uint32_t max_stall);
void nnx_term(void);
int nnx_dispatch_check(void);
void nnx_dispatch_check_blocking(void);
void nnx_dispatch_task(nnx_task_t *task);
int nnx_resolve_check(nnx_task_t *task);
void nnx_resolve_check_blocking(nnx_task_t *task);
void nnx_task_init(nnx_task_t *task, uint8_t kernel_shape, uint8_t depthwise,
                   uint8_t input_bits, uint8_t output_bits, uint8_t weights_bits,
                   nnx_weight_offset_mode_e weights_offset_mode,
                   uint32_t weights_offset_factor, nnx_quant_t quant,
                   nnx_norm_t norm, uint8_t stride);
uint32_t nnx_pad_ptr(uint32_t ptr, uint32_t width, uint32_t channel,
                     uint8_t bits, uint8_t padding_top, uint8_t padding_left);
void nnx_task_set_ptrs(nnx_task_t *task, uint32_t input_ptr, uint32_t w_in,
                       uint32_t k_in, uint8_t bits_in, uint8_t padding_top,
                       uint8_t padding_left, uint32_t output_ptr,
                       uint32_t weights_ptr, uint32_t scale_ptr,
                       uint32_t shift_ptr, uint32_t bias_ptr);
void nnx_task_set_dims(nnx_task_t *task, uint32_t w_in, uint32_t k_in,
                       uint32_t w_in_stride, uint32_t k_in_stride,
                       uint32_t h_out, uint32_t w_out, uint32_t k_out,
                       uint32_t w_out_stride, uint32_t k_out_stride,
                       uint8_t padding_top, uint8_t padding_bottom,
                       uint8_t padding_right, uint8_t padding_left);
void nnx_task_set_dims_stride2x2(nnx_task_t *task, uint32_t h_in,
                                 uint32_t w_in, uint32_t k_in,
                                 uint32_t w_in_stride, uint32_t k_in_stride,
                                 uint32_t h_out, uint32_t w_out,
                                 uint32_t k_out, uint32_t w_out_stride,
                                 uint32_t k_out_stride, uint8_t h_ker,
                                 uint8_t w_ker, uint8_t padding_top,
                                 uint8_t padding_bottom, uint8_t padding_right,
                                 uint8_t padding_left);
void nnx_dispatch_task_stride2x2(nnx_task_t *task, uint32_t w_in,
                                 uint32_t k_in, uint32_t w_in_stride,
                                 uint32_t k_in_stride, uint32_t h_out,
                                 uint32_t w_out, uint32_t k_out,
                                 uint32_t w_out_stride,
                                 uint32_t k_out_stride, uint8_t h_ker,
                                 uint8_t w_ker);

#define DORY_NE16_MAX_POLL 1000000u
typedef struct {
  uint32_t dma_l2_to_l1_count;
  uint32_t dma_l1_to_l2_count;
  uint32_t ne16_dispatch_count;
  uint32_t ne16_completion_count;
} dory_ne16_stats_t;

void dory_ne16_compat_reset_stats(void);
dory_ne16_stats_t dory_ne16_compat_stats(void);
int dory_ne16_compat_error(void);
void dory_ne16_compat_set_error(int error);
void dory_ne16_compat_note_dma_l2_to_l1(void);
void dory_ne16_compat_note_dma_l1_to_l2(void);

#endif
