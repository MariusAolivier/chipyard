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

`scripts/prepare-rtl.sh` retains its default Verilator manifest. With
`--vivado`, it instead generates `build/vivado/ne16-vivado.tcl` using
Bender's Vivado synthesis target, including Xilinx clock cells and target
defines. It adds the `NE16BlackBox` wrapper and substitutes the same HCI FIFO
overlay in a separate build directory, without changing the upstream sources
or the Verilator manifest. The FPGA manifest also replaces the four
clock-gated SCM implementations with `fpga/ne16_storage.sv`.
`--fpga-sim` selects the native FPGA source targets and the same storage
replacement in `build/fpga-sim/ne16-fpga.f`, for functional comparison with
the default ASIC manifest.

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
execution, the verbatim generated DORY tile, the automatically imported
multi-tile layer, and the existing 1x1, 3x3, and
DORY-derived convolutions. The double-buffer test checks six distinct
fixture-derived jobs, both slot reuse and output ordering, scratchpad/DRAM
guards, and a hardware counter proving DMA scratchpad access during NE16
execution.

## FireSim U250 bring-up

`firechip.chip.FireSimNE16RocketConfig` wraps `chipyard.NE16RocketConfig`
with the FireSim bridges. The existing build recipe is
`alveo_u250_firesim_ne16_rocket_singlecore_no_nic`, using
`BaseXilinxAlveoU250Config`, a 60 MHz **host FPGA** clock, and the `TIMING`
strategy. This is a FireSim-hosted simulation of the target SoC, not a
standalone native FPGA SoC deployment.

Regenerate the RTL and driver together in fresh directories after activating
the Chipyard/FireSim environment. From the repository root:

```sh
ROOT=$(pwd)
tag=ne16-u250-$(date +%Y%m%d-%H%M%S)
GENERATED_DIR="$ROOT/sims/firesim/sim/generated-src/$tag"
OUTPUT_DIR="$ROOT/sims/firesim/sim/output/$tag"
CL_DIR="$ROOT/sims/firesim/platforms/xilinx_alveo_u250/cl_$tag"
cd "$ROOT/sims/firesim/sim"
make replace-rtl \
  PLATFORM=xilinx_alveo_u250 TARGET_PROJECT=firesim \
  TARGET_PROJECT_MAKEFRAG="$ROOT/generators/firechip/chip/src/main/makefrag/firesim" \
  DESIGN=FireSim TARGET_CONFIG=FireSimNE16RocketConfig \
  PLATFORM_CONFIG=BaseXilinxAlveoU250Config \
  GENERATED_DIR="$GENERATED_DIR" OUTPUT_DIR="$OUTPUT_DIR" \
  fpga_work_dir="$CL_DIR"
```

The checked-out FireSim template lacks a Vivado 2024.2 block-design library,
and its Aurora 12.0 references do not exist in the installed 2024.2 catalog.
The task-scoped overlay copies the three affected definitions, selects
Aurora 13.0, and reuses the unchanged interface/connection definitions.
The host clock wizard uses `PRIM_SOURCE No_buffer` because DDR0's UI clock,
not an external clock pin, drives its input. This preserves the DDR-derived
clock's timing instead of redefining it as an internal primary clock.
It does not modify the FireSim source template or disable IP-catalog,
block-design, synthesis, or implementation checks. Preparation deliberately
rejects an existing 2024.2 library instead of overwriting a native port.
It also generates and loads the pinned NE16 Vivado source manifest.
`NE16BlackBox` has externally supplied RTL: generating the Chisel/Golden Gate
design alone does not supply its wrapper or transitive RTL dependencies.

```sh
VIVADO_ROOT=/path/to/Vivado/2024.2
source "$VIVADO_ROOT/settings64.sh"
export NE16_U250_BOARD_REPO="$VIVADO_ROOT/data/boards"
bash "$ROOT/generators/ne16/scripts/prepare-firesim-u250-vivado.sh" "$CL_DIR"
vivado -mode batch -nolog -nojournal \
  -source "$ROOT/generators/ne16/scripts/firesim-u250-vivado-preflight.tcl" \
  -tclargs "$NE16_U250_BOARD_REPO" "$CL_DIR/design/ne16-vivado.tcl"
"$ROOT/sims/firesim/platforms/xilinx_alveo_u250/build-bitstream.sh" \
  --cl_dir "$CL_DIR" --frequency 60 --strategy TIMING --board au250
```

