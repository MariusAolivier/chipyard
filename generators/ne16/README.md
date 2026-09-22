# Chipyard NE16 integration

This directory integrates the upstream PULP NE16 RTL without modifying it.

## Pinned inputs

- Chipyard: `e602d917dcc495c58cabe906535e411707096c9c`
- NE16: `6bd727206d49e5febbe4d1037eda91d064d203e1`
- PULP-NNX software reference: `c4f6ba351e30b31125baba35896db394804d819d`
- Bender: `v0.32.1`
- Bender Linux x86-64 archive SHA-256:
  `59a36723b056a06b266dc68d4ceedcd0aa17a1c096e8c2ea512af264ff6a13f6`

`Bender.lock` records immutable revisions for NE16's transitive RTL
dependencies. Run Bender from this directory so those exact revisions are
used.

`Bender.local` pins HCI to the NE16-declared `v1.0.6` revision and pins
compatible patch releases of HWPE Stream (`v1.6.4`) and HWPE Control
(`v1.6.2`) through HTTPS overrides. It also pins their technology-cell
dependencies. Later HCI
`1.0.x` releases changed internal interface types while retaining a compatible
semantic version, so unconstrained resolution does not compile this NE16 RTL.

## Standalone RTL validation

Install the pinned Bender binary and lint the complete NE16 design:

```sh
scripts/fetch-bender.sh
scripts/fetch-deps.sh
scripts/standalone-lint.sh
```

`patches/hci-fifo-interface.patch` is applied to a generated copy of HCI's
`hci_core_sink.sv`. HCI v1.0.6 incorrectly instantiates a legacy fixed-width
HWPE FIFO on its 288-bit HCI path. The overlay selects HCI's own
width-parameterized FIFO. The pinned upstream repositories remain untouched.

This FIFO overlay is separate from `common_cells` namespace issues that can
occur when unrelated PULP and Chipyard RTL dependency graphs are compiled
together. The FIFO patch fixes an HCI interface-width incompatibility; it does
not resolve duplicate `common_cells` modules or macros. This standalone NE16
flow does not apply a `common_cells` namespace overlay, so such collisions
must be handled separately if additional external RTL is added to the
simulator.

`rtl/ne16_chipyard_wrapper.sv` exposes the upstream nine-lane TCDM and HWPE
control interfaces as packed vectors suitable for a Chisel BlackBox. It keeps
HWPE's native 16-bit transaction ID internally and ties it to zero because the
Chipyard bridge allows only one outstanding control request.

## Chipyard simulation

After sourcing Chipyard's `env.sh`, build the integrated Rocket simulator:

```sh
export BENDER=/path/to/pinned/bender
scripts/build-chipyard-sim.sh
```

The build script applies `patches/testchipip-simdram-valid-write-data.patch`
to the checked-out `testchipip` submodule. This prevents its Verilator DRAM
DPI shim from dereferencing an invalid AXI write-data array when the write
handshake is not active. It also normalizes the generated fixed byte arrays
because cached TestChipIP resources may otherwise retain the original
descending-range declarations.

The integration maps NE16 control registers at `0x10030000` and a 64 KiB
uncached shared scratchpad at `0x20000000`.

Run the bare-metal regression with:

```sh
scripts/run-conv-test.sh
```

The regression covers both 1x1 and 3x3 convolution, the original DORY-derived
single-layer fixture, and the generated three-layer NE16 network. It runs the
host-side importer rejection/reproducibility tests before building the
firmware. Each generated layer also has a standalone target; the network test
executes the generated entry points in order and checks both intermediate
outputs and the final output.

## Generating the three-layer fixture

The checked-in network configuration is derived from
`config/config_single_layer0_ne16_chipyard.json` and uses the pinned DORY
commit recorded in `config/config_three_layer_ne16_chipyard.json`. It defines
three fused, quantized 3x3 convolutions so every computational layer is
eligible for the NE16 path. The Chipyard hardware description sets L1 to
64 KiB and reserves no PULP core stacks; the importer additionally limits the
guarded live allocation to 63,232 bytes.

Generation must run on an allocated Idun node:

```sh
python3 generators/ne16/scripts/generate-dory-ne16-generated-network.py \
  --dory-root /cluster/work/mariusao/dory \
  --config generators/ne16/config/config_three_layer_ne16_chipyard.json \
  --output-dir tests/ne16-dory-generated-network
python3 generators/ne16/scripts/test-import-dory-ne16-generated-network.py
```

The generator copies DORY to a temporary directory, overlays
`hardware_description_chipyard_ne16.json`, and never modifies the DORY
checkout. It emits a manifest containing the source/config/parameter/reference
hashes, layer order, dimensions, tile grids, and guarded L1 regions. The
network importer rejects generic `pulp_nn_*` or cluster calls, missing NNX
calls, reordered or truncated artifacts, invalid dimensions, and physical or
guarded L1 overflows.
