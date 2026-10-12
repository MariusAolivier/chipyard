# NE16 Codebase Map

This is a clickable map of the NE16 integration. The source files remain in
their existing locations because the Chipyard, Bender, FireSim, and DORY
builds depend on those paths.

## Recommended reading order

1. [`NE16.scala`](../chipyard/src/main/scala/ne16/NE16.scala): top-level
   integration, TileLink connections, MMIO map, and configuration key.
2. [`NE16Scratchpad.scala`](../chipyard/src/main/scala/ne16/NE16Scratchpad.scala):
   nine-bank scratchpad and CPU/DMA/NE16 arbitration.
3. [`NE16DMA.scala`](../chipyard/src/main/scala/ne16/NE16DMA.scala): DMA
   descriptor, transfer FSM, address generation, and error handling.
4. [`ne16_dma.h`](../../tests/ne16_dma.h) and
   [`ne16_dma.c`](../../tests/ne16_dma.c): the software-facing DMA contract.
5. [`dory_dma.c`](../../tests/dory_ne16_compat/dory_dma.c): how generated
   DORY transfers are mapped onto the native DMA.
6. [`README.md`](README.md): build, simulation, FPGA, and validation context.
7. [`IDUN_RUNBOOK.md`](IDUN_RUNBOOK.md): operational FireSim/U250 procedures.

<details>
<summary><strong>1. Chipyard integration and configuration</strong></summary>

### NE16 hardware integration

- [`NE16.scala`](../chipyard/src/main/scala/ne16/NE16.scala)
  - `NE16Params` and `NE16Key`
  - `NE16BlackBox`
  - control and DMA TileLink nodes
  - MMIO register maps
  - FBUS memory connection
  - `CanHavePeripheryNE16`
  - `WithNE16`
- [`NE16DMA.scala`](../chipyard/src/main/scala/ne16/NE16DMA.scala)
- [`NE16Scratchpad.scala`](../chipyard/src/main/scala/ne16/NE16Scratchpad.scala)
- [`DigitalTop.scala`](../chipyard/src/main/scala/DigitalTop.scala)
  - adds the optional NE16 peripheral mixin
- [`MMIOAcceleratorConfigs.scala`](../chipyard/src/main/scala/config/MMIOAcceleratorConfigs.scala)
  - defines `NE16RocketConfig`

### External RTL wrapper

- [`ne16_chipyard_wrapper.sv`](rtl/ne16_chipyard_wrapper.sv)
  - packs the upstream NE16 TCDM and control interfaces for Chisel

</details>

<details>
<summary><strong>2. Upstream RTL and dependency resolution</strong></summary>

- [`Bender.yml`](Bender.yml)
  - declares the upstream NE16 RTL dependency graph
- [`Bender.lock`](Bender.lock)
  - records pinned dependency revisions
- [`Bender.local`](Bender.local)
  - applies local dependency overrides and compatible versions
- [`hci-fifo-interface.patch`](patches/hci-fifo-interface.patch)
  - fixes the generated HCI FIFO interface width
- [`testchipip-simdram-valid-write-data.patch`](patches/testchipip-simdram-valid-write-data.patch)
  - fixes the Verilator DRAM shim used by the integrated simulator

The actual upstream NE16, HCI, HWPE, and technology-cell sources are resolved
into generated build directories; they are not copied into this repository.

</details>

<details>
<summary><strong>3. C drivers and firmware interfaces</strong></summary>

### NE16 and DMA drivers

- [`ne16_driver.h`](../../tests/ne16_driver.h)
- [`ne16_driver.c`](../../tests/ne16_driver.c)
- [`ne16_dma.h`](../../tests/ne16_dma.h)
- [`ne16_dma.c`](../../tests/ne16_dma.c)

### Register-level and convolution tests

- [`ne16-descriptor.c`](../../tests/ne16-descriptor.c)
- [`ne16-conv.c`](../../tests/ne16-conv.c)
- [`ne16-conv3x3.c`](../../tests/ne16-conv3x3.c)
- [`ne16-dma.c`](../../tests/ne16-dma.c)
- [`ne16-dma-smoke.c`](../../tests/ne16-dma-smoke.c)
- [`ne16-double-buffer.c`](../../tests/ne16-double-buffer.c)
- [`ne16_test_scratch.h`](../../tests/ne16_test_scratch.h)
  - shared scratchpad test helper and guard checker