The preflight checks the installed U250 1.3 board, enumerates required
IP versions, and optionally elaborates `NE16BlackBox` with explicit lane/data
width checks. RTL elaboration is not synthesis, timing, or MAC validation.
The implementation outputs are
`$CL_DIR/vivado_proj/firesim.bit` and
`$CL_DIR/vivado_proj/reports/final_timing_summary.rpt`. Check completed
implementation, fresh clock/methodology and bus-skew reports, and matching
RTL/driver/bitstream provenance before running firmware. The driver's
`+check-fingerprint` checks the common FireSim presence value, not a
design-specific identity; it cannot replace provenance checks.
The corrected-clock Vivado 2024.2 flow has produced a bitstream with the
current DMA design and fresh reports with no failing setup, hold, or
pulse-width endpoints. All 28 bus-skew checks pass, with minimum slack
2.969 ns. The clock-wizard primary-clock warnings are resolved. The remaining
vendor PCIe scoped-clock warning was traced to the reference-clock probation
logic: both affected clocks have no register users outside XDMA. The reset
inputs terminate in vendor synchronizers/MMCM reset, and the DDR reset output
comes from the MIG initialization state machine, including its 200 us/500 us
reset sequence. These reset/protocol interfaces are not synchronous data
interfaces requiring ordinary I/O-delay specifications; no arbitrary timing
exceptions or warning suppression were added. The selected Idun U250 has been
programmed with the corrected volatile bitstream and reconnected over PCIe.
The native fingerprint check and descriptor firmware pass on that card.
The first convolution firmware currently times out with status `0x00000101`:
CPU scratchpad input readback is correct and 114 accelerator grants are
observed, but both output buffers retain their sentinels. A separate DMA probe
did not finish before the host rebooted. On recovery, the FPGA node lacked
both `/cluster/work` and `/cluster/projects`; the reboot cause and the current
volatile image are unverified. On-board MAC, DMA, double-buffering, and chain
validation remain incomplete. Both shared filesystems were accessible again
on October 8. Reestablish image provenance before any further board execution.

### Confirmed FPGA clock-cell incompatibility

**The unmodified native FPGA sources and the previously built image are not
functionally safe to deploy.**
Its pinned `tech_cells_generic/src/fpga/tc_clk_xilinx.sv` implements
`tc_clk_gating` as `assign clk_o = clk_i`, ignoring the enable inputs.
NE16/HWPE's latch-based configuration register file uses those clock gates
as per-word/per-byte write enables, not just power-saving controls. Removing
the gates therefore overwrites every configuration word/context with the
last sampled write data. The input buffer, accumulator memories, and SCM
FIFO storage also depend on functional clock gating.

The standalone `tests/ne16-fpga-core-tb.sv` repeats the two queued operations
from `tests/ne16-conv.c`, with atomic nine-lane grants and one-cycle memory
read responses. The ASIC source configuration preserves the weight/input
pointers and produces all 18 expected output words. With the native FPGA
sources, both pointers instead read back as the final configuration word,
`0x00408047`; the first TCDM address becomes `0x00408044` instead of
`0x20001000`. Strict mode rejects that invalid pointer. The optional
`+wrap-invalid-address` diagnostic models low-address aliasing and reproduces
the busy/full `0x00000101` timeout; it is not an input-validation bypass for
normal use or a cycle-exact model of the full SoC.

Compile the probe with Verilator's `--binary --timing`, top module
`ne16_fpga_core_tb`, the corresponding NE16 source manifest, and
`+define+SYNTHESIS`. The native comparison uses the pinned
`rtl`, `fpga`, `xilinx`, `synthesis`, and `vivado` Bender targets with the same
HCI overlay and BlackBox wrapper. Changing only the native manifest's clock
cell file to the functional generic implementation restores correct pointer
readback and both numerical results in simulation. This isolates the source
incompatibility; it is **not** a validated FPGA implementation or a new
bitstream. A production port must preserve the functional write/clock-enable
semantics in FPGA-safe RTL, then repeat implementation, timing review, and
board validation. Increasing the software poll limit cannot fix corrupted
configuration words. The descriptor firmware only constructs descriptors in
CPU memory, so its pass did not exercise this register-file write path.

