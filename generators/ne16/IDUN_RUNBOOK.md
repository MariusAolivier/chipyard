# Idun runbook for NE16 and DORY work

This document collects the practical commands and environment information
needed to generate DORY output, build Chipyard/Verilator, run regressions, and
perform FPGA/Vivado work on Idun.

The central operational rule is:

> Do not run large searches, builds, Verilator simulations, DORY generation,
> Vivado jobs, or FPGA operations directly on an Idun login node. Start them
> through Slurm.

Small read-only inspection commands on the login node are acceptable, but
anything CPU-, memory-, I/O-, or tool-intensive belongs in an allocation.

## Known paths and hosts

### Chipyard checkout

The working checkout used for this integration is:

```text
/cluster/work/mariusao/chipyard
```

Run Chipyard commands from the repository root unless a script says otherwise.

### DORY checkout

The DORY checkout used for the generated artifacts is:

```text
/cluster/work/mariusao/dory
```

The analyzed DORY revision was:

```text
add0d9c1be889b5f802b2606ced8c59acff8aa02
```

The exact NE16 configuration is:

```text
dory/dory_examples/config_files/config_single_layer0_ne16.json
```

Existing and generated analysis output directories include:

```text
/cluster/work/mariusao/dory/generated-single-layer0-uint
/cluster/work/mariusao/dory/generated-analysis-ne16-2
```

Do not treat the first directory as an NE16 runtime artifact; it was
generated through the generic GAP9 path and is incomplete for this purpose.
The second directory was generated through the generic path as well. The
correct command for the NE16 backend is documented below.

### FPGA and Vivado

The FPGA reservation is on:

```text
idun-12-[21-24]
```

`idun-12-21` was observed to expose two U250 cards:

```text
d8:00.0
af:00.0
```

Both used the `xdma` driver. Slurm did not expose per-card U250 GRES during
the investigation; it exposed only generic `gpu` GRES. Use `--exclusive` for
FPGA builds and programming until the cluster administrators provide
per-card resource names.

Vivado 2024.2:

```text
/cluster/projects/itea_lille-ie-idi/opt/Xilinx-2024/Vivado/2024.2
```

### U250 target used for validation

The validated board is an Xilinx Alveo U250:

```text
Part:       xcu250-figd2104-2l-e
Board:      xilinx.com:au250:part0:1.3
BDF:        0000:d8:00.0
UID:        Xilinx/21330409B00KA
```

The bitstream was programmed volatile-only. Do not program the other card or
write flash/MCS images unless that operation is explicitly authorized and the
target has been reverified.

## Connecting to Idun

Use the existing authenticated terminal/session when possible. Login sessions
can disconnect unexpectedly, so do not rely on an interactive shell for a
long-running command.

The normal pattern is:

```bash
ssh <user>@idun-login1
```

Then submit work with `srun` or `sbatch`. If a terminal session is already
connected through the app, send commands to that terminal rather than opening
multiple competing SSH sessions.

Do not put credentials, private keys, or tokens into repository files or
commands copied into logs.

## Slurm basics

### Interactive allocation

For a short CPU build or test:

```bash
srun --time=00:15:00 \
     --cpus-per-task=4 \
     --mem=8G \
     --job-name=ne16-build \
     bash -lc 'cd /cluster/work/mariusao/chipyard && <command>'
```

For a longer build:

```bash
srun --time=01:00:00 \
     --cpus-per-task=8 \
     --mem=32G \
     --job-name=chipyard-build \
     bash -lc 'cd /cluster/work/mariusao/chipyard && <command>'
```

For FPGA/Vivado work:

```bash
srun --exclusive \
     --partition=CPUQ \
     --account=share-ie-idi \
     --reservation=fpga \
     --nodes=1 \
     --ntasks=1 \
     --time=04:00:00 \
     --cpus-per-task=8 \
     --mem=32G \
     --job-name=u250-ne16 \
     bash -lc '<command>'
```

