/* Genuine bindings; geometry/descriptor/transport validation, NOT MAC/RTL. */
#include "dory_chain_l0_layer.h"
#include "dory_chain_l1_layer.h"
#include "dory-ne16-generated-host.h"

#define INPUT UINT32_C(0x80000003)
#define WEIGHTS0 UINT32_C(0x81000005)
#define INTERMEDIATE UINT32_C(0x82000007)
#define WEIGHTS1 UINT32_C(0x83000009)
#define OUTPUT UINT32_C(0x8400000b)
#define GUARD_BYTES 32u
#define GUARDED(name, bytes) static struct { \
  uint8_t before[GUARD_BYTES], data[bytes], after[GUARD_BYTES]; \
} name

_Static_assert(DORY_CHAIN_L0_HEIGHT == 48 && DORY_CHAIN_L0_WIDTH == 48 &&
    DORY_CHAIN_L1_HEIGHT == 48 && DORY_CHAIN_L1_WIDTH == 48 &&
    DORY_CHAIN_L0_CHANNELS == 16 && DORY_CHAIN_L1_CHANNELS == 16 &&
    DORY_CHAIN_L0_JOBS == 16 && DORY_CHAIN_L1_JOBS == 16 &&
    DORY_CHAIN_L0_QUANT_SHIFT == 11 && DORY_CHAIN_L1_QUANT_SHIFT == 9 &&
    DORY_CHAIN_L0_OUTPUT_BYTES == DORY_CHAIN_L1_INPUT_BYTES,
    "expected the genuine 48x48, shift-11/9 two-layer fixture");

GUARDED(input, DORY_CHAIN_L0_INPUT_BYTES);
GUARDED(weights0, DORY_CHAIN_L0_WEIGHTS_BYTES);
GUARDED(intermediate, DORY_CHAIN_L0_OUTPUT_BYTES);
GUARDED(weights1, DORY_CHAIN_L1_WEIGHTS_BYTES);
GUARDED(output, DORY_CHAIN_L1_OUTPUT_BYTES);
static uint8_t intermediate_snapshot[DORY_CHAIN_L0_OUTPUT_BYTES];
static uint8_t previous_output[DORY_CHAIN_L1_OUTPUT_BYTES], transport_salt;
static uint8_t coverage0[DORY_CHAIN_L0_OUTPUT_BYTES], coverage1[DORY_CHAIN_L1_OUTPUT_BYTES];
static dory_chain_l0_layer_t layer0;
static dory_chain_l1_layer_t layer1;
static dory_ne16_tiled_layer_t reference0, reference1;
static unsigned int producer_complete;

static uint8_t first_tag(uint32_t byte) {
  return (uint8_t)(byte * 37u + byte / 16u * 11u + transport_salt);
}

static uint8_t second_tag(uint32_t byte) {
  const uint32_t size = DORY_CHAIN_L1_INPUT_BYTES;
  const uint8_t *actual = host_memory(layer1.input, size);
  return (uint8_t)(byte * 19u + actual[byte] * 3u +
                  actual[(byte + 17u) % size] * 5u +
                  actual[(byte * 7u + 23u) % size] * 11u);
}

static int consumer_prepare(void *context, uint32_t index, unsigned int slot,
                            dory_pipeline_memory_t allocation, dory_pipeline_job_t *job) {
  CHECK(producer_complete && layer0.output == INTERMEDIATE && layer1.input == layer0.output);
  CHECK(memcmp(intermediate.data, intermediate_snapshot, sizeof(intermediate_snapshot)) == 0);
  for (uint32_t i = 0; i < sizeof(coverage0); ++i) CHECK(coverage0[i] == 1);
  return dory_chain_l1_layer_prepare(context, index, slot, allocation, job);
}

static void descriptor_checks(dory_pipeline_prepare_t prepare, void *context,
                              dory_ne16_tiled_layer_t *reference, uint32_t shift) {
  for (uint32_t index = 0; index < reference->jobs; ++index)
    for (unsigned int slot = 0; slot < 2; ++slot) {
      dory_pipeline_job_t job;
      CHECK(prepare(context, index, slot, slots[slot], &job) == 0);
      host_compare_job(reference, shift, index, slot, slots[slot], &job);
    }
  dory_pipeline_job_t job, unchanged;
  memset(&job, 0x6d, sizeof(job));
  unchanged = job;
#define REJECT(ctx, index, slot, allocation) do { \
  CHECK(prepare(ctx, index, slot, allocation, &job) < 0); \
  CHECK(memcmp(&job, &unchanged, sizeof(job)) == 0); \
} while (0)
  REJECT(NULL, 0, 0, slots[0]);
  REJECT(context, reference->jobs, 0, slots[0]);
  REJECT(context, 0, 2, slots[0]);
  dory_pipeline_memory_t invalid = slots[0];
  --invalid.bytes;
  REJECT(context, 0, 0, invalid);
  invalid = (dory_pipeline_memory_t){NE16_SCRATCH_BASE - 1u, 16384u};
  REJECT(context, 0, 0, invalid);
  invalid = (dory_pipeline_memory_t){NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 16383u, 16384u};
  REJECT(context, 0, 0, invalid);
  CHECK(prepare(context, 0, 0, slots[0], NULL) < 0);
  dory_ne16_compat_set_error(-88);
  REJECT(context, 0, 0, slots[0]);
  dory_ne16_compat_reset_stats();
