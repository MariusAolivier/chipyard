#!/usr/bin/env python3
"""Run bounded tiled-runtime and exhaustive scratch-helper host checks; no RTL/MAC."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc",
                        help="Existing host C compiler (default: gcc, not target CC)")
    parser.add_argument("--timeout", type=int, default=60,
                        help="Maximum seconds for each compile/link/run")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    compiler = ([args.cc] if Path(args.cc).is_file()
                else shlex.split(args.cc, posix=os.name != "nt"))
    if not compiler or shutil.which(compiler[0]) is None:
        parser.error(f"host compiler not found: {args.cc}")
    root = Path(__file__).resolve().parents[3]
    tests = root / "tests"
    compat = tests / "dory_ne16_compat"
    flags = ["-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
             "-I", str(tests), "-I", str(compat)]
    try:
        with tempfile.TemporaryDirectory(prefix="dory-ne16-tiled-host-") as temp:
            directory = Path(temp)
            validator = directory / "ne16_dma.o"
            # Only the three MMIO entry points used by the queue are replaced.
            # The actual ne16_dma_validate implementation is linked unchanged.
            subprocess.run(
                compiler + flags +
                ["-Dne16_dma_submit=host_unused_mmio_submit",
                 "-Dne16_dma_poll=host_unused_mmio_poll",
                 "-Dne16_dma_status=host_unused_mmio_status",
                 "-c", str(tests / "ne16_dma.c"), "-o", str(validator)],
                check=True, timeout=args.timeout,
            )
            executable = directory / ("tiled-host.exe" if os.name == "nt"
                                      else "tiled-host")
            sources = [
                tests / "dory-ne16-tiled-host.c",
                compat / "dory_tiled_layer.c",
                compat / "dory_pipeline.c",
                compat / "dory_dma.c",
            ]
            subprocess.run(
                compiler + flags + [str(path) for path in sources] +
                [str(validator), "-o", str(executable)],
                check=True, timeout=args.timeout,
            )
            subprocess.run([str(executable)], check=True, timeout=args.timeout)
            scratch_executable = directory / (
                "scratch-host.exe" if os.name == "nt" else "scratch-host")
            subprocess.run(
                compiler + flags + [str(tests / "ne16-test-scratch-host.c"),
                                    "-o", str(scratch_executable)],
                check=True, timeout=args.timeout,
            )
            subprocess.run([str(scratch_executable)], check=True,
                           timeout=args.timeout)
    except (OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"DORY tiled host validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