The `fpga` reservation is not the Slurm partition. During validation, FPGA
work used `CPUQ` with account `share-ie-idi` and reservation `fpga`. Confirm
the current site configuration before submitting:

```bash
sinfo
scontrol show partition
```

Always use `--nodes=1 --ntasks=1` for these jobs. Use `--exclusive` for any
Vivado or board operation, and verify the allocated hostname before touching
hardware.

### Batch jobs

For work that should survive a disconnected terminal, create a job script in
a private temporary directory or use `sbatch` with an explicit output file:

```bash
sbatch --time=01:00:00 \
       --cpus-per-task=8 \
       --mem=32G \
       --job-name=ne16-sim \
       --output=/cluster/work/mariusao/slurm-%j.out \
       --wrap='cd /cluster/work/mariusao/chipyard && <command>'
```

The command output should identify the Slurm job ID. Save that ID.

### Inspect and cancel jobs

```bash
squeue -u "$USER"
sacct -j <jobid> --format=JobID,State,Elapsed,AllocTRES,MaxRSS,ExitCode
scancel <jobid>
```

Cancel only the specific job that was started for the current task.

### Avoid accidental login-node execution

Good:

```bash
srun --time=00:30:00 --cpus-per-task=4 --mem=16G \
  bash -lc 'cd /cluster/work/mariusao/chipyard && make'
```

Bad:

```bash
cd /cluster/work/mariusao/chipyard
make
```

The second form runs on the login node.

## Common Slurm environment setup

Use one shell so environment changes and the build happen in the same
allocation:

```bash
srun --time=00:30:00 --cpus-per-task=4 --mem=16G \
  bash -lc '
    set -e
    cd /cluster/work/mariusao/chipyard
    source env.sh
    <command>
  '
```

Useful diagnostics inside the allocation:

```bash
hostname
pwd
id
which python3
which java
which verilator
which firtool
which dtc
echo "$RISCV"
echo "$PATH"
```

If the allocation does not inherit the expected environment, source the
project environment explicitly and set the required tool paths in the same
shell.

For FireSim work, initialize the environments in this order:

```bash
source /cluster/work/mariusao/miniforge3/etc/profile.d/conda.sh
source "$ROOT/env.sh"
source "$ROOT/sims/firesim/env.sh"
```

Use `set -eo pipefail` while activating Conda and the project environments;
enable `set -u` only afterward if needed. Conda activation can fail with an
unbound `RISCV` variable when nounset is enabled too early.

`XDG_RUNTIME_DIR` can be required by Java/build tools. If it is missing:

```bash
export XDG_RUNTIME_DIR=/tmp/$USER-runtime-$SLURM_JOB_ID
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
```

Do not use a shared or world-writable runtime directory.

## DORY generation

The generic generator emits software PULP-NN kernels and is not the target
for this integration:

```bash
python layer_generate.py PULP.GAP9 ...
```

Use the NE16-specific generator:

```bash
srun --time=00:20:00 --cpus-per-task=2 --mem=8G \
  --job-name=dory-ne16-generate \
  bash -lc '
    set -e
    cd /cluster/work/mariusao/dory
    source .venv/bin/activate
    mkdir -p /tmp/$USER-mako
    python -m pip install --no-cache-dir --no-deps \
      --target /tmp/$USER-mako Mako==1.3.10
    export PYTHONPATH=/tmp/$USER-mako:${PYTHONPATH:-}
    export PYTHONBREAKPOINT=0
    rm -rf /cluster/work/mariusao/dory/generated-chipyard-ne16-tile
    python layer_generate_ne16.py \
      PULP.GAP9_NE16 \
      --config_file dory/dory_examples/config_files/config_single_layer0_ne16.json \
      --app_dir /cluster/work/mariusao/dory/generated-chipyard-ne16-tile \
      --optional 8bit
  '
```

If package download is unavailable, inspect whether Mako already exists in a
usable environment before attempting another installation:

```bash
python -c 'import mako; print(mako.__file__)'
```

