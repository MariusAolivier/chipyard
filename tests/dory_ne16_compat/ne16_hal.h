#ifndef CHIPYARD_DORY_NE16_HAL_H
#define CHIPYARD_DORY_NE16_HAL_H

#include <stdint.h>

typedef enum nnx_weight_offset_mode_e {
  weightOffsetModeSymmetric = 0,
  weightOffsetModeLayerWise = 1u << 15
} nnx_weight_offset_mode_e;

typedef enum {
  normMode8Bit = 0,
  normMode16Bit = 1u << 12,
  normMode32Bit = 2u << 12
} nnx_norm_mode_e;

typedef struct nnx_norm_t {
  nnx_norm_mode_e mode;
  int flag_bias;
  int flag_shift;
} nnx_norm_t;

typedef enum nnx_quant_mode_e {
  quantMode8Bit = 0,
  quantMode16Bit = 1u << 21,
  quantMode32Bit = 2u << 21
} nnx_quant_mode_e;

typedef enum nnx_quant_function_e {
  quantFunctionIdentity = 1u << 23,
  quantFunctionRelu = 0
} nnx_quant_function_e;

typedef struct nnx_quant_t {
  unsigned shift_amount;
  nnx_quant_mode_e mode;
  nnx_quant_function_e function;
  int flag_rounding;
} nnx_quant_t;

#define NE16_DONT_PAD 0
#define NE16_FLAG_USED 1
#define NE16_FLAG_UNUSED 0

#endif
