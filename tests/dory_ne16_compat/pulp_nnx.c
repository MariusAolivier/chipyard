#include "pulp_nnx.h"
#include "ne16_driver.h"

_Static_assert(sizeof(nnx_task_t) == sizeof(ne16_task_t),
               "NNX and Chipyard descriptors must have identical ABI size");

static dory_ne16_stats_t stats;
static int active;
static int error_code;
static uint32_t active_polls;

void dory_ne16_compat_reset_stats(void) {
  stats = (dory_ne16_stats_t){0};
  active = 0;
  error_code = 0;
  active_polls = 0;
}

dory_ne16_stats_t dory_ne16_compat_stats(void) { return stats; }
int dory_ne16_compat_error(void) { return error_code; }

void dory_ne16_compat_set_error(int error) {
  if (error_code == 0) error_code = error == 0 ? -1 : error;
}

void nnx_init(uint32_t max_stall) {
  (void)max_stall;
  active = 0;
  active_polls = 0;
  ne16_reset();
}

void nnx_term(void) {
  if (active) dory_ne16_compat_set_error(-20);
}

int nnx_dispatch_check(void) {
  return error_code == 0 && !active && !ne16_queue_full();
}

void nnx_dispatch_check_blocking(void) {
  uint32_t remaining = DORY_NE16_MAX_POLL;
  while (!nnx_dispatch_check()) {
    if (error_code != 0 || remaining-- == 0) {
      dory_ne16_compat_set_error(-21);
      return;
    }
  }
}

void nnx_dispatch_task(nnx_task_t *task) {
  if (task == NULL || !nnx_dispatch_check() || ne16_submit((ne16_task_t *)task) != 0) {
    dory_ne16_compat_set_error(-22);
    return;
  }
  active = 1;
  active_polls = 0;
  ++stats.ne16_dispatch_count;
}

int nnx_resolve_check(nnx_task_t *task) {
  if (task == NULL || error_code != 0) return 0;
  if (!active) return 1;
  if (!ne16_idle()) {
    if (++active_polls >= DORY_NE16_MAX_POLL) {
      active = 0;
      dory_ne16_compat_set_error(-32);
      return 1;
    }
    return 0;
  }
  active = 0;
  ++stats.ne16_completion_count;
  return 1;
}

void nnx_resolve_check_blocking(nnx_task_t *task) {
  uint32_t remaining = DORY_NE16_MAX_POLL;
  while (!nnx_resolve_check(task)) {
    if (remaining-- == 0) {
      dory_ne16_compat_set_error(-23);
      return;
    }
  }
}

void nnx_task_init(nnx_task_t *task, uint8_t kernel_shape, uint8_t depthwise,
                   uint8_t input_bits, uint8_t output_bits, uint8_t weights_bits,
                   nnx_weight_offset_mode_e weights_offset_mode,
                   uint32_t weights_offset_factor, nnx_quant_t quant,
                   nnx_norm_t norm, uint8_t stride) {
  if (task == NULL) {
    dory_ne16_compat_set_error(-24);
    return;
  }
  const ne16_task_config_t config = {
      .kernel_shape = kernel_shape,
      .depthwise = depthwise,
      .input_bits = input_bits,
      .output_bits = output_bits,
      .weights_bits = weights_bits,
      .stride = stride,
      .weights_offset_mode = weights_offset_mode,
      .weights_offset_factor = weights_offset_factor,
      .quant_shift = quant.shift_amount,
      .quant_mode = quant.mode,
      .quant_function = quant.function,
      .quant_rounding = (uint32_t)quant.flag_rounding,
      .norm_mode = norm.mode,
      .norm_bias = (uint8_t)norm.flag_bias,
      .norm_shift = (uint8_t)norm.flag_shift,
  };
  if (ne16_task_init((ne16_task_t *)task, &config) != 0) {
    dory_ne16_compat_set_error(-25);
  }
}