The FPGA-only storage overlay keeps the original module interfaces but uses
ordinary-clock flip-flops with explicit write enables. It preserves
configuration byte masks, clear/reset behavior, sampled-data broadcasts,
scalar/wide accumulator masks, registered reads, and held upper read lanes.
It does not substitute ASIC clock gates into the FPGA flow or change the
Scala control bridge, scratchpad arbiter, or DMA.

Run the storage/core comparison inside an allocated build environment:

```sh
bash generators/ne16/scripts/test-fpga-storage.sh /path/to/fresh-results
```

The regression compares both source configurations against storage scoreboards
at accumulator width factors 4 and 8, then runs two queued 1x1/3x3 jobs and
two serialized quantized DORY fixture jobs, each with and without
whole-request backpressure. Serialization matches the existing DORY pipeline,
which overlaps DMA with one active NE16 computation. Queuing both quantized
fixtures directly exposes a numerical mismatch in the unmodified ASIC RTL;
that separate baseline limitation is not attributed to the FPGA storage port.
It checks all 24 configuration words, numerical outputs, unchanged inputs and
weights, and every scratchpad guard byte. The ASIC latch scoreboard separates
reads from writes to avoid zero-delay gate/latch scheduling races; the FPGA
scoreboard additionally checks simultaneous read/write behavior against the
registered read-before-write contract. The DORY reference is the committed
8x8x16 fixture, not a reference calculated by the replacement RTL.
On October 8, all four storage runs and twelve core cases passed on the
pinned ASIC and repaired native FPGA source sets. The default ASIC manifest
was byte-identical before and after preparation. These standalone checks do
not establish full-SoC or on-board operation. The fresh repaired-storage
`TIMING` build at a 60 MHz host clock completed routing with setup slack
of +0.057 ns and hold slack of +0.010 ns, with zero failing setup, hold, or
pulse-width endpoints. All 28 routed bus-skew checks passed, with minimum
slack +3.182 ns. The build completed with status 0, recorded build-input
hashes matched, and both bitstream copies had SHA-256
`d6238efc1e06785fe91a98e2849dd60adf5288f1e9487c6f801e014661e376e8`.
The October 8 allocation was released before the authorized cutoff, without
programming or changing XDMA. On October 10, the repaired image passed the
primitive-level storage/clock review: zero mapped NE16 latches and global
buffer drivers for active clock pins. Fresh timing checks reported no
unconstrained internal endpoints, missing clocks, or timing failures.
The clock/reset audit found no registers outside XDMA using either clock in
the vendor scoped clock-group `TIMING-54` warning, and confirmed the reset
input synchronizers and DDR calibration reset-output source. The warning
remains visible. The generated target-clock multicycle exceptions are
one-cycle setup and zero-cycle hold, not relaxed multi-cycle requirements.
The detailed CDC report has no critical violations: 360 synchronized-bit
and 46 synchronized-reset informational entries, plus 415 clock-enable
controlled crossing warnings. Violation-object properties resolved every
endpoint: both endpoints of each warning are confined to vendor debug
FIFOs (4), AXI width-converter FIFOs (122), XDMA calibration counters (288),
or the AXI clock-converter FIFO (1), with none in NE16. This completed the
review for bounded physical bring-up without waiving the warnings.
Both hierarchical and mapped-leaf `report_cdc -cells` probes returned empty
reports, so they are not used to attribute the encrypted crossings.
The target clock bridge is a synchronous-enable `BUFGCE`
driven by the host MMCM. Progressive physical validation remains incomplete.
Selected-card hardware preflight and guarded volatile programming of
`0000:d8:00.0` completed successfully after rechecking the build inputs,
all six firmware hashes, and 55 canonical firmware source files. XDMA
automatically reloaded on PCIe rescan; both cards were bound with polling
enabled, and the selected card resolved to XDMA instance 1. Permissions
were restored. The repaired image's common FireSim fingerprint check passed
(`0x46697265`); this is presence evidence, not design-specific identity.
Descriptor firmware and both raw convolution regressions passed on the
repaired image: two queued 1x1 jobs and two queued 3x3 jobs each matched
their software references. This resolves the previously observed raw
convolution timeout for these cases. The subsequent bounded DMA run did not
record completion or a PASS. Slurm ended allocation `25641608` as
`NODE_FAIL 1:0` at 12:44:45 UTC on October 10, with `idun-12-22`
`DOWN+RESERVED+NOT_RESPONDING`. The saved DMA log contains only driver
startup and "Commencing simulation"; there is no firmware result or timeout
status. The node-failure cause is unknown, and this does not establish a DMA
or storage fault. Double-buffer and generated-chain board tests were not
reached. The watch was stopped on allocation loss; current volatile-image
and driver state require revalidation after node recovery.
The node subsequently recovered, with a boot time of 12:42:59 UTC and
Slurm restart at 12:50:03 UTC. A new user-authorized, exclusive 30-minute
recovery allocation (`25642252`) found both cards and shared filesystems
accessible, XDMA polling enabled with no users, and the previous owned
hardware-server PID absent. System journals and persistent crash records
are not readable by the FPGA user's permitted commands; administrator
evidence is needed to establish the reboot cause. Recovery is limited to
restoring image provenance and repeating the previously passing checks,
not blindly rerunning DMA.
Within that recovery allocation, the selected JTAG identity, all five
recorded build inputs, all six firmware hashes, and the repaired bitstream
hash were rechecked. Volatile programming of only `0000:d8:00.0` completed
with status 0. PCIe rescan automatically restored XDMA in polling mode;
the selected card again resolved to instance 1. After restoring permissions,
the common fingerprint, descriptor test, and both queued raw convolution
tests passed again at 13:02 UTC. No DMA, double-buffer, or generated-chain
workload was attempted during recovery.
The boot ID remained unchanged through these checks. The owned hardware
server binary and its two launcher processes were confirmed absent after
cleanup, with localhost port 3121 free. Recovery allocation `25642252`
completed with Slurm status `COMPLETED 0:0` at 13:03:50 UTC, after 10 minutes
41 seconds, and the recovery watch was cleared. Full DMA/chain board
validation remains blocked pending investigation of the earlier reboot.
The separately authorized focused DMA retry used allocation `25642265` and
an isolated firmware image that submitted only one aligned 16-byte
DRAM-to-scratchpad transfer. Image provenance, the selected card, and the
descriptor/1x1/3x3 baselines all passed again. The smoke driver reached
FireSim startup and `Commencing simulation`, but never reached its first
firmware checkpoint before Slurm reported `NODE_FAIL 1:0` for `idun-12-22`
after 9 minutes 16 seconds. Its step and extern records were cancelled;
there was no DMA PASS, status result, or firmware MMIO checkpoint. No
additional retry was made. A later read-only node check showed a subsequent
boot on October 12, but that does not establish the cause of the October 10
failure.
Timing figures for the earlier image do not establish correctness of this
storage port.