Do not modify the DORY repository to add temporary dependencies. The
temporary target directory and `PYTHONPATH` are sufficient.

After generation:

```bash
find /cluster/work/mariusao/dory/generated-chipyard-ne16-tile \
  -maxdepth 3 -type f -printf '%p %s bytes\n' | sort

grep -R -n -E \
  'nnx_task_set_dims|nnx_task_set_ptrs|nnx_dispatch_task|nnx_resolve_check' \
  /cluster/work/mariusao/dory/generated-chipyard-ne16-tile
```

Reject output that contains only generic `pulp_nn_conv_*` calls. The desired
output must contain the NE16 NNX path.

## Chipyard environment and dependencies

From an allocation, start with:

```bash
cd /cluster/work/mariusao/chipyard
source env.sh
```

The NE16 integration uses pinned RTL and dependency material under:

```text
generators/ne16/
```

Important scripts:

```text
generators/ne16/scripts/fetch-bender.sh
generators/ne16/scripts/fetch-deps.sh
generators/ne16/scripts/prepare-rtl.sh
generators/ne16/scripts/standalone-lint.sh
generators/ne16/scripts/build-chipyard-sim.sh
generators/ne16/scripts/run-conv-test.sh
```

The integration records pinned revisions in `Bender.lock` and uses a pinned
Bender binary. If `BENDER` is not already set:

```bash
export BENDER=/path/to/pinned/bender
```

Run Bender from `generators/ne16`, not from an unrelated directory.

The simulator build may require all of the following to be visible:

- Java;
- `dtc`;
- `firtool`;
- RISC-V toolchain via `RISCV`;
- Verilator;
- a writable runtime directory.

Check each tool inside the allocation before starting a long build.

## Prepare and lint the standalone NE16 RTL

From the Chipyard root, inside Slurm:

```bash
srun --time=00:20:00 --cpus-per-task=2 --mem=8G \
  --job-name=ne16-rtl-lint \
  bash -lc '
    set -e
    cd /cluster/work/mariusao/chipyard/generators/ne16
    ./scripts/fetch-bender.sh
    ./scripts/fetch-deps.sh
    ./scripts/standalone-lint.sh
  '
```

`prepare-rtl.sh` resolves the pinned RTL and prepares the Verilator manifest.
The manifest proves source discovery, dependency closure, ordering, and
defines; it does not prove Chisel elaboration, Verilator compilation, or
accelerator functionality.

The HCI FIFO patch is applied to a generated copy of the dependency. Do not
edit pinned upstream repositories in place.

## Build the Chipyard Verilator simulator

Run the canonical script in an allocation:

```bash
srun --time=01:00:00 --cpus-per-task=8 --mem=32G \
  --job-name=ne16-verilator-build \
  bash -lc '
    set -e
    cd /cluster/work/mariusao/chipyard
    source env.sh
    export XDG_RUNTIME_DIR=/tmp/$USER-runtime-$SLURM_JOB_ID
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
    generators/ne16/scripts/build-chipyard-sim.sh
  '
```

The build script also applies or repairs the TestChipIP simulator workaround
for invalid AXI write-data DPI handles and generated array ranges. This is a
Verilator/TestChipIP simulation workaround only; it does not modify the NE16
RTL or FireSim FPGA memory path.

If the build fails, capture:

```bash
hostname
git status --short
git submodule status
which java verilator firtool dtc
echo "$RISCV"
```

Do not delete or reset unrelated worktree changes while diagnosing a failure.

## Run existing NE16 simulator tests

The canonical regression is:

```bash
srun --time=00:30:00 --cpus-per-task=4 --mem=16G \
  --job-name=ne16-conv-tests \
  bash -lc '
    set -e
    cd /cluster/work/mariusao/chipyard
    source env.sh
    generators/ne16/scripts/run-conv-test.sh
  '
```

The expected passing coverage includes:

- 1x1 convolution;
- 3x3 convolution;
- DORY-style layer;
- queued/consecutive operations where covered by the current scripts.

Do not report a new generated-tile test as successful if these baseline tests
regress.

