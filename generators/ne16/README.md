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
DPI shim from dereferencing an invalid AXI write-data array when `w_valid` is
low.

The integration maps NE16 control registers at `0x10030000` and a 64 KiB
uncached shared scratchpad at `0x20000000`.

Run the bare-metal regression with:

```sh
scripts/run-conv-test.sh
```

The regression covers both 1x1 and 3x3 convolution. Each test queues two real
operations without resetting NE16 and checks every 32-bit output against an
independently computed software reference.