For an existing failed synthesis project, `scripts/resume-firesim-u250-vivado.tcl`
loads `$CL_DIR/design/ne16-vivado.tcl`, verifies the U250 part, board, top, and
60 MHz host clock, then reruns the stock `TIMING` synthesis/implementation
flow. Preserve failure logs before using it, and only resume a task-owned
project with matching current RTL/driver provenance. It does not bypass
synthesis, implementation, or timing checks.

For a completed task-owned project with the old primary-clock setting, append
`--repair-host-clock` to its `-tclargs`. The script first verifies DDR0's UI
clock wiring and snapshots the original bitstream, MCS, project, reports,
block design, and host-clock IP configuration under
`vivado_proj/pre_host_clock_repair_<SLURM_JOB_ID>`. It then regenerates the
corrected clock IP and reruns synthesis/implementation; it does not simply
delete a timing warning from the routed design. The normal resume path rejects
the old setting.

Run `scripts/review-firesim-u250-vivado.tcl` with `-tclargs "$CL_DIR"` through
Slurm to regenerate verbose timing checks, methodology, clocks, clock
interactions, exceptions, bus skew, unconstrained-path timing, and detailed CDC reports in
`vivado_proj/review_<SLURM_JOB_ID>`. Review actual clock and reset endpoints,
including vendor-scoped exceptions; zero failing setup/hold endpoints alone
does not establish that the constraints are correct.
The review requires a completed bitstream and nonnegative setup, hold, and
pulse-width statistics. It also records the four FPGA storage module families
in `ne16_storage.rpt` and requires the retained configuration, input-buffer,
and accumulator hierarchies. The FIFO SCM can be absent from the optimized
hierarchy; this is reported explicitly, and the primitive/clock checks still
cover the entire mapped NE16. The review rejects mapped NE16 latches or active
clock pins not driven by global clock buffers. Constant DSP clock pins are
accepted only for verified direct combinational multiplier/cascade modes,
with datapath/control pipelines disabled and unused C/D/AD enables tied low;
other tied clocks are rejected.
The review also saves `cdc_objects.rpt` with CDC violation properties,
including startpoint and endpoint pins hidden in the formatted CDC report.
Inspect these properties to establish crossing ownership and topology;
successful report generation is not CDC acceptance.