For a generated-tile implementation, use this order:

1. Build only the new test target.
2. Run descriptor-only tests.
3. Run DMA-only tests.
4. Run the generated-tile test.
5. Run `run-conv-test.sh`.

Add a simulator timeout to every test command that could hang:

```bash
timeout 300 <simulator-command>
```

When the test hangs, first check whether it is polling an NE16 status bit,
waiting for a DMA token, or stuck in a generated loop. Do not repeatedly
rerun an unbounded command.

## Test ELF and symbol checks

The generated-tile target must prove that it uses the generated NNX path.
After building the ELF:

```bash
nm -C <path-to-elf> | grep -E \
  'nnx_|dory_|ne16_submit|ne16_build_dory_3x3_task'
```

The generated-tile ELF should contain the generated layer and compatibility
symbols. It must not reference the specialized manual builder.

If a link map is available, inspect it for:

- generated layer source;
- `dory_ne16_compat`;
- `dory_dma`;
- `ne16_driver`;
- absence of generic `pulp_nn_conv_*` implementations.

## Vivado and U250 work

FPGA operations are separate from the first DORY simulator milestone. They
must use an exclusive Slurm allocation:

```bash
srun --exclusive --partition=CPUQ --account=share-ie-idi \
  --reservation=fpga --nodes=1 --ntasks=1 \
  --time=04:00:00 --cpus-per-task=8 --mem=32G \
  --job-name=ne16-u250-bitstream \
  bash -lc '
    set -e
    ROOT=/cluster/work/mariusao/chipyard
    source /cluster/work/mariusao/miniforge3/etc/profile.d/conda.sh
    source "$ROOT/env.sh"
    source "$ROOT/sims/firesim/env.sh"
    export VIVADO_ROOT=/cluster/projects/itea_lille-ie-idi/opt/Xilinx-2024/Vivado/2024.2
    source "$VIVADO_ROOT/settings64.sh"
    vivado -version
    hostname
    cd "$ROOT/sims/firesim/sim"
    tag=ne16-u250-$(date +%Y%m%d-%H%M%S)
    GENERATED_DIR="$ROOT/sims/firesim/sim/generated-src/$tag"
    OUTPUT_DIR="$ROOT/sims/firesim/sim/output/$tag"
    CL_DIR="$ROOT/sims/firesim/platforms/xilinx_alveo_u250/cl_$tag"
    make replace-rtl \
      PLATFORM=xilinx_alveo_u250 TARGET_PROJECT=firesim \
      TARGET_PROJECT_MAKEFRAG="$ROOT/generators/firechip/chip/src/main/makefrag/firesim" \
      DESIGN=FireSim TARGET_CONFIG=FireSimNE16RocketConfig \
      PLATFORM_CONFIG=BaseXilinxAlveoU250Config \
      GENERATED_DIR="$GENERATED_DIR" OUTPUT_DIR="$OUTPUT_DIR" \
      fpga_work_dir="$CL_DIR"
    export NE16_U250_BOARD_REPO="$VIVADO_ROOT/data/boards"
    bash "$ROOT/generators/ne16/scripts/prepare-firesim-u250-vivado.sh" "$CL_DIR"
    vivado -mode batch -nolog -nojournal \
      -source "$ROOT/generators/ne16/scripts/firesim-u250-vivado-preflight.tcl" \
      -tclargs "$NE16_U250_BOARD_REPO" "$CL_DIR/design/ne16-vivado.tcl"
    "$ROOT/sims/firesim/platforms/xilinx_alveo_u250/build-bitstream.sh" \
      --cl_dir "$CL_DIR" --frequency 60 --strategy TIMING --board au250
  '
```

The expected Vivado version is:

```text
Vivado v2024.2 (64-bit)
```

The U250 wrapper invokes Vivado batch synthesis, optimization, placement,
routing, timing analysis, and bitstream generation. Do not program a board
unless the allocation owns the relevant FPGA host and card.

