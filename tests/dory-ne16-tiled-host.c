/* Host-only geometry/ownership checks. The constructor captures its arguments;
 * compute emits transport tags, NOT MACs or a convolution reference. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dory_tiled_layer.h"
#include "ne16_driver.h"

#define CHECK(test) do { \
  if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
    exit(EXIT_FAILURE); \
  } \
} while (0)

#define GUARD 0xa5u
#define GUARD_BYTES 32u
#define MAX_CAPTURES 256u
#define INPUT_ADDRESS UINT32_C(0x80000003)
#define WEIGHTS_ADDRESS UINT32_C(0x81000005)
#define OUTPUT_ADDRESS UINT32_C(0x82000007)

typedef struct {
  uint32_t address, bytes;
  uint8_t *allocation, *data;
} region_t;

static region_t regions[4];
static dory_ne16_tiled_layer_t layer;
static uint8_t *coverage;
static ne16_dory_3x3_config_t captures[MAX_CAPTURES];
static unsigned int capture_count, constructor_failure;
static dory_ne16_stats_t stats;
static int sticky_error, dma_ticks, dma_frozen, dma_failure;
static ne16_dma_config_t active_dma;
static unsigned int dma_starts, dma_finishes;
static int nnx_active, nnx_ticks, compute_delay;
static unsigned int active_capture, dispatches, stores, overlap, pipeline_mode;
static unsigned int slot_uses[2];
static const dory_pipeline_memory_t slots[2] = {
    {NE16_SCRATCH_BASE + 0x1000u, 16384u},
    {NE16_SCRATCH_BASE + 0x6000u, 16384u}};

static uint64_t endpoint_span(const ne16_dma_config_t *c, int source) {
  return (uint64_t)(c->planes - 1u) *
             (source ? c->src_plane_stride : c->dst_plane_stride) +
         (uint64_t)(c->rows - 1u) *
             (source ? c->src_row_stride : c->dst_row_stride) + c->line_bytes;
}

static void check_inside(uint32_t address, uint64_t bytes,
                         uint32_t base, uint32_t capacity) {
  CHECK(address >= base);
  CHECK((uint64_t)address + bytes <= (uint64_t)base + capacity);
}

static uint8_t *memory(uint32_t address, uint32_t bytes) {
  for (unsigned int i = 0; i < 4; ++i) {
    if (address >= regions[i].address &&
        (uint64_t)address + bytes <=
            (uint64_t)regions[i].address + regions[i].bytes)
      return regions[i].data + (address - regions[i].address);
  }
  fprintf(stderr, "unmapped host-model span: 0x%08" PRIx32 " + %" PRIu32 "\n",
          address, bytes);
  exit(EXIT_FAILURE);
}

static void release_regions(void) {
  for (unsigned int i = 0; i < 4; ++i) {
    free(regions[i].allocation);
    regions[i] = (region_t){0};
  }
  free(coverage);
  coverage = NULL;
}

static void configure_layer(uint32_t height, uint32_t width, uint32_t channels) {
  CHECK(!dma_ticks && !nnx_active);
  release_regions();
  CHECK(dory_ne16_tiled_init(&layer, INPUT_ADDRESS, WEIGHTS_ADDRESS,
                            OUTPUT_ADDRESS, height, width, channels) == 0);
  const uint32_t addresses[4] = {
      INPUT_ADDRESS, WEIGHTS_ADDRESS, OUTPUT_ADDRESS, NE16_SCRATCH_BASE};
  const uint32_t sizes[4] = {
      height * width * 16u, channels * 152u,
      height * width * channels, NE16_SCRATCH_BYTES};
  for (unsigned int i = 0; i < 4; ++i) {
    regions[i].address = addresses[i];
    regions[i].bytes = sizes[i];
    regions[i].allocation = malloc((size_t)sizes[i] + 2u * GUARD_BYTES);
    CHECK(regions[i].allocation != NULL);
    memset(regions[i].allocation, GUARD, (size_t)sizes[i] + 2u * GUARD_BYTES);
    regions[i].data = regions[i].allocation + GUARD_BYTES;
  }
  for (uint32_t i = 0; i < regions[0].bytes; ++i)
    regions[0].data[i] = (uint8_t)(i * 29u + (i / 16u) * 7u + 3u);
  for (uint32_t i = 0; i < regions[1].bytes; ++i)
    regions[1].data[i] = (uint8_t)(i * 37u + (i / 144u) * 11u + 9u);
  coverage = calloc(regions[2].bytes, 1);
  CHECK(coverage != NULL);
  capture_count = 0;
}

static void check_guards(void) {
  for (unsigned int region = 0; region < 4; ++region)
    for (unsigned int i = 0; i < GUARD_BYTES; ++i) {
      CHECK(regions[region].allocation[i] == GUARD);
      CHECK(regions[region].data[regions[region].bytes + i] == GUARD);
    }
}

static uint32_t minimum(uint32_t a, uint32_t b) { return a < b ? a : b; }

static dory_ne16_tile_t expected_tile(uint32_t index) {
  uint32_t groups = layer.output_channels / 16u;
  uint32_t columns = (layer.width + 11u) / 12u;
  uint32_t spatial = index / groups;
  dory_ne16_tile_t tile = {0};
  tile.output_y = spatial / columns * 12u;
  tile.output_x = spatial % columns * 12u;
  tile.output_channel = index % groups * 16u;
  tile.output_height = minimum(12u, layer.height - tile.output_y);
  tile.output_width = minimum(12u, layer.width - tile.output_x);
  tile.input_y = tile.output_y == 0 ? 0 : tile.output_y - 1u;
  tile.input_x = tile.output_x == 0 ? 0 : tile.output_x - 1u;
  uint32_t bottom = minimum(layer.height,
                            tile.output_y + tile.output_height + 1u);
  uint32_t right = minimum(layer.width,
                           tile.output_x + tile.output_width + 1u);
  tile.input_height = bottom - tile.input_y;
  tile.input_width = right - tile.input_x;
  tile.padding_top = tile.output_y == 0;
  tile.padding_left = tile.output_x == 0;
  tile.padding_bottom = bottom == layer.height &&
                       tile.output_y + tile.output_height == layer.height;
  tile.padding_right = right == layer.width &&
                      tile.output_x + tile.output_width == layer.width;
  return tile;
}

static void check_tile(const dory_ne16_tile_t *a, const dory_ne16_tile_t *b) {
#define FIELD(name) CHECK(a->name == b->name)
  FIELD(output_y); FIELD(output_x); FIELD(output_channel);
  FIELD(output_height); FIELD(output_width);
  FIELD(input_y); FIELD(input_x); FIELD(input_height); FIELD(input_width);
  FIELD(padding_top); FIELD(padding_bottom); FIELD(padding_left); FIELD(padding_right);
#undef FIELD
}

int ne16_build_dory_3x3_task(ne16_task_t *task,
                            const ne16_dory_3x3_config_t *config) {
  CHECK(task != NULL && config != NULL);
  if (constructor_failure) return -1;
  CHECK(capture_count < MAX_CAPTURES);
  captures[capture_count] = *config;
  /* These are pointer placeholders plus a capture token, not NE16 encoding. */
  *task = (ne16_task_t){0};
  task->words[0] = config->weights;
  task->words[1] = config->input;
  task->words[2] = config->output;
  task->words[3] = config->scale;
  task->words[4] = config->shift;
  task->words[5] = config->bias;
  task->words[23] = ++capture_count;
  return 0;
}

