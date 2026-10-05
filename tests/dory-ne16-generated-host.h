/* Shared geometry/descriptor/transport model. No convolution/MAC simulation. */
#ifndef DORY_NE16_GENERATED_HOST_H
#define DORY_NE16_GENERATED_HOST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dory_tiled_layer.h"
#include "ne16_driver.h"

#define CHECK(test) do { if (!(test)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)
#define GUARD 0xa5

static const dory_pipeline_memory_t slots[2] = {
    {NE16_SCRATCH_BASE + 0x1000u, 16384u},
    {NE16_SCRATCH_BASE + 0x6000u, 16384u}};

typedef struct {
  uint32_t base, bytes;
  uint8_t *data, *shadow;
} host_region_t;

static struct {
  host_region_t regions[8];
  unsigned int region_count;
  uint8_t scratch[NE16_SCRATCH_BYTES];
  dory_ne16_tiled_layer_t *reference;
  dory_pipeline_prepare_t prepare;
  void *context;
  dory_pipeline_t *pipeline;
  uint8_t (*tag)(uint32_t);
  uint8_t *coverage;
  uint32_t shift, prepared, dispatched, completed, stores, dma_count;
  unsigned int compute_ticks, compute_delay, dma_ticks, load_overlap, store_overlap;
  unsigned int uses[2], loaded[2], computed[2], stored[2], owner[2];
  dory_pipeline_job_t jobs[2];
  const ne16_task_t *task_pointer;
  ne16_task_t task;
  const ne16_dma_config_t *dma_pointer;
  ne16_dma_config_t dma;
  unsigned int dma_slot, dma_load;
} host;

static host_region_t *host_region(uint32_t address, uint32_t bytes) {
  for (unsigned int i = 0; i < host.region_count; ++i) {
    host_region_t *r = &host.regions[i];
    if (address >= r->base &&
        (uint64_t)address + bytes <= (uint64_t)r->base + r->bytes) return r;
  }
  CHECK(!"unmapped transport span");
  return NULL;
}

static uint8_t *host_memory(uint32_t address, uint32_t bytes) {
  host_region_t *r = host_region(address, bytes);
  return r->data + address - r->base;
}

static void host_integrity(void) {
  for (unsigned int i = 0; i < host.region_count; ++i) {
    host_region_t *r = &host.regions[i];
    CHECK(memcmp(r->data, r->shadow, r->bytes) == 0);
  }
  for (unsigned int slot = 0; slot < 2; ++slot)
    if (host.uses[slot] && !host.stored[slot])
      CHECK(memcmp(&host.pipeline->slots[slot].job, &host.jobs[slot],
                   sizeof(host.jobs[slot])) == 0);
  if (host.compute_ticks)
    CHECK(memcmp(host.task_pointer, &host.task, sizeof(host.task)) == 0);
  if (host.dma_ticks)
    CHECK(memcmp(host.dma_pointer, &host.dma, sizeof(host.dma)) == 0);
}

static void host_write(uint32_t address, const uint8_t *source, uint32_t bytes) {
  host_region_t *r = host_region(address, bytes);
  CHECK(r->base == NE16_SCRATCH_BASE || r->base == host.reference->output);
  memcpy(r->data + address - r->base, source, bytes);
  memcpy(r->shadow + address - r->base, source, bytes);
}

static void host_release(void) {
  for (unsigned int i = 0; i < host.region_count; ++i) free(host.regions[i].shadow);
  host.region_count = 0;
}

static void host_add_region(uint32_t base, uint8_t *data, uint32_t bytes) {
  CHECK(host.region_count < 8 && bytes);
  for (unsigned int i = 0; i < host.region_count; ++i)
    CHECK((uint64_t)base + bytes <= host.regions[i].base ||
          (uint64_t)host.regions[i].base + host.regions[i].bytes <= base);
  uint8_t *shadow = malloc(bytes);
  CHECK(shadow != NULL);
  memcpy(shadow, data, bytes);
  host.regions[host.region_count++] = (host_region_t){base, bytes, data, shadow};
}