- [`ne16-test-scratch-host.c`](../../tests/ne16-test-scratch-host.c)
  - host-side validation of the scratchpad test helper

### DORY-oriented firmware tests

- [`ne16-dory-generated-tile.c`](../../tests/ne16-dory-generated-tile.c)
- [`ne16-dory-generated-layer.c`](../../tests/ne16-dory-generated-layer.c)
- [`ne16-dory-generated-chain.c`](../../tests/ne16-dory-generated-chain.c)
- [`ne16-dory-tiled-layer.c`](../../tests/ne16-dory-tiled-layer.c)
- [`dory-ne16-generated-layer-host.c`](../../tests/dory-ne16-generated-layer-host.c)
- [`dory-ne16-generated-chain-host.c`](../../tests/dory-ne16-generated-chain-host.c)
- [`dory-ne16-tiled-host.c`](../../tests/dory-ne16-tiled-host.c)

The complete firmware directory is [`tests/`](../../tests/), which also
contains unrelated Chipyard example programs.

</details>

<details>
<summary><strong>4. DORY compatibility layer</strong></summary>

These files provide the PULP/DORY-facing API used by generated kernels and
translate it to the Chipyard NE16 and DMA interfaces.

- [`dory_dma.h`](../../tests/dory_ne16_compat/dory_dma.h)
- [`dory_dma.c`](../../tests/dory_ne16_compat/dory_dma.c)
- [`dory_pipeline.h`](../../tests/dory_ne16_compat/dory_pipeline.h)
- [`dory_pipeline.c`](../../tests/dory_ne16_compat/dory_pipeline.c)
- [`dory_tiled_layer.h`](../../tests/dory_ne16_compat/dory_tiled_layer.h)
- [`dory_tiled_layer.c`](../../tests/dory_ne16_compat/dory_tiled_layer.c)
- [`pulp_nnx.h`](../../tests/dory_ne16_compat/pulp_nnx.h)
- [`pulp_nnx.c`](../../tests/dory_ne16_compat/pulp_nnx.c)
- [`pulp_nnx_util.h`](../../tests/dory_ne16_compat/pulp_nnx_util.h)
- [`pulp_nnx_util.c`](../../tests/dory_ne16_compat/pulp_nnx_util.c)
- [`ne16_hal.h`](../../tests/dory_ne16_compat/ne16_hal.h)
- [`network.h`](../../tests/dory_ne16_compat/network.h)
- [`pmsis.h`](../../tests/dory_ne16_compat/pmsis.h)
- [`monitor.h`](../../tests/dory_ne16_compat/monitor.h)
- [`net_utils.h`](../../tests/dory_ne16_compat/net_utils.h)

Open the complete directory here:
[`tests/dory_ne16_compat/`](../../tests/dory_ne16_compat/)

</details>

<details>
<summary><strong>5. Generated DORY sources and fixtures</strong></summary>

### Generated layers and chains

- [`tests/ne16-dory-generated/`](../../tests/ne16-dory-generated/)
  - imported/generated single-tile kernel
- [`tests/ne16-dory-generated-layer/`](../../tests/ne16-dory-generated-layer/)
  - generated layer, metadata, fixture data, and upstream DORY support files
- [`tests/ne16-dory-generated-chain/`](../../tests/ne16-dory-generated-chain/)
  - generated two-layer chain, per-layer cooperative plans, metadata, and
    upstream DORY support files

### Generated data and host fixtures

- [`ne16_dory_layer_data.c`](../../tests/ne16_dory_layer_data.c)
- [`ne16_dory_layer_data.h`](../../tests/ne16_dory_layer_data.h)
- [`dory-ne16-generated-host.h`](../../tests/dory-ne16-generated-host.h)
- [`generated chain test`](../../tests/ne16-dory-generated-chain.c)

Generated DORY code is intentionally kept under `tests/` rather than mixed
with the hand-written hardware or compatibility code.

</details>

<details>
<summary><strong>6. DORY generation and import scripts</strong></summary>

### Generation

- [`generate-dory-ne16-cooperative-layer.py`](scripts/generate-dory-ne16-cooperative-layer.py)
- [`generate-dory-ne16-cooperative-chain.py`](scripts/generate-dory-ne16-cooperative-chain.py)
- [`dory-ne16-chain-frontend.py`](scripts/dory-ne16-chain-frontend.py)

### Import