static void check_dma(const ne16_dma_config_t *a, const ne16_dma_config_t *b) {
#define FIELD(name) CHECK(a->name == b->name)
  FIELD(src); FIELD(dst); FIELD(line_bytes); FIELD(rows); FIELD(planes);
  FIELD(src_row_stride); FIELD(dst_row_stride);
  FIELD(src_plane_stride); FIELD(dst_plane_stride);
#undef FIELD
  CHECK(ne16_dma_validate(a) == 0);
}

static uint64_t check_neighbors(dory_ne16_tile_t tile,
                                const ne16_dma_config_t *input,
                                const uint8_t *packed) {
  uint64_t checked = 0;
  for (uint32_t y = 0; y < tile.output_height; ++y)
    for (uint32_t x = 0; x < tile.output_width; ++x)
      for (int ky = 0; ky < 3; ++ky)
        for (int kx = 0; kx < 3; ++kx) {
          int gy = (int)(tile.output_y + y) + ky - 1;
          int gx = (int)(tile.output_x + x) + kx - 1;
          int ly = (int)y + ky - tile.padding_top;
          int lx = (int)x + kx - tile.padding_left;
          int global_valid = gy >= 0 && gx >= 0 &&
                             gy < (int)layer.height && gx < (int)layer.width;
          int local_valid = ly >= 0 && lx >= 0 &&
                            ly < (int)tile.input_height &&
                            lx < (int)tile.input_width;
          CHECK(global_valid == local_valid);
          if (!global_valid) {
            CHECK((gy < 0 && tile.padding_top) ||
                  (gx < 0 && tile.padding_left) ||
                  (gy >= (int)layer.height && tile.padding_bottom) ||
                  (gx >= (int)layer.width && tile.padding_right));
            checked += 16;
            continue;
          }
          for (uint32_t channel = 0; channel < 16; ++channel) {
            uint32_t source = input->src + (uint32_t)ly * input->src_row_stride +
                              (uint32_t)lx * 16u + channel;
            uint32_t expected = layer.input +
                ((uint32_t)gy * layer.width + (uint32_t)gx) * 16u + channel;
            CHECK(source == expected);
            uint32_t local = (uint32_t)ly * input->dst_row_stride +
                             (uint32_t)lx * 16u + channel;
            CHECK(packed[local] == *memory(expected, 1));
            ++checked;
          }
        }
  return checked;
}