static void host_reset(void) {
  CHECK(!host.compute_ticks && !host.dma_ticks);
  host_release();
  memset(&host, 0, sizeof(host));
  memset(host.scratch, GUARD, sizeof(host.scratch));
  host_add_region(NE16_SCRATCH_BASE, host.scratch, sizeof(host.scratch));
}

static void host_compare_job(dory_ne16_tiled_layer_t *reference, uint32_t shift,
                             uint32_t index, unsigned int slot,
                             dory_pipeline_memory_t allocation,
                             const dory_pipeline_job_t *actual) {
  dory_pipeline_job_t expected;
  CHECK(shift <= 31);
  CHECK(dory_ne16_tiled_prepare(reference, index, slot, allocation, &expected) == 0);
  CHECK(((expected.task.words[23] >> 16) & 31u) == 11u);
  /* Only the generated shift differs from the independent fixed-11 builder. */
  expected.task.words[23] = (expected.task.words[23] & ~(31u << 16)) | (shift << 16);
  CHECK(((actual->task.words[23] >> 16) & 31u) == shift);
  for (unsigned int word = 0; word < NE16_TASK_WORDS; ++word)
    CHECK(actual->task.words[word] == expected.task.words[word]);
  CHECK(actual->load_count == expected.load_count);
  CHECK(memcmp(actual->loads, expected.loads, sizeof(actual->loads)) == 0);
  CHECK(memcmp(&actual->store, &expected.store, sizeof(actual->store)) == 0);
  for (unsigned int i = 0; i < actual->load_count; ++i)
    CHECK(ne16_dma_validate(&actual->loads[i]) == 0);
  CHECK(ne16_dma_validate(&actual->store) == 0);
}

static int host_prepare(void *context, uint32_t index, unsigned int slot,
                        dory_pipeline_memory_t allocation, dory_pipeline_job_t *job) {
  CHECK(context == host.context && slot < 2 && index == host.prepared++);
  host_integrity();
  CHECK(host.pipeline->slots[slot].state == DORY_SLOT_FREE);
  CHECK(host.stored[slot]);
  CHECK(host.prepare(context, index, slot, allocation, job) == 0);
  host_compare_job(host.reference, host.shift, index, slot, allocation, job);
  ++host.uses[slot];
  host.jobs[slot] = *job;
  host.owner[slot] = index;
  host.loaded[slot] = host.computed[slot] = host.stored[slot] = 0;
  return 0;
}

void ne16_reset(void) { CHECK(!host.compute_ticks); }