`scripts/audit-firesim-u250-constraints.tcl`, also run through Slurm with
`-tclargs "$CL_DIR"`, records the host/DDR/PCIe clock properties, registers
using the PCIe reference/internal clocks outside XDMA, and routed reset
connectivity in `vivado_proj/constraint_audit_<SLURM_JOB_ID>`. Use this evidence
with the vendor constraints and reset RTL; reset names alone do not justify
ignoring missing I/O delays.

On Idun, run builds through Slurm. Physical access uses partition `CPUQ`,
account `share-ie-idi`, and reservation `fpga`; always specify
`--nodes=1 --ntasks=1` and acquire an exclusive FPGA-node allocation before
programming. Use the installed FPGA database and one explicit BDF. XDMA
module removal/reloading affects both cards on the host. Do not invoke
`firesim-remove-dev-shm`, which deletes all shared-memory objects, or blindly
use the example FireSim deployment configurations.

PCIe rescan can automatically reload XDMA. Check the actual module binding
and `poll_mode=1` before invoking the non-idempotent load helper; an `insmod`
"File exists" response does not mean programming failed. Restore the selected
BDF's sysfs/device permissions after rescan, and resolve the XDMA instance
from that BDF rather than assuming `/dev/xdma0` is the selected card.

Before programming, start a task-owned hardware server only if port 3121 is
unused, then run `scripts/firesim-u250-hardware-preflight.tcl` through the
exclusive allocation with `-tclargs <target-UID>`. It requires exactly one
matching JTAG target and one U250 device and prints
`SELECTED_U250_TARGET_PRESENT`. A zero process exit without that evidence is
not a successful device check.

For bare-metal execution, give the ELF to the native driver's TSI/FESVR
loader as a positional argument, with host options inside
`+permissive ... +permissive-off`. Do not reuse Verilator's
`+loadmem=<ELF>` convention: FireSim's generic memory-file initialization is a
different path. The TSI bridge uses its load-memory widget to transfer ELF
segments when available. Validate the descriptor/boot path before proceeding
to convolution, DMA, double buffering, and the generated chain.

When wrapping the driver in `timeout` inside an interactive subshell or loop,
use `timeout --foreground`: the UART driver configures the controlling terminal,
and a separate background process group can stop on `SIGTTOU` before producing
any output. Run each firmware in its own log directory because the driver
creates statistics files in its working directory.
`tests/ne16-conv.c` reports accelerator status, CPU/NE16 scratchpad grant
counters, input readback, and output sentinels on its existing timeout path
to distinguish a dead interface from an incomplete accelerator transaction.

## Shared scratchpad arbitration

`NE16Scratchpad.scala` owns nine word-interleaved, byte-masked `SyncReadMem`
banks. Word `w` maps to bank `w % 9`, row `w / 9`.