static uint64_t check_job(uint32_t index, unsigned int slot,
                          dory_pipeline_job_t *job) {
  dory_ne16_tile_t tile, expected = expected_tile(index);
  CHECK(dory_ne16_tiled_info(&layer, index, &tile) == 0);
  check_tile(&tile, &expected);
  unsigned int previous = capture_count;
  CHECK(dory_ne16_tiled_prepare(&layer, index, slot, slots[slot], job) == 0);
  CHECK(capture_count == previous + 1u && job->load_count == 4);
  const ne16_dory_3x3_config_t *c = &captures[previous];
  uint32_t base = slots[slot].base;
  CHECK(c->input == base + 2048u && c->output == base + 8192u);
  CHECK(c->weights == base + 12288u && c->scale == base + 14592u);
  CHECK(c->bias == base + 14656u && c->shift == 0);
  CHECK(c->input_width == tile.input_width && c->input_channels == 16);
  CHECK(c->output_height == tile.output_height);
  CHECK(c->output_width == tile.output_width && c->output_channels == 16);
  CHECK(c->padding_top == tile.padding_top);
  CHECK(c->padding_bottom == tile.padding_bottom);
  CHECK(c->padding_left == tile.padding_left);
  CHECK(c->padding_right == tile.padding_right);
  CHECK(c->weight_offset_factor == -128);
  uint32_t pad = (tile.padding_top * tile.input_width + tile.padding_left) * 16u;
  CHECK(pad <= 2048u);
  check_inside(c->input - pad, pad + tile.input_height * tile.input_width * 16u +
                36u, base, slots[slot].bytes);
  CHECK(2048u + tile.input_height * tile.input_width * 16u + 36u <= 8192u);
  CHECK(8192u + tile.output_height * tile.output_width * 16u + 36u <= 12288u);
  CHECK(14656u + 64u + 36u <= slots[slot].bytes);

  ne16_dma_config_t want[4] = {
      {.src = layer.input + (tile.input_y * layer.width + tile.input_x) * 16u,
       .dst = c->input, .line_bytes = tile.input_width * 16u,
       .rows = tile.input_height, .planes = 1,
       .src_row_stride = layer.width * 16u,
       .dst_row_stride = tile.input_width * 16u},
      {.src = layer.weights + tile.output_channel * 144u, .dst = c->weights,
       .line_bytes = 2304, .rows = 1, .planes = 1},
      {.src = layer.weights + layer.output_channels * 144u +
              tile.output_channel * 4u, .dst = c->scale,
       .line_bytes = 64, .rows = 1, .planes = 1},
      {.src = layer.weights + layer.output_channels * 148u +
              tile.output_channel * 4u, .dst = c->bias,
       .line_bytes = 64, .rows = 1, .planes = 1}};
  for (unsigned int i = 0; i < 4; ++i) {
    check_dma(&job->loads[i], &want[i]);
    check_inside(job->loads[i].dst, endpoint_span(&job->loads[i], 0),
                 base, slots[slot].bytes);
    unsigned int tensor = i == 0 ? 0 : 1;
    check_inside(job->loads[i].src, endpoint_span(&job->loads[i], 1),
                 regions[tensor].address, regions[tensor].bytes);
  }
  ne16_dma_config_t store = {
      .src = c->output,
      .dst = layer.output +
             (tile.output_y * layer.width + tile.output_x) *
             layer.output_channels + tile.output_channel,
      .line_bytes = 16, .rows = tile.output_width, .planes = tile.output_height,
      .src_row_stride = 16, .dst_row_stride = layer.output_channels,
      .src_plane_stride = tile.output_width * 16u,
      .dst_plane_stride = layer.width * layer.output_channels};
  check_dma(&job->store, &store);
  check_inside(store.src, endpoint_span(&store, 1), base, slots[slot].bytes);
  check_inside(store.dst, endpoint_span(&store, 0),
               regions[2].address, regions[2].bytes);
  for (uint32_t p = 0; p < store.planes; ++p)
    for (uint32_t r = 0; r < store.rows; ++r)
      for (uint32_t b = 0; b < store.line_bytes; ++b) {
        uint32_t src = store.src + p * store.src_plane_stride +
                       r * store.src_row_stride + b;
        uint32_t dst = store.dst + p * store.dst_plane_stride +
                       r * store.dst_row_stride + b;
        CHECK(src == c->output + (p * tile.output_width + r) * 16u + b);
        CHECK(dst == layer.output +
            ((tile.output_y + p) * layer.width + tile.output_x + r) *
            layer.output_channels + tile.output_channel + b);
        CHECK(++coverage[dst - layer.output] == 1);
      }
  uint8_t packed[14u * 14u * 16u];
  memset(packed, GUARD, sizeof(packed));
  for (uint32_t row = 0; row < job->loads[0].rows; ++row)
    memcpy(packed + row * job->loads[0].dst_row_stride,
           memory(job->loads[0].src + row * job->loads[0].src_row_stride,
                  job->loads[0].line_bytes), job->loads[0].line_bytes);
  return check_neighbors(tile, &job->loads[0], packed);
}

static void geometry_case(uint32_t height, uint32_t width, uint32_t channels,
                           int target) {
  configure_layer(height, width, channels);
  CHECK(layer.jobs == ((height + 11u) / 12u) * ((width + 11u) / 12u) *
                     (channels / 16u));
  uint64_t neighbors = 0;
  unsigned int corners = 0, interiors = 0, tails = 0, large_external_span = 0;
  for (uint32_t index = 0; index < layer.jobs; ++index) {
    dory_pipeline_job_t job;
    neighbors += check_job(index, index % 2u, &job);
    dory_ne16_tile_t tile = expected_tile(index);
    corners += (tile.padding_top || tile.padding_bottom) &&
               (tile.padding_left || tile.padding_right);
    interiors += !tile.padding_top && !tile.padding_bottom &&
                 !tile.padding_left && !tile.padding_right;
    tails += tile.output_height != 12 || tile.output_width != 12;
    large_external_span += endpoint_span(&job.store, 0) > 16384u;
  }
  for (uint32_t i = 0; i < regions[2].bytes; ++i) CHECK(coverage[i] == 1);
  CHECK(neighbors == (uint64_t)height * width * (channels / 16u) * 9u * 16u);
  if (target) {
    CHECK(layer.jobs == 27 && corners == 12 && interiors == 3 && tails == 15);
    CHECK(regions[0].bytes + regions[1].bytes + regions[2].bytes == 72832u);
    CHECK(regions[2].bytes == 49152u && neighbors == 442368u);
    CHECK(large_external_span != 0);
    printf("target geometry PASS: 27 jobs, 12 corners/3 interior/15 tails, "
           "49152 output bytes once, 442368 neighbor bytes/padding\n");
  }
  check_guards();
}

