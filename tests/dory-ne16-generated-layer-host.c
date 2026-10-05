/* Descriptor/transport validation, not a software convolution or RTL model. */
#include "dory_generated_layer.h"
#include "dory-ne16-generated-host.h"

#define INPUT UINT32_C(0x80000003)
#define WEIGHTS UINT32_C(0x81000005)
#define OUTPUT UINT32_C(0x82000007)

static uint8_t input[DORY_GENERATED_INPUT_BYTES], weights[DORY_GENERATED_WEIGHTS_BYTES];
static uint8_t output[DORY_GENERATED_OUTPUT_BYTES], coverage[DORY_GENERATED_OUTPUT_BYTES];
static dory_generated_layer_t layer;
static dory_ne16_tiled_layer_t reference;

static uint8_t tag(uint32_t byte) { return (uint8_t)(byte * 37u + byte / 16u * 11u); }

static void descriptor_checks(void) {
  dory_pipeline_job_t a, b;
  for (uint32_t index = 0; index < DORY_GENERATED_JOBS; ++index)
    for (unsigned int slot = 0; slot < 2; ++slot) {
      CHECK(dory_generated_layer_prepare(&layer, index, slot, slots[slot], &a) == 0);
      host_compare_job(&reference, 11, index, slot, slots[slot], &a);
    }
  memset(&a, 0x6d, sizeof(a));
  b = a;
#define REJECT(ctx, idx, slot, allocation) do { \
  CHECK(dory_generated_layer_prepare(ctx, idx, slot, allocation, &a) < 0); \
  CHECK(memcmp(&a, &b, sizeof(a)) == 0); \
} while (0)
  REJECT(NULL, 0, 0, slots[0]);
  REJECT(&layer, DORY_GENERATED_JOBS, 0, slots[0]);
  REJECT(&layer, 0, 2, slots[0]);
  dory_pipeline_memory_t invalid = slots[0];
  --invalid.bytes;
  REJECT(&layer, 0, 0, invalid);
  invalid = (dory_pipeline_memory_t){NE16_SCRATCH_BASE - 1u, 16384u};
  REJECT(&layer, 0, 0, invalid);
  invalid = (dory_pipeline_memory_t){NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 16383u, 16384u};
  REJECT(&layer, 0, 0, invalid);
  CHECK(dory_generated_layer_prepare(&layer, 0, 0, slots[0], NULL) < 0);
  dory_generated_layer_t invalid_layer = layer;
  invalid_layer.input = UINT32_MAX;
  REJECT(&invalid_layer, 0, 0, slots[0]);
  dory_ne16_compat_set_error(-88);
  REJECT(&layer, 0, 0, slots[0]);
  dory_ne16_compat_reset_stats();
#undef REJECT
  CHECK(dory_generated_layer_init(NULL, INPUT, WEIGHTS, OUTPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, 0x7fffffffu, WEIGHTS, OUTPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, INPUT, INPUT, OUTPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, INPUT, WEIGHTS, INPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, INPUT, OUTPUT, OUTPUT) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer, INPUT, WEIGHTS, UINT32_MAX) < 0);
  CHECK(dory_generated_layer_init(&invalid_layer,
      UINT32_MAX - DORY_GENERATED_INPUT_BYTES + 1u, WEIGHTS, OUTPUT) == 0);
}

static void transport_checks(unsigned int delay) {
  host_reset();
  memset(output, GUARD, sizeof(output));
  host_add_region(INPUT, input, sizeof(input));
  host_add_region(WEIGHTS, weights, sizeof(weights));
  host_add_region(OUTPUT, output, sizeof(output));
  host_run(dory_generated_layer_prepare, &layer, &reference, 11, tag, delay, coverage);
  for (uint32_t i = 0; i < sizeof(input); ++i) CHECK(input[i] == (uint8_t)(i * 29u + 3u));
  for (uint32_t i = 0; i < sizeof(weights); ++i) CHECK(weights[i] == (uint8_t)(i * 17u + 5u));
}

int main(void) {
  CHECK(dory_generated_layer_init(&layer, INPUT, WEIGHTS, OUTPUT) == 0);
  CHECK(dory_ne16_tiled_init(&reference, INPUT, WEIGHTS, OUTPUT,
        DORY_GENERATED_HEIGHT, DORY_GENERATED_WIDTH, DORY_GENERATED_CHANNELS) == 0);
  CHECK(reference.jobs == DORY_GENERATED_JOBS);
  for (uint32_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i * 29u + 3u);
  for (uint32_t i = 0; i < sizeof(weights); ++i) weights[i] = (uint8_t)(i * 17u + 5u);
  descriptor_checks();
  transport_checks(8);
  transport_checks(80);
  host_release();
  puts("DORY generated binding descriptors/halos/transport PASS (no MAC simulation)");
  return 0;
}