CPU TileLink accesses, the DMA local port, and NE16 compete through a
whole-request round-robin arbiter. One client gets the shared memory access
slot per cycle. A continuously eligible requester waits for at most two
other grants; clients with a pending narrow response are not eligible.
NE16 still receives either all nine grants or none, and accepted NE16 reads
return all nine words on the next cycle, including back-to-back reads.
Grants acknowledge accepted requests, not idle arbiter readiness: they are
zero when NE16 has no request. PULP's HCI FIFO depends on this when it
temporarily suppresses requests because its incoming FIFO is full; an
unsolicited grant can otherwise discard a queued read and stall computation.

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
transfer. Pipeline transfers use native `ne16_dma_config_t` descriptors with
independent source/destination strides, admitted through
`dma_transfer_enqueue_config`. The legacy generated `DmaTransferConf` API and
its existing layout behavior remain unchanged. Slots must be disjoint, fit in the
scratchpad, and contain the job's complete footprint, including NE16
padding/realignment guards.

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

## Complete multi-tile DORY-compatible layer

`tests/dory_ne16_compat/dory_tiled_layer.h` adapts one complete HWC 3x3,
stride-1, same-padded BN/ReLU convolution to the cooperative scheduler. It
supports the proven uint8/packed-8-bit-weight, 16-input-channel, shift-11,
weight-offset-minus-128 mode, with output channels in groups of 16.
The weight blob contains all packed filters, followed by all int32 scales
and then all int32 biases. Source, weight and destination tensors must be
disjoint, stay in DRAM, and remain alive until completion or draining.

Each job uses at most 12x12 output pixels and 16 output channels. Interior
tiles load real one-pixel halos; only global image edges use NE16 padding.
Unequal DMA row strides pack cropped inputs into local memory, and a 3D
descriptor scatters packed output pixels into the full HWC destination.
Both 16-KiB slots include prefix/read-ahead guards and cannot be reused
before their output transfer finishes.

`ne16-dory-tiled-layer` extends the committed DORY 32x32x16 fixture to one
32x32x48 convolution by permuting complete filters and matching scale/bias
entries into three output-channel groups. Its full 72,832 tensor bytes exceed
the 65,536-byte scratchpad even without guards. It executes 27 jobs covering
corners, edges, interior halos, and 8-pixel spatial tails. Every one of the
49,152 output bytes is compared with the corresponding channel of DORY's
original full-image reference; no tiled CPU kernel supplies the reference.
The regression checks complete coverage, both-slot reuse, immutable sources,
DRAM guards, all scratch bytes (including stale larger-tile tails), and actual
DMA grants during NE16 execution.

This is an explicit adaptation of a DORY-derived layer, not a newly generated
upstream multi-worker function or automatic support for other operators.
The imported generated source remains verbatim. Run the focused firmware with
the same simulator as the baseline suite:

```sh
cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug
cmake --build tests/build --target ne16-dory-tiled-layer
sims/verilator/simulator-chipyard.harness-NE16RocketConfig \
  +permissive +loadmem=tests/build/ne16-dory-tiled-layer.riscv \
  +permissive-off tests/build/ne16-dory-tiled-layer.riscv
```

The canonical regression runner allows 90 minutes for this complete-image
test and 30 minutes for each smaller test. `TIMEOUT` overrides both defaults;
`TILED_TIMEOUT` overrides the complete-image limit specifically. Full scratch
initialization/checking and repeated tiles are intentionally not skipped.

The host suite checks all output-byte coverage and input-halo neighbors,
192 geometry shapes, native DMA group lifetimes, unequal endpoint strides,
and asynchronous scheduler timings:

```sh
python3 generators/ne16/scripts/test-dory-ne16-tiled-layer.py
```

It uses the actual DMA validator but constructor/transport mocks, so it does
not replace the integrated simulator's numerical output and overlap checks.
The same command maps the real scratch guard helper at its fixed address
and checks all 65,536 individual byte corruptions and restorations, all 256
fill values, adjacent word-boundary faults, and unaligned expected buffers.
The helper compares complete 64-bit words on success and diagnoses individual
bytes only on mismatch; no scratch bytes are omitted.

## Automatic DORY cooperative generation/import

`scripts/generate-dory-ne16-cooperative-layer.py` runs the upstream NE16
single-layer frontend and imports its output without manually adapting tiles
or DMA descriptors. DORY must be pinned to
`add0d9c1be889b5f802b2606ced8c59acff8aa02`, with an already working Python
environment. The wrapper verifies the checkout revision and rejects modified
tracked generator sources other than `HW_description.json`. It stages the
configuration, raw tensors, and generated application privately; it does not
overwrite the DORY checkout's hardware description. That description is
recorded byte-for-byte alongside the original layer source and headers.