void ne16_memory_fence(void) {}
int dory_ne16_compat_error(void) { return sticky_error; }
void dory_ne16_compat_set_error(int error) {
  if (sticky_error == 0) sticky_error = error == 0 ? -1 : error;
}
dory_ne16_stats_t dory_ne16_compat_stats(void) { return stats; }
void dory_ne16_compat_reset_stats(void) {
  CHECK(!dma_ticks && !nnx_active);
  sticky_error = 0;
  stats = (dory_ne16_stats_t){0};
}
void dory_ne16_compat_note_dma_l2_to_l1(void) { ++stats.dma_l2_to_l1_count; }
void dory_ne16_compat_note_dma_l1_to_l2(void) { ++stats.dma_l1_to_l2_count; }

uint32_t ne16_dma_status(void) { return dma_ticks ? NE16_DMA_BUSY : 0; }

int ne16_dma_submit(const ne16_dma_config_t *config) {
  CHECK(ne16_dma_validate(config) == 0);
  if (dma_ticks) return NE16_DMA_EBUSY;
  if (pipeline_mode && nnx_active) {
    int load = config->src >= UINT32_C(0x80000000);
    uint32_t local = load ? config->dst : config->src;
    uint64_t span = endpoint_span(config, !load);
    uint32_t base = captures[active_capture].input - 2048u;
    CHECK((uint64_t)local + span <= base || local >= base + 16384u);
    ++overlap;
  }
  active_dma = *config;
  dma_ticks = 9;
  ++dma_starts;
  return 0;
}

int ne16_dma_poll(void) {
  CHECK(dma_ticks != 0);
  if (dma_frozen || --dma_ticks != 0) return 0;
  ++dma_finishes;
  if (dma_failure != 0) {
    int error = dma_failure;
    dma_failure = 0;
    return error;
  }
  int output = pipeline_mode && active_dma.dst >= layer.output &&
               active_dma.dst < layer.output + regions[2].bytes;
  if (output) {
    CHECK(stores < layer.jobs);
    dory_ne16_tile_t tile = expected_tile(stores);
    CHECK(active_dma.dst == layer.output +
        (tile.output_y * layer.width + tile.output_x) * layer.output_channels +
        tile.output_channel);
    ++stores;
  }
  for (uint32_t p = 0; p < active_dma.planes; ++p)
    for (uint32_t r = 0; r < active_dma.rows; ++r) {
      uint32_t src = active_dma.src + p * active_dma.src_plane_stride +
                     r * active_dma.src_row_stride;
      uint32_t dst = active_dma.dst + p * active_dma.dst_plane_stride +
                     r * active_dma.dst_row_stride;
      memcpy(memory(dst, active_dma.line_bytes),
             memory(src, active_dma.line_bytes), active_dma.line_bytes);
      if (output)
        for (uint32_t b = 0; b < active_dma.line_bytes; ++b)
          CHECK(++coverage[dst + b - layer.output] == 1);
    }
  return 1;
}

static uint8_t output_tag(uint32_t y, uint32_t x, uint32_t channel) {
  return (uint8_t)(y * 43u + x * 17u + channel * 7u + 19u);
}

static void complete_compute(void) {
  const ne16_dory_3x3_config_t *config = &captures[active_capture];
  dory_ne16_tile_t tile = expected_tile(active_capture);
  ne16_dma_config_t input = {
      .src = layer.input + (tile.input_y * layer.width + tile.input_x) * 16u,
      .dst = config->input, .line_bytes = tile.input_width * 16u,
      .rows = tile.input_height, .planes = 1,
      .src_row_stride = layer.width * 16u,
      .dst_row_stride = tile.input_width * 16u};
  CHECK(check_neighbors(tile, &input,
        memory(config->input, tile.input_height * tile.input_width * 16u)) ==
        (uint64_t)tile.output_height * tile.output_width * 9u * 16u);
  CHECK(memcmp(memory(config->weights, 2304),
               memory(layer.weights + tile.output_channel * 144u, 2304), 2304) == 0);
  CHECK(memcmp(memory(config->scale, 64),
               memory(layer.weights + layer.output_channels * 144u +
                      tile.output_channel * 4u, 64), 64) == 0);
  CHECK(memcmp(memory(config->bias, 64),
               memory(layer.weights + layer.output_channels * 148u +
                      tile.output_channel * 4u, 64), 64) == 0);
  uint8_t *output = memory(config->output,
                           tile.output_height * tile.output_width * 16u);
  for (uint32_t y = 0; y < tile.output_height; ++y)
    for (uint32_t x = 0; x < tile.output_width; ++x)
      for (uint32_t c = 0; c < 16; ++c)
        output[(y * tile.output_width + x) * 16u + c] =
            output_tag(tile.output_y + y, tile.output_x + x,
                       tile.output_channel + c);
}

