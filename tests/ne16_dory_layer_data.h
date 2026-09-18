#ifndef NE16_DORY_LAYER_DATA_H
#define NE16_DORY_LAYER_DATA_H

#include <stddef.h>
#include <stdint.h>

#define NE16_DORY_INPUT_BYTES 16384
#define NE16_DORY_WEIGHTS_BYTES 2304
#define NE16_DORY_SCALE_BYTES 64
#define NE16_DORY_BIAS_BYTES 64
#define NE16_DORY_OUTPUT_BYTES 16384

extern const uint8_t ne16_dory_input[NE16_DORY_INPUT_BYTES];
extern const uint8_t ne16_dory_weights[NE16_DORY_WEIGHTS_BYTES];
extern const uint8_t ne16_dory_scale[NE16_DORY_SCALE_BYTES];
extern const uint8_t ne16_dory_bias[NE16_DORY_BIAS_BYTES];
extern const uint8_t ne16_dory_expected[NE16_DORY_OUTPUT_BYTES];

#endif