The supported mode remains uint8 HWC, 16 input channels, 3x3 stride-1
same padding, packed 8-bit weights, shift-11 BN/ReLU, and complete groups of
16 output channels. The importer checks the generated kernel, tile lattice,
input geometry, full-tensor DMA strides, normalization offsets, NNX mode,
and preparation helper. Unsupported modes and malformed inputs fail before
output publication. Output directories must be fresh; existing fixtures are
never silently replaced.

**This is lowering/retiling, not direct execution of DORY's three-worker
function.** The binding calls the original `execute_prepare` helper and NNX
primitives, while automatically emitting Chipyard's guarded 12x12x16 jobs
and native DMA recipes. It uses the cooperative scheduler rather than
sequentially pretending to run PMSIS loader/executor/storer workers.
The raw generated worker source is retained unchanged as provenance, not
linked into this firmware. Copied headers are system includes so their unused
debug helpers remain byte-identical without weakening checks on our code.

`tests/ne16-dory-generated-layer` is a genuinely generated 32x32x48 layer,
not a permutation of the earlier reference. DORY chose four 9x32x48 body
tiles with a 5-row last tile; the importer emits 27 spatial/channel jobs.
Its 72,832 tensor bytes exceed the 64-KiB scratchpad. The firmware compares
all 49,152 output bytes against DORY's independent full-layer reference and
checks coverage, source integrity, guards, slot reuse, and actual hardware
DMA/NE16 overlap. Metadata records both tile counts, configuration and plan
hashes, and every original-source/tensor hash. Original artifacts and generated
fixture files retain their exact bytes across Git checkouts.

The integrated `NE16RocketConfig` Verilator run passed all 49,152 output bytes,
coverage, source integrity, and every scratch/DRAM guard. It completed 27 jobs
with slot reuse counts 14/13 and 4,720 DMA/NE16 overlap grants. The observed
run finished successfully in 1:02:31; exact timing and grant counts may vary.

Generate into a new directory using the checked-in configuration. To reproduce
the recorded upstream tile selection, the DORY checkout must use the hardware
description saved under the fixture's `upstream/HW_description.json`; the
wrapper records the description actually used rather than installing it:

```sh
python3 generators/ne16/scripts/generate-dory-ne16-cooperative-layer.py \
  --dory-root "$DORY_ROOT" --python "$DORY_ROOT/.venv/bin/python" \
  --config generators/ne16/config/config_single_layer0_ne16_cooperative.json \
  --config-name generators/ne16/config/config_single_layer0_ne16_cooperative.json \
  --output-dir /tmp/ne16-generated-layer
```

Heavy generation, builds, and simulation on Idun belong in Slurm allocations.
The importer can also consume an existing generated directory through
`scripts/import-dory-ne16-cooperative-layer.py`; `--help` lists the required
raw tensor, configuration/hash, revision, and hardware-description arguments.
DORY's `hex/inputs.hex` and `hex/BNReluConvolution0_weights.hex` are raw binary
despite their extensions; `out_layer0.txt` is the decimal full-layer oracle.
Configuration hashing normalizes CRLF to LF; original source and tensor
hashes do not normalize bytes.

```sh
python3 generators/ne16/scripts/test-dory-ne16-generated-layer.py
cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug
cmake --build tests/build --target ne16-dory-generated-layer
sims/verilator/simulator-chipyard.harness-NE16RocketConfig \
  +permissive +loadmem=tests/build/ne16-dory-generated-layer.riscv \
  +permissive-off tests/build/ne16-dory-generated-layer.riscv
```

The host suite checks deterministic fixture reproduction, rejection without
partial writes, full coverage and halos, and the actual generated C callback.
It compares real NNX descriptor construction against the existing adapter
and exercises the native DMA queue/cooperative pipeline under different
transport timings. Transport tags are not MAC results; only the integrated
simulator checks numerical correctness. The regression runner allows 135
minutes for this firmware; `GENERATED_LAYER_TIMEOUT` overrides that limit,
and `TIMEOUT` overrides all defaults. Whole-network graph scheduling,
other operators/quantization modes, input-channel tiling, and multicore PMSIS
execution remain unsupported.