int ne16_idle(void) { return !nnx_active; }
int ne16_wait_idle(uint32_t timeout) {
  (void)timeout;
  return nnx_active ? -1 : 0;
}
int nnx_dispatch_check(void) { return !nnx_active && sticky_error == 0; }
void nnx_dispatch_task(nnx_task_t *task) {
  CHECK(nnx_dispatch_check());
  CHECK(task->words[23] == dispatches + 1u);
  active_capture = dispatches++;
  CHECK(active_capture < capture_count);
  uint32_t base = captures[active_capture].input - 2048u;
  unsigned int slot = base == slots[0].base ? 0 : 1;
  CHECK(base == slots[slot].base);
  ++slot_uses[slot];
  nnx_active = 1;
  nnx_ticks = compute_delay;
  ++stats.ne16_dispatch_count;
}
int nnx_resolve_check(nnx_task_t *task) {
  CHECK(task != NULL && nnx_active && sticky_error == 0);
  CHECK(task->words[23] == active_capture + 1u);
  if (nnx_ticks > 0 && --nnx_ticks > 0) return 0;
  complete_compute();
  nnx_active = 0;
  ++stats.ne16_completion_count;
  return 1;
}

static void native_validator_tests(void) {
  ne16_dma_config_t valid = {
      .src = INPUT_ADDRESS, .dst = NE16_SCRATCH_BASE + 0x200u,
      .line_bytes = 5, .rows = 3, .planes = 2,
      .src_row_stride = 11, .dst_row_stride = 7,
      .src_plane_stride = 41, .dst_plane_stride = 29};
  CHECK(ne16_dma_validate(&valid) == 0);
  ne16_dma_config_t bad = valid;
  bad.rows = 0; CHECK(ne16_dma_validate(&bad) == NE16_DMA_EINVAL);
  bad = valid; bad.planes = 0; CHECK(ne16_dma_validate(&bad) == NE16_DMA_EINVAL);
  bad = valid; bad.line_bytes = 0; CHECK(ne16_dma_validate(&bad) == NE16_DMA_EINVAL);
  bad = valid; bad.dst_row_stride = 4;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EINVAL);
  bad = valid; bad.src_plane_stride = 26;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EINVAL);
  bad = valid; bad.dst = NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 1u;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EADDRESS);
  bad = valid; bad.src = UINT32_MAX - 2u;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EADDRESS);
  bad = valid; bad.dst = WEIGHTS_ADDRESS;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EADDRESS);
  bad = valid; bad.src = NE16_SCRATCH_BASE;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EADDRESS);
  bad = valid; bad.src = 0x10031000u;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EADDRESS);
  bad = valid; bad.rows = UINT32_MAX; bad.planes = 1;
  bad.src_row_stride = UINT32_MAX;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EADDRESS);
  bad = valid; bad.rows = 1; bad.planes = UINT32_MAX;
  bad.src_plane_stride = UINT32_MAX;
  CHECK(ne16_dma_validate(&bad) == NE16_DMA_EADDRESS);
  bad = valid; bad.rows = bad.planes = 1; bad.line_bytes = 4;
  bad.src = UINT32_MAX - 3u;
  bad.dst = NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 4u;
  CHECK(ne16_dma_validate(&bad) == 0);
  ++bad.src; CHECK(ne16_dma_validate(&bad) == NE16_DMA_EADDRESS);
  CHECK(ne16_dma_validate(NULL) == NE16_DMA_EINVAL);
}

static void invalid_adapter_tests(void) {
  configure_layer(32, 32, 48);
  dory_ne16_tiled_layer_t result = layer, saved = layer;
#define BAD_INIT(in, wt, out, h, w, oc) do { \
  CHECK(dory_ne16_tiled_init(&result, in, wt, out, h, w, oc) == \
        DORY_PIPELINE_EINVAL); \
  CHECK(memcmp(&result, &saved, sizeof(result)) == 0); \
} while (0)
  const uint32_t dimensions[] = {0, 65536u, UINT32_MAX};
  for (unsigned int i = 0; i < 3; ++i) {
    BAD_INIT(layer.input, layer.weights, layer.output, dimensions[i], 32, 48);
    BAD_INIT(layer.input, layer.weights, layer.output, 32, dimensions[i], 48);
  }
  const uint32_t channels[] = {0, 1, 15, 17, 47, 65536u, UINT32_MAX};
  for (unsigned int i = 0; i < 7; ++i)
    BAD_INIT(layer.input, layer.weights, layer.output, 32, 32, channels[i]);
  BAD_INIT(0x7fffffffu, layer.weights, layer.output, 32, 32, 48);
  BAD_INIT(layer.input, 0x7fffffffu, layer.output, 32, 32, 48);
  BAD_INIT(layer.input, layer.weights, 0x7fffffffu, 32, 32, 48);
  BAD_INIT(UINT32_MAX, layer.weights, layer.output, 32, 32, 48);
  BAD_INIT(layer.input, UINT32_MAX, layer.output, 32, 32, 48);
  BAD_INIT(layer.input, layer.weights, UINT32_MAX, 32, 32, 48);
  BAD_INIT(layer.input, layer.input + 1u, layer.output, 32, 32, 48);
  BAD_INIT(layer.input, layer.weights, layer.input + 16383u, 32, 32, 48);
  BAD_INIT(layer.input, layer.weights, layer.weights + 7295u, 32, 32, 48);
  BAD_INIT(layer.input, layer.weights, layer.output, 65535u, 65535u, 65520u);
  CHECK(dory_ne16_tiled_init(NULL, layer.input, layer.weights,
                            layer.output, 32, 32, 48) == DORY_PIPELINE_EINVAL);
  CHECK(dory_ne16_tiled_init(&result, layer.input, layer.input + 16384u,
                            layer.input + 16384u + 7296u, 32, 32, 48) == 0);
  CHECK(dory_ne16_tiled_init(&result, UINT32_MAX - 15u, layer.weights,
                            layer.output, 1, 1, 16) == 0);
  CHECK(dory_ne16_tiled_init(&result, layer.input, UINT32_MAX - 2431u,
                            layer.output, 1, 1, 16) == 0);
  CHECK(dory_ne16_tiled_init(&result, layer.input, layer.weights,
                            UINT32_MAX - 15u, 1, 1, 16) == 0);
