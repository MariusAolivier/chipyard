# Chipyard ITA integration

This directory integrates the upstream PULP Integer Transformer Accelerator
without modifying its RTL or test generator.

## Pinned inputs

- Chipyard: `e602d917dcc495c58cabe906535e411707096c9c`
- ITA: `ba96519becce195d64e85eb9a5302e8a1d5487e7`
- Bender: `v0.28.1`
- Bender Linux x86-64 archive SHA-256:
  `561de10e4108627f5accdd1fed94380456440d99f6766f7750fbbe1903e53b68`

The upstream `Bender.lock` pins all RTL dependencies.

## Standalone validation

After sourcing Chipyard's `env.sh`, prepare and run the Verilator regression:

```sh
scripts/fetch-bender.sh
scripts/fetch-deps.sh
scripts/build-smoke-verilator.sh
scripts/run-smoke-regression.sh
```

The regression uses ITA's unmodified HWPE wrapper, control, streamers, and
compute RTL. It queues two 64x64 linear operations without reset and checks all
8,192 output bytes against independently calculated row sums.

`rtl/ita_chipyard_wrapper.sv` flattens the native sixteen-lane, 1024-bit HCI
memory path and HWPE control interface into packed vectors suitable for a
Chisel BlackBox. The upstream submodule remains unchanged.