### Genuine two-layer generated chain

`tests/ne16-dory-generated-chain` contains two independently weighted
48x48x16-to-16 convolutions. Each layer has 76,160 tensor bytes, four
upstream DORY tiles, and 16 cooperative jobs. The pinned public frontend
generates layer 0, then supplies its actual quantized output to the next
`create_layer` call. Torch is seeded once for the chain, not once per
layer. Thus layer 1's input equals layer 0's reference output byte-for-byte;
its final oracle is a genuinely chained full-layer convolution reference,
not a second standalone random-input test.

DORY selects shifts 11 and 9 respectively. The cooperative importer accepts
`--use-generated-shift` only with an explicit nondefault `--namespace`.
It validates a literal shift in 0..31, preserves all other strict task-mode
checks, and records the shift in the namespaced header and metadata.
Without this opt-in, imports remain shift-11-only and the existing fixture
is byte-identical. The manual tiled adapter also retains its shift-11
default.

```sh
python3 generators/ne16/scripts/generate-dory-ne16-cooperative-chain.py \
  --dory-root "$DORY_ROOT" --python "$DORY_ROOT/.venv/bin/python" \
  --config generators/ne16/config/config_single_layer0_ne16_chain.json \
  --config-name generators/ne16/config/config_single_layer0_ne16_chain.json \
  --config generators/ne16/config/config_single_layer0_ne16_chain.json \
  --config-name generators/ne16/config/config_single_layer0_ne16_chain.json \
  --output-dir /tmp/ne16-generated-chain
```

The coordinator checks tensor linkage, original decimal/binary oracle
agreement, and frontend/task shift agreement before atomically publishing
the entire chain. Namespaces `dory_chain_l0` and `dory_chain_l1` isolate the
bindings; each is compiled with only its own byte-preserved upstream helper
directory. This is a two-layer composition of pinned DORY APIs, not general
whole-network DORY graph scheduling.

The complete generation/import wrapper passed on Idun in Slurm job
`25590872` (`0:0`, 1 minute 11 seconds), reproducing all 27 fixture files
byte-for-byte. The integrated `NE16RocketConfig` firmware also passed in
Slurm job `25591115` (`0:0`, 1 hour 27 minutes 32 seconds). All 36,864
intermediate and 36,864 final bytes matched the genuine chained references,
with 16 jobs per layer and shifts 11/9. Each layer reused both slots 8/8
times, with 3,455/3,453 physical DMA/NE16 overlap grants respectively.
Source/reference integrity, unchanged intermediate, full coverage, all
DRAM guards, and the entire scratchpad including stale tails passed.

The firmware stores layer 0's output in a guarded DRAM intermediate, waits
for every producer store to finish, verifies that intermediate, then gives
the same buffer address to layer 1. It does not copy or repack the second
input. Both layers reuse both scratch slots and require physical DMA/NE16
overlap. Intermediate and final comparisons cover all 36,864 bytes each;
source integrity, all DRAM guards, complete output coverage, and the whole
scratchpad including stale tails are checked without clearing between
layers.

```sh
python3 generators/ne16/scripts/test-dory-ne16-generated-chain.py
cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug
cmake --build tests/build --target ne16-dory-generated-chain
sims/verilator/simulator-chipyard.harness-NE16RocketConfig \
  +permissive +loadmem=tests/build/ne16-dory-generated-chain.riscv \
  +permissive-off tests/build/ne16-dory-generated-chain.riscv
```

The host suite checks actual descriptors, shifts, halos and chained native
DMA/pipeline transport; transport tags are not MAC results. Numerical
correctness requires the integrated simulator. The regression runner
allows 180 minutes, overridden by `GENERATED_CHAIN_TIMEOUT` or `TIMEOUT`.
Generation, builds, and simulation on Idun must run inside Slurm.

Focused hardware tests can be run from the Chipyard root:

```sh
java -jar scripts/sbt-launch.jar \
  'chipyard/testOnly chipyard.ne16.NE16ScratchpadSpec chipyard.ne16.NE16DMASpec'
```

These require a Linux Verilator/firtool environment. On Idun, both these tests
and simulator builds/runs must execute inside Slurm allocations.