#undef BAD_INIT
  dory_ne16_tile_t tile, untouched;
  memset(&tile, 0xcc, sizeof(tile)); untouched = tile;
  CHECK(dory_ne16_tiled_info(&layer, layer.jobs, &tile) == DORY_PIPELINE_EINVAL);
  CHECK(memcmp(&tile, &untouched, sizeof(tile)) == 0);
  CHECK(dory_ne16_tiled_info(&layer, UINT32_MAX, &tile) == DORY_PIPELINE_EINVAL);
  CHECK(dory_ne16_tiled_info(NULL, 0, &tile) == DORY_PIPELINE_EINVAL);
  CHECK(dory_ne16_tiled_info(&layer, 0, NULL) == DORY_PIPELINE_EINVAL);
  result = layer; ++result.jobs;
  CHECK(dory_ne16_tiled_info(&result, 0, &tile) == DORY_PIPELINE_EINVAL);
  result = layer; result.width = 0;
  CHECK(dory_ne16_tiled_info(&result, 0, &tile) == DORY_PIPELINE_EINVAL);
  dory_pipeline_job_t job, original;
  memset(&job, 0xcc, sizeof(job)); original = job;
#define BAD_PREPARE(ctx, index, slot, mem, ptr) do { \
  CHECK(dory_ne16_tiled_prepare(ctx, index, slot, mem, ptr) == \
        DORY_PIPELINE_EINVAL); \
  CHECK(memcmp(&job, &original, sizeof(job)) == 0); \
} while (0)
  dory_pipeline_memory_t memory = slots[0];
  BAD_PREPARE(NULL, 0, 0, memory, &job);
  BAD_PREPARE(&layer, layer.jobs, 0, memory, &job);
  BAD_PREPARE(&layer, 0, 2, memory, &job);
  BAD_PREPARE(&layer, 0, 0, memory, NULL);
  --memory.bytes; BAD_PREPARE(&layer, 0, 0, memory, &job);
  memory = slots[0]; memory.base = NE16_SCRATCH_BASE - 1u;
  BAD_PREPARE(&layer, 0, 0, memory, &job);
  memory.base = NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 16u;
  BAD_PREPARE(&layer, 0, 0, memory, &job);
  memory.base = UINT32_MAX; memory.bytes = UINT32_MAX;
  BAD_PREPARE(&layer, 0, 0, memory, &job);
#undef BAD_PREPARE
  constructor_failure = 1;
  CHECK(dory_ne16_tiled_prepare(&layer, 0, 0, slots[0], &job) ==
        DORY_PIPELINE_EPREPARE);
  CHECK(memcmp(&job, &original, sizeof(job)) == 0);
  constructor_failure = 0;
  memory = slots[0]; ++memory.bytes;
  CHECK(dory_ne16_tiled_prepare(&layer, 0, 0, memory, &job) == 0);
  puts("adapter/actual native-validator negatives PASS: dimensions, disjoint spans, "
       "unaligned DRAM, exact-2^32 ends, overflow, index, slot and constructor errors");
}

