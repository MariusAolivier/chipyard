#ifndef CHIPYARD_DORY_NET_UTILS_H
#define CHIPYARD_DORY_NET_UTILS_H

#include <stddef.h>
#include <stdint.h>

#define NET_UTILS_PAD_TOP (1u << 3)
#define NET_UTILS_PAD_RIGHT (1u << 2)
#define NET_UTILS_PAD_BOTTOM (1u << 1)
#define NET_UTILS_PAD_LEFT (1u << 0)
#define NET_UTILS_NO_PAD 0u

typedef struct {
  unsigned int L3_input;
  unsigned int L3_output;
  unsigned int L3_after_weights;
  unsigned int L2_input;
  unsigned int bypass;
  unsigned int L2_output;
  unsigned int L2_weights;
  unsigned int L1_buffer;
  unsigned int ram;
  unsigned int padding;
  unsigned int layer_id;
} layer_args_t;

#endif