#undef REJECT
}

static void guards(void) {
#define CHECK_GUARDS(name) do { \
  for (unsigned int i = 0; i < GUARD_BYTES; ++i) \
    CHECK(name.before[i] == GUARD && name.after[i] == GUARD); \
} while (0)
  CHECK_GUARDS(input);
  CHECK_GUARDS(weights0);
  CHECK_GUARDS(intermediate);
  CHECK_GUARDS(weights1);
  CHECK_GUARDS(output);
#undef CHECK_GUARDS
}

static void transport_checks(unsigned int delay) {
  host_reset();
  producer_complete = 0;
  /* Distinct producer bytes also rule out a hard-coded consumer result. */
  transport_salt = (uint8_t)delay;
  memset(&input, GUARD, sizeof(input));
  memset(&weights0, GUARD, sizeof(weights0));
  memset(&intermediate, GUARD, sizeof(intermediate));
  memset(&weights1, GUARD, sizeof(weights1));
  memset(&output, GUARD, sizeof(output));
  memcpy(input.data, dory_chain_l0_input, sizeof(input.data));
  memcpy(weights0.data, dory_chain_l0_weights, sizeof(weights0.data));
  memcpy(weights1.data, dory_chain_l1_weights, sizeof(weights1.data));
  /* There is deliberately no second input allocation or CPU input copy. */
  host_add_region(INPUT, input.data, sizeof(input.data));
  host_add_region(WEIGHTS0, weights0.data, sizeof(weights0.data));
  host_add_region(INTERMEDIATE, intermediate.data, sizeof(intermediate.data));
  host_add_region(WEIGHTS1, weights1.data, sizeof(weights1.data));
  host_add_region(OUTPUT, output.data, sizeof(output.data));
  host_run(dory_chain_l0_layer_prepare, &layer0, &reference0,
           DORY_CHAIN_L0_QUANT_SHIFT, first_tag, delay, coverage0);
  CHECK(host.stores == DORY_CHAIN_L0_JOBS && dma_transfer_idle());
  CHECK(layer0.output == layer1.input && layer1.input == INTERMEDIATE);
  guards();
  for (uint32_t i = 0; i < sizeof(output.data); ++i) CHECK(output.data[i] == GUARD);
  memcpy(intermediate_snapshot, intermediate.data, sizeof(intermediate_snapshot));
  producer_complete = 1;
  host_run(consumer_prepare, &layer1, &reference1,
           DORY_CHAIN_L1_QUANT_SHIFT, second_tag, delay, coverage1);
  CHECK(memcmp(intermediate.data, intermediate_snapshot, sizeof(intermediate_snapshot)) == 0);
  for (uint32_t i = 0; i < sizeof(output.data); ++i) {
    uint32_t size = sizeof(intermediate_snapshot);
    uint8_t expected = (uint8_t)(i * 19u + intermediate_snapshot[i] * 3u +
        intermediate_snapshot[(i + 17u) % size] * 5u +
        intermediate_snapshot[(i * 7u + 23u) % size] * 11u);
    CHECK(output.data[i] == expected && coverage1[i] == 1 && coverage0[i] == 1);
  }
  CHECK(memcmp(input.data, dory_chain_l0_input, sizeof(input.data)) == 0);
  CHECK(memcmp(weights0.data, dory_chain_l0_weights, sizeof(weights0.data)) == 0);
  CHECK(memcmp(weights1.data, dory_chain_l1_weights, sizeof(weights1.data)) == 0);
  guards();
  host_integrity();
}

int main(void) {
  CHECK(dory_chain_l0_layer_init(&layer0, INPUT, WEIGHTS0, INTERMEDIATE) == 0);
  CHECK(dory_chain_l1_layer_init(&layer1, INTERMEDIATE, WEIGHTS1, OUTPUT) == 0);
  CHECK(dory_ne16_tiled_init(&reference0, INPUT, WEIGHTS0, INTERMEDIATE,
      DORY_CHAIN_L0_HEIGHT, DORY_CHAIN_L0_WIDTH, DORY_CHAIN_L0_CHANNELS) == 0);
  CHECK(dory_ne16_tiled_init(&reference1, INTERMEDIATE, WEIGHTS1, OUTPUT,
      DORY_CHAIN_L1_HEIGHT, DORY_CHAIN_L1_WIDTH, DORY_CHAIN_L1_CHANNELS) == 0);
  CHECK(reference0.jobs == DORY_CHAIN_L0_JOBS && reference1.jobs == DORY_CHAIN_L1_JOBS);
  CHECK(memcmp(dory_chain_l0_expected, dory_chain_l1_input, DORY_CHAIN_L1_INPUT_BYTES) == 0);
  descriptor_checks(dory_chain_l0_layer_prepare, &layer0, &reference0, 11);
  descriptor_checks(dory_chain_l1_layer_prepare, &layer1, &reference1, 9);
  transport_checks(8);
  memcpy(previous_output, output.data, sizeof(previous_output));
  transport_checks(80);
  CHECK(memcmp(previous_output, output.data, sizeof(previous_output)) != 0);
  host_release();
  puts("DORY genuine chain geometry/descriptors/halos/queue/lifetimes/transport PASS "
       "(shifts 11/9, timings 8/80; NO MAC simulation)");
  return 0;
}