For an existing task-owned project, use
`generators/ne16/scripts/resume-firesim-u250-vivado.tcl` only after checking
the project, source-manifest, clock, and provenance conditions. Use
`audit-firesim-u250-constraints.tcl` and
`review-firesim-u250-vivado.tcl` after implementation. A completed
implementation is not sufficient without fresh timing, bus-skew, CDC,
mapped-storage, and provenance checks.

Before any board operation:

```bash
hostname
lspci | grep -i -E 'xilinx|alveo|xdma'
```

The FireSim RTL-generation working directory previously used was:

```text
/cluster/work/mariusao/chipyard/sims/firesim/sim
```

Golden Gate lowering succeeded in Slurm before the FireSim manager path was
blocked by its external build-farm/SSH setup. Keep RTL generation, Golden
Gate, Vivado, and FireSim manager failures separate when diagnosing them.

### Current physical validation status

The repaired image used for validation had these recorded artifacts:

```text
Bitstream SHA-256:
d6238efc1e06785fe91a98e2849dd60adf5288f1e9487c6f801e014661e376e8

Driver SHA-256:
5bcc84d7defdc5b3b8f5c4c199462a0ef2a855eb71ad66258e0ab53a5475846f
```

The FireSim fingerprint, descriptor firmware, queued raw 1x1 convolution, and
queued raw 3x3 convolution passed on the selected U250. Physical DMA,
double-buffer, and generated DORY-chain validation remain incomplete.
Allocations `25641608` and `25642265` ended in `NODE_FAIL` before useful
DMA-firmware checkpoints. Do not attribute those failures to DMA RTL without
administrator-level kernel, PCIe/AER, XDMA, watchdog, and node-recovery
evidence. Do not repeat physical DMA testing automatically.

## Common failure modes

### `srun` is queued

This is normal when resources are busy:

```text
srun: job <id> queued and waiting for resources
```

Wait or inspect the queue with `squeue`; do not move the command to the login
node.

### Missing `mako`

Use the temporary target installation described in the DORY section. Confirm
the import in the same shell that runs generation.

### Missing `torch`

Use `/cluster/work/mariusao/dory/.venv/bin/activate`. Confirm:

```bash
python -c 'import torch; print(torch.__version__)'
```

### Missing `firtool`, `dtc`, Java, or RISC-V tools

Check the allocation environment and source Chipyard's `env.sh`. Do not
install or modify dependencies in the repository without an explicit
dependency change.

### DORY output contains no NNX symbols

The generic generator was used. Regenerate with:

```text
layer_generate_ne16.py PULP.GAP9_NE16
```

### Verilator simulation hangs

Use a bounded `timeout`, inspect the last printed phase, and distinguish:

- NE16 busy/status polling;
- DMA transfer wait;
- generated tile loop;
- simulator/DRAM initialization.

### TestChipIP DPI errors

Use the repository's canonical simulator build script so the
`testchipip-simdram-valid-write-data.patch` workaround is applied
idempotently. Do not patch the submodule manually in an ad hoc way.

### FireSim manager tries SSH to localhost

That is an external FireSim build-farm configuration issue, not proof that
NE16 RTL generation or Golden Gate failed. Validate those stages independently.

### U250 contention

Slurm did not expose individual U250 cards as GRES during the investigation.
Use `CPUQ` with the site-approved account/reservation, `--exclusive`,
`--nodes=1 --ntasks=1`, and verify the node and card. Never program a shared
card or assume that a BDF still refers to the same physical UID after a PCIe
rescan.

## Reproducibility checklist

Every implementation agent should record:

- Git commit and branch;
- DORY commit and configuration path;
- Slurm job IDs;
- hostname for each hardware/build job;
- exact command;
- tool versions;
- generated artifact hashes;
- simulator/test output;
- whether the worktree was modified;
- whether a failure occurred on generation, compilation, simulation, Golden
  Gate, Vivado, or FireSim manager.

Do not claim a generated DORY tile passed unless the generated layer itself
was called and the output was compared byte-for-byte.
