#include "pulp_nnx_util.h"

int divnceil(int dividend, int divisor) {
  if (dividend < 0 || divisor <= 0) return 0;
  return (dividend + divisor - 1) / divisor;
}

int dory_remainder(int dividend, int divisor) {
  if (dividend <= 0 || divisor <= 0) return 0;
  int result = dividend % divisor;
  return result == 0 ? divisor : result;
}

uint32_t concat_half(uint16_t high, uint16_t low) {
  return ((uint32_t)high << 16) | low;
}
