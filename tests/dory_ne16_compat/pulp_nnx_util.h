#ifndef CHIPYARD_DORY_NNX_UTIL_H
#define CHIPYARD_DORY_NNX_UTIL_H
#include <stdint.h>
int divnceil(int dividend, int divisor);
int dory_remainder(int dividend, int divisor);
uint32_t concat_half(uint16_t high, uint16_t low);
#endif