static void queue_tests(void) {
  configure_layer(32, 32, 48);
  dory_ne16_compat_reset_stats();
  dma_mutex_init();
  ne16_dma_config_t load = {
      .src = layer.input + 1u, .dst = NE16_SCRATCH_BASE + 0x200u,
      .line_bytes = 5, .rows = 3, .planes = 2,
      .src_row_stride = 11, .dst_row_stride = 7,
      .src_plane_stride = 41, .dst_plane_stride = 29};
  unsigned int before = dma_finishes;
  DmaTransfer first = dma_transfer_create();
  CHECK(dma_transfer_enqueue_config(&load) == 0);
  ne16_dma_config_t second = load; second.src += 100; second.dst += 100;
  CHECK(dma_transfer_enqueue_config(&second) == 0);
  DmaTransfer later = dma_transfer_create();
  ne16_dma_config_t store = {
      .src = load.dst, .dst = layer.weights + 3u,
      .line_bytes = 5, .rows = 3, .planes = 2,
      .src_row_stride = 7, .dst_row_stride = 13,
      .src_plane_stride = 29, .dst_plane_stride = 43};
  CHECK(dma_transfer_enqueue_config(&store) == 0);
  int result; unsigned int polls = 0;
  do { result = dma_transfer_poll(first); CHECK(++polls < 100); } while (!result);
  CHECK(result == 1 && dma_finishes == before + 2u && dma_ticks != 0);
  CHECK(dma_transfer_free_result(first) == 0);
  CHECK(dma_transfer_wait_result(later) == 0);
  CHECK(dma_transfer_free_result(later) == 0);
  CHECK(stats.dma_l2_to_l1_count == 2 && stats.dma_l1_to_l2_count == 1);
  for (uint32_t p = 0; p < 2; ++p)
    for (uint32_t r = 0; r < 3; ++r)
      for (uint32_t b = 0; b < 5; ++b)
        CHECK(*memory(store.dst + p * 43u + r * 13u + b, 1) ==
              *memory(load.src + p * 41u + r * 11u + b, 1));
  CHECK(dma_transfer_poll(first) == DORY_DMA_EHANDLE);
  dory_ne16_compat_reset_stats();
  DmaTransfer full = dma_transfer_create();
  before = dma_starts;
  for (unsigned int i = 0; i < DORY_DMA_QUEUE_CAPACITY; ++i)
    CHECK(dma_transfer_enqueue_config(&load) == 0);
  CHECK(dma_transfer_enqueue_config(&load) == DORY_DMA_EFULL);
  CHECK(dma_transfer_wait_result(full) == DORY_DMA_EFULL);
  CHECK(dma_transfer_idle() && dma_starts == before + DORY_DMA_QUEUE_CAPACITY);
  CHECK(dma_transfer_free_result(full) == DORY_DMA_EFULL);
  CHECK(stats.dma_l2_to_l1_count == DORY_DMA_QUEUE_CAPACITY);
  dory_ne16_compat_reset_stats();
  DmaTransfer bad = dma_transfer_create();
  CHECK(dma_transfer_enqueue_config(&load) == 0);
  CHECK(dma_transfer_enqueue_config(NULL) == NE16_DMA_EINVAL);
  CHECK(dma_transfer_enqueue_config(&load) == NE16_DMA_EINVAL);
  dma_frozen = 1;
  CHECK(dma_transfer_free_result(bad) == NE16_DMA_ETIMEOUT);
  CHECK(dma_transfer_poll(bad) == 0 && !dma_transfer_idle());
  dma_frozen = 0;
  CHECK(dma_transfer_drain() == 0);
  CHECK(dma_transfer_wait_result(bad) == NE16_DMA_EINVAL);
  CHECK(dma_transfer_free_result(bad) == NE16_DMA_EINVAL);
  CHECK(stats.dma_l2_to_l1_count == 1);
  dory_ne16_compat_reset_stats();
  bad = dma_transfer_create();
  CHECK(dma_transfer_enqueue_config(&load) == 0);
  CHECK(dma_transfer_enqueue_config(&second) == 0);
  before = dma_starts; dma_failure = NE16_DMA_ETL;
  CHECK(dma_transfer_wait_result(bad) == NE16_DMA_ETL);
  CHECK(dma_starts == before && dma_transfer_idle());
  CHECK(dma_transfer_free_result(bad) == NE16_DMA_ETL);
  dory_ne16_compat_reset_stats();
  for (unsigned int i = 0; i < DORY_DMA_GROUP_CAPACITY * 3u; ++i) {
    DmaTransfer legacy = dma_transfer_create();
    CHECK(legacy.id != 0);
    CHECK(dma_transfer_enqueue_config(&load) == 0);
    dma_transfer_wait(legacy);
    CHECK(sticky_error == 0 && dma_transfer_idle());
  }
  DmaTransferConf legacy = {.ext = layer.input, .loc = load.dst,
                            .length_1d_copy = 5, .dir = DORY_DMA_DIR_EXT2LOC};
  DmaTransfer mixed = dma_transfer_create();
  dma_transfer_1d_async(legacy);
  CHECK(dma_transfer_enqueue_config(&second) == 0);
  CHECK(dma_transfer_free_result(mixed) == 0);
  CHECK(dma_transfer_enqueue_config(&load) == DORY_DMA_EHANDLE);
  CHECK(dma_transfer_drain() == 0);
  dory_ne16_compat_reset_stats();
  DmaTransfer handles[DORY_DMA_GROUP_CAPACITY];
  for (unsigned int i = 0; i < DORY_DMA_GROUP_CAPACITY; ++i) {
    handles[i] = dma_transfer_create();
    CHECK(handles[i].id != 0);
  }
  CHECK(dma_transfer_create().id == 0 && sticky_error == DORY_DMA_EFULL);
  for (unsigned int i = 0; i < DORY_DMA_GROUP_CAPACITY; ++i)
    CHECK(dma_transfer_free_result(handles[i]) == 0);
  check_guards();
  dory_ne16_compat_reset_stats();
  puts("native queue PASS: unequal strides/both directions, fixed watermarks, "
       "full/error/timeout retention/drain, TL failure, legacy lifecycle/ABI");
}