- [`import-dory-ne16-layer.py`](scripts/import-dory-ne16-layer.py)
- [`import-dory-ne16-cooperative-layer.py`](scripts/import-dory-ne16-cooperative-layer.py)
- [`import-dory-ne16-generated-tile.py`](scripts/import-dory-ne16-generated-tile.py)

### Generated-output validation

- [`dory_ne16_host_test.py`](scripts/dory_ne16_host_test.py)
- [`test-dory-ne16-generated-layer.py`](scripts/test-dory-ne16-generated-layer.py)
- [`test-dory-ne16-generated-chain.py`](scripts/test-dory-ne16-generated-chain.py)
- [`test-dory-ne16-tiled-layer.py`](scripts/test-dory-ne16-tiled-layer.py)
- [`test-import-dory-ne16-generated-tile.py`](scripts/test-import-dory-ne16-generated-tile.py)

</details>

<details>
<summary><strong>7. Simulation, lint, and regression scripts</strong></summary>

- [`fetch-bender.sh`](scripts/fetch-bender.sh)
- [`fetch-deps.sh`](scripts/fetch-deps.sh)
- [`standalone-lint.sh`](scripts/standalone-lint.sh)
- [`prepare-rtl.sh`](scripts/prepare-rtl.sh)
- [`build-chipyard-sim.sh`](scripts/build-chipyard-sim.sh)
- [`run-conv-test.sh`](scripts/run-conv-test.sh)
- [`test-fpga-storage.sh`](scripts/test-fpga-storage.sh)

The main test documentation is [`tests/README.md`](../../tests/README.md).

</details>

<details>
<summary><strong>8. FPGA, FireSim, and Vivado</strong></summary>

### FPGA-safe RTL

- [`ne16_storage.sv`](fpga/ne16_storage.sv)
  - explicit-enable replacements for clock-gated storage
- [`platform_env_2024.2.tcl`](fpga/platform_env_2024.2.tcl)
  - Vivado platform/environment settings

### FireSim and Vivado patches

- [`firesim-u250-vivado-2024.2.patch`](patches/firesim-u250-vivado-2024.2.patch)

### Project preparation and recovery

- [`prepare-firesim-u250-vivado.sh`](scripts/prepare-firesim-u250-vivado.sh)
- [`resume-firesim-u250-vivado.tcl`](scripts/resume-firesim-u250-vivado.tcl)
- [`firesim-u250-vivado-preflight.tcl`](scripts/firesim-u250-vivado-preflight.tcl)
- [`firesim-u250-hardware-preflight.tcl`](scripts/firesim-u250-hardware-preflight.tcl)

### Timing, constraints, and review

- [`audit-firesim-u250-constraints.tcl`](scripts/audit-firesim-u250-constraints.tcl)
- [`review-firesim-u250-vivado.tcl`](scripts/review-firesim-u250-vivado.tcl)

Operational instructions are in [`IDUN_RUNBOOK.md`](IDUN_RUNBOOK.md).

</details>

<details>
<summary><strong>9. Hardware verification testbenches</strong></summary>

- [`ne16-fpga-storage-tb.sv`](tests/ne16-fpga-storage-tb.sv)
  - storage-level FPGA replacement tests
- [`ne16-fpga-core-tb.sv`](tests/ne16-fpga-core-tb.sv)
  - standalone NE16 core tests with queued 1x1, 3x3, and DORY cases

The test runner is [`test-fpga-storage.sh`](scripts/test-fpga-storage.sh).

</details>

<details>
<summary><strong>10. Documentation and design notes</strong></summary>

- [`README.md`](README.md)
  - technical overview and validated behavior
- [`IDUN_RUNBOOK.md`](IDUN_RUNBOOK.md)
  - reproducible Idun/U250 and FireSim procedures
- [`DORY_INTEGRATION_PLAN.md`](DORY_INTEGRATION_PLAN.md)
  - DORY integration planning notes
- [`tests/README.md`](../../tests/README.md)
  - firmware test build and execution notes

</details>

## Conceptual dependency graph

```text
Chipyard configuration
        |
        v
NE16.scala
   |             \
   v              v
NE16DMA.scala   NE16BlackBox
   |
   v
NE16Scratchpad.scala <--- NE16 TCDM requests
   |
   v
DMA/NE16 test drivers
   |
   v
DORY compatibility layer
   |
   v
Generated DORY layer/chain
```