int ne16_submit(const ne16_task_t *task) {
  host_integrity();
  CHECK(!host.compute_ticks && host.dispatched < host.reference->jobs);
  unsigned int slot = task->words[2] >= slots[1].base ? 1u : 0u;
  CHECK(host.owner[slot] == host.dispatched && host.loaded[slot] == 4);
  CHECK(!host.computed[slot] && !host.stored[slot]);
  CHECK(task == (const ne16_task_t *)&host.pipeline->slots[slot].job.task);
  CHECK(memcmp(task, &host.jobs[slot].task, sizeof(*task)) == 0);
  dory_ne16_tile_t tile;
  CHECK(dory_ne16_tiled_info(host.reference, host.dispatched, &tile) == 0);
  uint8_t *packed = host_memory(slots[slot].base + DORY_NE16_TILED_INPUT_OFFSET,
                               tile.input_height * tile.input_width * 16u);
  for (uint32_t y = 0; y < tile.input_height; ++y)
    CHECK(memcmp(packed + y * tile.input_width * 16u,
                 host_memory(host.reference->input +
                     ((tile.input_y + y) * host.reference->width + tile.input_x) * 16u,
                     tile.input_width * 16u), tile.input_width * 16u) == 0);
  for (uint32_t y = 0; y < tile.output_height; ++y)
    for (uint32_t x = 0; x < tile.output_width; ++x)
      for (int ky = 0; ky < 3; ++ky)
        for (int kx = 0; kx < 3; ++kx) {
          int gy = (int)(tile.output_y + y) + ky - 1;
          int gx = (int)(tile.output_x + x) + kx - 1;
          int ly = (int)y + ky - tile.padding_top;
          int lx = (int)x + kx - tile.padding_left;
          int global_valid = gy >= 0 && gx >= 0 && gy < (int)host.reference->height &&
                             gx < (int)host.reference->width;
          int local_valid = ly >= 0 && lx >= 0 && ly < (int)tile.input_height &&
                            lx < (int)tile.input_width;
          CHECK(global_valid == local_valid);
          if (global_valid)
            CHECK(memcmp(packed + ((uint32_t)ly * tile.input_width + (uint32_t)lx) * 16u,
                         host_memory(host.reference->input +
                             ((uint32_t)gy * host.reference->width + (uint32_t)gx) * 16u,
                             16), 16) == 0);
        }
  const uint32_t offsets[] = {DORY_NE16_TILED_WEIGHTS_OFFSET,
                             DORY_NE16_TILED_SCALE_OFFSET, DORY_NE16_TILED_BIAS_OFFSET};
  const uint32_t sources[] = {tile.output_channel * 144u,
      host.reference->output_channels * 144u + tile.output_channel * 4u,
      host.reference->output_channels * 148u + tile.output_channel * 4u};
  for (unsigned int i = 0; i < 3; ++i) {
    uint32_t bytes = i == 0 ? 2304u : 64u;
    CHECK(memcmp(host_memory(slots[slot].base + offsets[i], bytes),
                 host_memory(host.reference->weights + sources[i], bytes), bytes) == 0);
  }
  host.task = *task;
  host.task_pointer = task;
  host.compute_ticks = host.compute_delay;
  ++host.dispatched;
  return 0;
}

int ne16_idle(void) {
  host_integrity();
  if (!host.compute_ticks) return 1;
  if (--host.compute_ticks) return 0;
  dory_ne16_tile_t tile;
  CHECK(dory_ne16_tiled_info(host.reference, host.completed, &tile) == 0);
  for (uint32_t y = 0; y < tile.output_height; ++y)
    for (uint32_t x = 0; x < tile.output_width; ++x)
      for (uint32_t channel = 0; channel < 16; ++channel) {
        uint32_t local = host.task.words[2] + (y * tile.output_width + x) * 16u + channel;
        uint32_t external = ((tile.output_y + y) * host.reference->width +
                             tile.output_x + x) * host.reference->output_channels +
                             tile.output_channel + channel;
        uint8_t tag = host.tag(external);
        host_write(local, &tag, 1);
      }
  unsigned int slot = host.task.words[2] >= slots[1].base ? 1u : 0u;
  host.computed[slot] = 1;
  ++host.completed;
  return 1;
}

int ne16_dma_submit(const ne16_dma_config_t *config) {
  host_integrity();
  CHECK(!host.dma_ticks && ne16_dma_validate(config) == 0);
  int load = config->dst >= NE16_SCRATCH_BASE &&
             config->dst < NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES;
  uint32_t address = load ? config->dst : config->src;
  unsigned int slot = address >= slots[1].base ? 1u : 0u;
  CHECK(address >= slots[slot].base && address < slots[slot].base + slots[slot].bytes);
  CHECK(host.uses[slot] && !host.stored[slot]);
  if (load) {
    CHECK(host.loaded[slot] < 4 && !host.computed[slot]);
    CHECK(memcmp(config, &host.jobs[slot].loads[host.loaded[slot]], sizeof(*config)) == 0);
    CHECK(host.pipeline->slots[slot].state == DORY_SLOT_LOADING);
  } else {
    CHECK(host.computed[slot] && host.loaded[slot] == 4);
    CHECK(memcmp(config, &host.jobs[slot].store, sizeof(*config)) == 0);
    CHECK(host.pipeline->slots[slot].state == DORY_SLOT_STORING);
  }
  host.dma = *config;
  host.dma_pointer = config;
  host.dma_slot = slot;
  host.dma_load = load;
  host.dma_ticks = 3;
  ++host.dma_count;
  return 0;
}

uint32_t ne16_dma_status(void) { return host.dma_ticks ? NE16_DMA_BUSY : 0; }

