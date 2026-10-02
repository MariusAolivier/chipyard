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

The integration maps NE16 control registers at `0x10030000`, DMA registers
at `0x10031000`, and a 64 KiB uncached shared scratchpad at `0x20000000`.

Run the bare-metal regression with:

```sh
scripts/run-conv-test.sh
```

The regression covers descriptor construction, hardware DMA, double-buffered
execution, the verbatim generated DORY tile, and the existing 1x1, 3x3, and
DORY-derived convolutions. The double-buffer test checks six distinct
fixture-derived jobs, both slot reuse and output ordering, scratchpad/DRAM
guards, and a hardware counter proving DMA scratchpad access during NE16
execution.

## Shared scratchpad arbitration

`NE16Scratchpad.scala` owns nine word-interleaved, byte-masked `SyncReadMem`
banks. Word `w` maps to bank `w % 9`, row `w / 9`.

CPU TileLink accesses, the DMA local port, and NE16 compete through a
whole-request round-robin arbiter. One client gets the shared memory access
slot per cycle. A continuously eligible requester waits for at most two
other grants; clients with a pending narrow response are not eligible.
NE16 still receives either all nine grants or none, and accepted NE16 reads
return all nine words on the next cycle, including back-to-back reads.

CPU and DMA writes acknowledge on the next cycle. Their reads issue at
cycle N, capture the synchronous SRAM output at the following edge, and
respond in cycle N+2. Each narrow port holds its response data until consumed,
so a stalled TileLink D response cannot corrupt its data or block other
clients. Only one narrow request per client can be outstanding.

There is no whole-operation `busy_o` exclusion. Since a wide NE16 request
touches all nine banks, DMA and NE16 accesses interleave rather than access
different banks simultaneously. Software must not modify buffers still owned
by NE16; arbitration does not enforce that ownership or add cache coherence
to the uncached scratchpad.

## Hardware DMA

`NE16DMA.scala` is a single-outstanding TileLink master connected through FBUS
to the system's coherent memory path. It copies between one scratchpad
endpoint and one external DRAM endpoint (32-bit addresses at or above
`0x80000000`). It does not copy arbitrary peripheral registers or support
DRAM-to-DRAM transfers.

A descriptor specifies line length, rows, planes, and independent source and
destination row/plane strides. Both complete address spans are checked before
starting. Rows/planes must not overlap. Arbitrarily aligned byte ranges are
handled with naturally aligned 1-, 2-, 4-, or 8-byte TileLink transfers and
scratchpad byte masks, without overwriting neighboring bytes. There are no
bursts, interrupts, or hardware descriptor queues yet.

| Offset | Register | Meaning |
|---|---|---|
| `0x00` | Command | Write 1 to start; write 2 to acknowledge completion/error |
| `0x04` | Status | Bit 0 busy, bit 1 done, bit 2 error |
| `0x08`, `0x0c` | Source, destination | Absolute byte addresses |
| `0x10` | Line bytes | Nonzero bytes per row |
| `0x14`, `0x18` | Rows, planes | Nonzero counts; use 1 for unused dimensions |
| `0x1c`, `0x20` | Source/destination row strides | Byte spacing between row starts |
| `0x24`, `0x28` | Source/destination plane strides | Byte spacing between plane starts |
| `0x2c` | Error | 1 invalid geometry/span; 2 denied/corrupt response; 3 unsupported external address/size |
| `0x30` | Bytes copied | Bytes committed by the most recent descriptor |
| `0x38`, `0x3c`, `0x40` | CPU, DMA, NE16 grants | Cumulative scratchpad grant counts |
| `0x44` | Contention | Cycles with multiple eligible scratchpad requesters |
| `0x48` | DMA/NE16 overlap | DMA scratchpad grants while NE16 is busy |

Configuration and command writes backpressure while busy; status reads remain
available. Completion/error is sticky until acknowledgment or a new start.
Bus errors stop the descriptor, but already committed bytes are not rolled
back. Counters are 32-bit, wrap modulo 2^32, and reset with hardware reset,
not NE16's software clear.

`tests/ne16_dma.h` exposes checked submit/poll/wait APIs and memory fences.
`tests/dory_ne16_compat/dory_dma.h` preserves the existing generated DMA API,
now backed by real hardware rather than synchronous CPU copies. Its bounded
software queue is advanced by service/poll/wait; hardware executes a complete
strided descriptor autonomously.

## Single-core double buffering

`tests/dory_ne16_compat/dory_pipeline.h` provides a cooperative two-slot
scheduler for a single Rocket core. A preparation callback supplies each
job's NNX descriptor, input/weight/normalization transfers, and output
transfer. Slots must be disjoint, fit in the scratchpad, and contain the
job's complete footprint, including NE16 padding/realignment guards.

The lifecycle is free -> loading -> ready -> computing -> storing -> free.
Next-slot loads and previous-slot stores overlap computation; a slot cannot
be reused until its output DMA completes. Errors are explicit and draining
is required before releasing buffers after a failed operation.

This is an explicit single-core scheduling API, not a replacement for the
entire GAP9/PMSIS cluster runtime. The existing imported one-tile DORY source
remains verbatim and uses hardware DMA. Its three-worker PMSIS shim still
runs workers sequentially; multi-tile generated worker functions must be
adapted to the cooperative API rather than assuming parallel CPU workers.
Dependent layers may only be scheduled after their input-producing output
transfer has completed.

Focused hardware tests can be run from the Chipyard root:

```sh
java -jar scripts/sbt-launch.jar \
  'chipyard/testOnly chipyard.ne16.NE16ScratchpadSpec chipyard.ne16.NE16DMASpec'
```

These require a Linux Verilator/firtool environment. On Idun, both these tests
and simulator builds/runs must execute inside Slurm allocations.