static void pipeline_case(int delay) {
  configure_layer(32, 32, 48);
  dory_ne16_compat_reset_stats();
  dma_mutex_init();
  pipeline_mode = 1; compute_delay = delay;
  dispatches = stores = overlap = slot_uses[0] = slot_uses[1] = 0;
  dory_pipeline_t pipeline;
  CHECK(dory_pipeline_init(&pipeline, slots, layer.jobs,
                           dory_ne16_tiled_prepare, &layer) == 0);
  unsigned int polls = 0; int result;
  do { result = dory_pipeline_poll(&pipeline); CHECK(++polls < 10000); } while (!result);
  CHECK(result == 1 && sticky_error == 0 && dma_transfer_idle() && ne16_idle());
  CHECK(stores == 27 && dispatches == 27 && capture_count == 27);
  CHECK(slot_uses[0] >= 2 && slot_uses[1] >= 2 && overlap != 0);
  CHECK(stats.dma_l2_to_l1_count == 108 && stats.dma_l1_to_l2_count == 27);
  CHECK(stats.ne16_dispatch_count == 27 && stats.ne16_completion_count == 27);
  for (uint32_t y = 0; y < layer.height; ++y)
    for (uint32_t x = 0; x < layer.width; ++x)
      for (uint32_t c = 0; c < layer.output_channels; ++c) {
        uint32_t offset = (y * layer.width + x) * layer.output_channels + c;
        CHECK(coverage[offset] == 1);
        CHECK(regions[2].data[offset] == output_tag(y, x, c));
      }
  /* Regions outside the live slot footprints, including all wide-read guards,
   * must remain untouched by the transport-tag model. */
  for (uint32_t offset = 0; offset < NE16_SCRATCH_BYTES; ++offset) {
    int live = 0;
    for (unsigned int slot = 0; slot < 2; ++slot) {
      uint32_t base = slots[slot].base - NE16_SCRATCH_BASE;
      if (offset < base || offset >= base + 16384u) continue;
      uint32_t local = offset - base;
      live |= (local >= 2048u && local < 2048u + 3136u) ||
              (local >= 8192u && local < 8192u + 2304u) ||
              (local >= 12288u && local < 14720u);
    }
    if (!live) CHECK(regions[3].data[offset] == GUARD);
  }
  check_guards();
  pipeline_mode = 0;
}

static int mutation;
static int mutated_prepare(void *context, uint32_t index, unsigned int slot,
                            dory_pipeline_memory_t memory,
                            dory_pipeline_job_t *job) {
  int result = dory_ne16_tiled_prepare(context, index, slot, memory, job);
  if (result != 0) return result;
  if (mutation == 0) job->loads[0].dst_row_stride = 2048u;
  else job->store.src_plane_stride = 2048u;
  CHECK(ne16_dma_validate(mutation == 0 ? &job->loads[0] : &job->store) == 0);
  return 0;
}

static void pipeline_negative_tests(void) {
  configure_layer(32, 32, 48);
  for (mutation = 0; mutation < 2; ++mutation) {
    dory_ne16_compat_reset_stats(); dma_mutex_init(); capture_count = 0;
    dory_pipeline_t pipeline;
    CHECK(dory_pipeline_init(&pipeline, slots, layer.jobs, mutated_prepare, &layer) == 0);
    unsigned int before = dma_starts;
    CHECK(dory_pipeline_poll(&pipeline) == DORY_PIPELINE_EINVAL);
    CHECK(dma_starts == before && dma_transfer_idle());
    CHECK(dory_pipeline_drain(&pipeline) == 0);
  }
  dory_pipeline_t pipeline;
  dory_pipeline_memory_t overlap_slots[2] = {slots[0], slots[0]};
  dory_ne16_compat_reset_stats();
  CHECK(dory_pipeline_init(&pipeline, overlap_slots, 27,
                           dory_ne16_tiled_prepare, &layer) == DORY_PIPELINE_EINVAL);
  dory_ne16_compat_reset_stats();
  overlap_slots[1] = slots[1];
  overlap_slots[1].base = NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES - 1u;
  CHECK(dory_pipeline_init(&pipeline, overlap_slots, 27,
                           dory_ne16_tiled_prepare, &layer) == DORY_PIPELINE_EINVAL);
  dory_ne16_compat_reset_stats();
  CHECK(dory_pipeline_init(&pipeline, slots, 0,
                           dory_ne16_tiled_prepare, &layer) == DORY_PIPELINE_EINVAL);
  dory_ne16_compat_reset_stats();
  CHECK(dory_pipeline_init(&pipeline, slots, 27, NULL, &layer) == DORY_PIPELINE_EINVAL);
  dory_ne16_compat_reset_stats();
  puts("native pipeline PASS: local-vs-external stride selection, rejected "
       "local span escape/overlapping slots, zero jobs/null callback");
}

int main(void) {
  CHECK(DORY_NE16_TILE_HEIGHT == 12 && DORY_NE16_TILE_WIDTH == 12);
  CHECK(DORY_NE16_TILE_CHANNELS == 16 && DORY_NE16_TILED_SLOT_BYTES == 16384);
  native_validator_tests();
  invalid_adapter_tests();
  geometry_case(32, 32, 48, 1);
  const uint32_t dimensions[] = {1, 2, 11, 12, 13, 24, 25, 32};
  const uint32_t channels[] = {16, 32, 48};
  for (unsigned int h = 0; h < 8; ++h)
    for (unsigned int w = 0; w < 8; ++w)
      for (unsigned int c = 0; c < 3; ++c)
        geometry_case(dimensions[h], dimensions[w], channels[c], 0);
  puts("geometry matrix PASS: 192 thin/exact-boundary/seam/tail/channel-group shapes");
  queue_tests();
  const int delays[] = {0, 1, 7, 100};
  for (unsigned int i = 0; i < 4; ++i) pipeline_case(delays[i]);
  puts("27-job async pipeline PASS: four compute timings, 108 loads/27 stores, "
       "all 49152 transport-tag bytes once, ordered stores, disjoint live slots/reuse");
  pipeline_negative_tests();
  release_regions();
  puts("DORY tiled host suite PASS (no MAC/reference or RTL execution claimed)");
  return 0;
}