uint32_t nnx_pad_ptr(uint32_t ptr, uint32_t width, uint32_t channel,
                     uint8_t bits, uint8_t padding_top, uint8_t padding_left) {
  if (bits == 0 || bits % 8 != 0) {
    dory_ne16_compat_set_error(-26);
    return ptr;
  }
  uint64_t adjustment =
      ((uint64_t)padding_top * width + padding_left) * channel * (bits / 8u);
  if (adjustment > UINT32_MAX || ptr < adjustment) {
    dory_ne16_compat_set_error(-27);
    return ptr;
  }
  return ptr - (uint32_t)adjustment;
}

void nnx_task_set_ptrs(nnx_task_t *task, uint32_t input_ptr, uint32_t w_in,
                       uint32_t k_in, uint8_t bits_in, uint8_t padding_top,
                       uint8_t padding_left, uint32_t output_ptr,
                       uint32_t weights_ptr, uint32_t scale_ptr,
                       uint32_t shift_ptr, uint32_t bias_ptr) {
  if (ne16_task_set_ptrs((ne16_task_t *)task, input_ptr, w_in, k_in, bits_in,
                         padding_top, padding_left, output_ptr, weights_ptr,
                         scale_ptr, shift_ptr, bias_ptr) != 0) {
    dory_ne16_compat_set_error(-28);
  }
}

void nnx_task_set_dims(nnx_task_t *task, uint32_t w_in, uint32_t k_in,
                       uint32_t w_in_stride, uint32_t k_in_stride,
                       uint32_t h_out, uint32_t w_out, uint32_t k_out,
                       uint32_t w_out_stride, uint32_t k_out_stride,
                       uint8_t padding_top, uint8_t padding_bottom,
                       uint8_t padding_right, uint8_t padding_left) {
  if (ne16_task_set_dims((ne16_task_t *)task, w_in, k_in, w_in_stride,
                         k_in_stride, h_out, w_out, k_out, w_out_stride,
                         k_out_stride, padding_top, padding_bottom,
                         padding_right, padding_left) != 0) {
    dory_ne16_compat_set_error(-29);
  }
}

void nnx_task_set_dims_stride2x2(nnx_task_t *task, uint32_t h_in,
                                 uint32_t w_in, uint32_t k_in,
                                 uint32_t w_in_stride, uint32_t k_in_stride,
                                 uint32_t h_out, uint32_t w_out,
                                 uint32_t k_out, uint32_t w_out_stride,
                                 uint32_t k_out_stride, uint8_t h_ker,
                                 uint8_t w_ker, uint8_t padding_top,
                                 uint8_t padding_bottom, uint8_t padding_right,
                                 uint8_t padding_left) {
  (void)task; (void)h_in; (void)w_in; (void)k_in; (void)w_in_stride;
  (void)k_in_stride; (void)h_out; (void)w_out; (void)k_out;
  (void)w_out_stride; (void)k_out_stride; (void)h_ker; (void)w_ker;
  (void)padding_top; (void)padding_bottom; (void)padding_right;
  (void)padding_left;
  dory_ne16_compat_set_error(-30);
}

void nnx_dispatch_task_stride2x2(nnx_task_t *task, uint32_t w_in,
                                 uint32_t k_in, uint32_t w_in_stride,
                                 uint32_t k_in_stride, uint32_t h_out,
                                 uint32_t w_out, uint32_t k_out,
                                 uint32_t w_out_stride,
                                 uint32_t k_out_stride, uint8_t h_ker,
                                 uint8_t w_ker) {
  (void)task; (void)w_in; (void)k_in; (void)w_in_stride; (void)k_in_stride;
  (void)h_out; (void)w_out; (void)k_out; (void)w_out_stride;
  (void)k_out_stride; (void)h_ker; (void)w_ker;
  dory_ne16_compat_set_error(-31);
}

void dory_ne16_compat_note_dma_l2_to_l1(void) {
  ++stats.dma_l2_to_l1_count;
}
void dory_ne16_compat_note_dma_l1_to_l2(void) {
  ++stats.dma_l1_to_l2_count;
}