int ne16_dma_poll(void) {
  host_integrity();
  if (!host.dma_ticks) return 1;
  if (host.compute_ticks) {
    if (host.dma_load) ++host.load_overlap;
    else ++host.store_overlap;
  }
  if (--host.dma_ticks) return 0;
  for (uint32_t plane = 0; plane < host.dma.planes; ++plane)
    for (uint32_t row = 0; row < host.dma.rows; ++row) {
      uint32_t source = host.dma.src + plane * host.dma.src_plane_stride +
                        row * host.dma.src_row_stride;
      uint32_t destination = host.dma.dst + plane * host.dma.dst_plane_stride +
                             row * host.dma.dst_row_stride;
      host_write(destination, host_memory(source, host.dma.line_bytes), host.dma.line_bytes);
      if (!host.dma_load)
        for (uint32_t byte = 0; byte < host.dma.line_bytes; ++byte)
          CHECK(++host.coverage[destination - host.reference->output + byte] == 1);
    }
  if (host.dma_load) ++host.loaded[host.dma_slot];
  else {
    host.stored[host.dma_slot] = 1;
    ++host.stores;
  }
  return 1;
}

static void host_run(dory_pipeline_prepare_t prepare, void *context,
                     dory_ne16_tiled_layer_t *reference, uint32_t shift,
                     uint8_t (*tag)(uint32_t), unsigned int delay, uint8_t *coverage) {
  CHECK(!host.compute_ticks && !host.dma_ticks && dma_transfer_idle() && nnx_dispatch_check());
  host_integrity();
  host.reference = reference;
  host.prepare = prepare;
  host.context = context;
  host.shift = shift;
  host.tag = tag;
  host.coverage = coverage;
  host.compute_delay = delay;
  host.prepared = host.dispatched = host.completed = host.stores = host.dma_count = 0;
  host.load_overlap = host.store_overlap = host.uses[0] = host.uses[1] = 0;
  host.stored[0] = host.stored[1] = 1;
  uint32_t bytes = reference->height * reference->width * reference->output_channels;
  memset(coverage, 0, bytes);
  dory_ne16_compat_reset_stats();
  nnx_init(8);
  dma_mutex_init();
  dory_pipeline_t pipeline;
  host.pipeline = &pipeline;
  CHECK(dory_pipeline_init(&pipeline, slots, reference->jobs, host_prepare, context) == 0);
  CHECK(dory_pipeline_run(&pipeline) == 0);
  CHECK(host.prepared == reference->jobs && host.dispatched == reference->jobs &&
        host.completed == reference->jobs && host.stores == reference->jobs &&
        host.dma_count == reference->jobs * 5u && host.uses[0] >= 2 && host.uses[1] >= 2 &&
        host.load_overlap && host.store_overlap);
  CHECK(!host.compute_ticks && !host.dma_ticks && dma_transfer_idle() && nnx_dispatch_check());
  CHECK(pipeline.completed == reference->jobs && pipeline.computing_slot == -1);
  CHECK(dory_ne16_compat_error() == 0);
  dory_ne16_stats_t stats = dory_ne16_compat_stats();
  CHECK(stats.ne16_dispatch_count == reference->jobs &&
        stats.ne16_completion_count == reference->jobs &&
        stats.dma_l2_to_l1_count == reference->jobs * 4u &&
        stats.dma_l1_to_l2_count == reference->jobs);
  for (unsigned int slot = 0; slot < 2; ++slot) {
    CHECK(pipeline.slots[slot].state == DORY_SLOT_FREE && host.stored[slot]);
    CHECK(dma_transfer_poll(pipeline.slots[slot].transfer) == DORY_DMA_EHANDLE);
  }
  /* Released handles must be invalid, not merely complete. Clear only the
   * probe's sticky NNX error; the actual DMA queue is left drained and intact. */
  dory_ne16_compat_reset_stats();
  host_integrity();
  for (uint32_t i = 0; i < bytes; ++i)
    CHECK(host_memory(reference->output, bytes)[i] == tag(i) && coverage[i] == 1);
}

#endif
