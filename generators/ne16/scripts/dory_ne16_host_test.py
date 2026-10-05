"""Shared host build support: real descriptor builders, MMIO/fence substitution only."""

import os
from pathlib import Path
import subprocess


def build_binding(root: Path, directory: Path, compiler: list[str], timeout: int,
                  fixtures: list[tuple[Path, str]], harness: str) -> None:
    tests = root / "tests"
    compat = tests / "dory_ne16_compat"
    flags = ["-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror", "-include", "stdio.h",
             "-I", str(tests), "-I", str(compat)]
    source = (tests / "ne16_driver.c").read_text()
    fence = '__asm__ volatile("fence rw, rw" ::: "memory");'
    assert source.count(fence) == 1
    driver = directory / "driver.c"
    driver.write_text(source.replace(fence, '__asm__ volatile("" ::: "memory");'))
    objects = []

    def compile_source(path, name, extra=()):
        target = directory / (name + ".o")
        subprocess.run(compiler + flags + list(extra) + ["-c", str(path), "-o", str(target)],
                       check=True, timeout=timeout)
        objects.append(target)

    compile_source(driver, "driver", [
        "-Dne16_reset=unused_mmio_reset", "-Dne16_submit=unused_mmio_submit",
        "-Dne16_idle=unused_mmio_idle"])
    compile_source(tests / "ne16_dma.c", "dma", [
        "-Dne16_dma_submit=unused_mmio_dma_submit", "-Dne16_dma_poll=unused_mmio_dma_poll",
        "-Dne16_dma_status=unused_mmio_dma_status"])
    for index, (fixture, namespace) in enumerate(fixtures):
        # execute.h and its transitive headers have shared guards/names.
        # Each binding gets ONLY its own generated system include directory.
        include = ["-I", str(fixture), "-isystem", str(fixture / "upstream" / "inc")]
        compile_source(fixture / (namespace + "_layer.c"), f"layer{index}", include)
        compile_source(fixture / (namespace + "_layer_data.c"), f"data{index}", include)
    for name in ("dory_tiled_layer", "dory_pipeline", "dory_dma", "pulp_nnx", "pulp_nnx_util"):
        compile_source(compat / (name + ".c"), name)
    includes = [part for fixture, _ in fixtures for part in ("-I", str(fixture))]
    compile_source(tests / harness, "harness", includes)
    executable = directory / ("binding.exe" if os.name == "nt" else "binding")
    subprocess.run(compiler + [str(path) for path in objects] + ["-o", str(executable)],
                   check=True, timeout=timeout)
    subprocess.run([str(executable)], check=True, timeout=timeout)
