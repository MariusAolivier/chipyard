#!/usr/bin/env python3
"""Check cooperative import determinism, rejection paths, and real C binding transport."""

import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from dory_ne16_import import parse_expected, sha256


def extract(source: str, name: str) -> bytes:
    match = re.search(rf"const uint8_t dory_generated_{name}\[\d+\] = \{{(.*?)\}};", source, re.S)
    if match is None:
        raise ValueError(f"missing fixture tensor {name}")
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", match.group(1)))


def check_import(root: Path, temporary: Path) -> None:
    scripts = root / "generators" / "ne16" / "scripts"
    specification = importlib.util.spec_from_file_location(
        "cooperative_import", scripts / "import-dory-ne16-cooperative-layer.py")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    fixture = root / "tests" / "ne16-dory-generated-layer"
    metadata = json.loads((fixture / "generated_layer_metadata.json").read_text())
    source = (fixture / "dory_generated_layer_data.c").read_text()
    config_path = root / metadata["config"]
    paths = {}
    for name in ("input", "weights", "expected"):
        paths[name] = temporary / (name + ".bin")
        paths[name].write_bytes(extract(source, name))
    args = SimpleNamespace(
        generated_dir=fixture / "upstream", config=config_path,
        hardware_description=fixture / "upstream" / "HW_description.json",
        config_name=metadata["config"], config_sha256=metadata["config_sha256"],
        dory_commit=metadata["dory_commit"], expected_format="binary",
        output_dir=temporary / "first", **paths)
    module.import_layer(args)
    args.output_dir = temporary / "second"
    module.import_layer(args)
    files = {file.relative_to(fixture) for file in fixture.rglob("*") if file.is_file()}
    for directory in (temporary / "first", temporary / "second"):
        assert files == {file.relative_to(directory) for file in directory.rglob("*") if file.is_file()}
        for name in files:
            assert (directory / name).read_bytes() == (fixture / name).read_bytes(), name
    # Canonical LF hashing permits a Windows checkout without changing provenance.
    windows_config = temporary / "windows.json"
    windows_config.write_bytes(config_path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
    windows = copy.copy(args)
    windows.config, windows.output_dir = windows_config, temporary / "windows"
    module.import_layer(windows)
    for name in files:
        assert (windows.output_dir / name).read_bytes() == (fixture / name).read_bytes(), name
    text = temporary / "expected.txt"
    text.write_text(",\n".join(str(byte) for byte in paths["expected"].read_bytes()))
    text_args = copy.copy(args)
    text_args.expected, text_args.expected_format = text, "text"
    text_args.output_dir = temporary / "text"
    module.import_layer(text_args)
    assert (text_args.output_dir / "dory_generated_layer_data.c").read_bytes() == (fixture / "dory_generated_layer_data.c").read_bytes()
    rejected = 0

    def reject(candidate):
        nonlocal rejected
        candidate.output_dir = temporary / ("rejected-" + str(rejected))
        try:
            module.import_layer(candidate)
        except (ValueError, OSError):
            assert not candidate.output_dir.exists(), "rejection wrote partial artifacts"
            rejected += 1
            return
        raise AssertionError("unsupported import was accepted")

    config = json.loads(config_path.read_bytes())
    for field, value in (
            ("stride", [2, 2]), ("group", 16), ("input_channels", 32),
            ("output_channels", 17), ("input_bits", 16), ("output_type", "int"),
            ("weight_bits", 4), ("bias_bits", 8), ("batchnorm", False),
            ("kernel_shape", [3.0, 3]), ("stride", [1.0, 1]),
            ("padding", [True, 1, 1, 1]),
            ("input_dimensions", [0, 32]), ("input_dimensions", [True, 32]),
            ("input_dimensions", [65536, 32]), ("branch_in", 1)):
        altered = dict(config, **{field: value})
        path = temporary / "invalid-config.json"
        path.write_text(json.dumps(altered))
        candidate = copy.copy(args)
        candidate.config, candidate.config_sha256 = path, sha256(path.read_bytes())
        reject(candidate)
    for field in ("input", "weights", "expected"):
        path = temporary / ("truncated-" + field)
        path.write_bytes(paths[field].read_bytes()[:-1])
        candidate = copy.copy(args)
        setattr(candidate, field, path)
        reject(candidate)
    for invalid_config in ([], None, 7):
        path = temporary / "invalid-root.json"
        path.write_text(json.dumps(invalid_config))
        candidate = copy.copy(args)
        candidate.config, candidate.config_sha256 = path, sha256(path.read_bytes())
        reject(candidate)
    for field, value in (("dory_commit", "0" * 40), ("config_sha256", "0" * 64)):
        candidate = copy.copy(args)
        setattr(candidate, field, value)
        reject(candidate)
    for relative, old, new in (
            ("src/BNReluConvolution0.c", ".height = 4,", ".height = 3,"),
            ("src/BNReluConvolution0.c", ".height = 11,", ".height = 12,"),
            ("src/BNReluConvolution0.c", ".channel = 16", ".channel = 8"),
            ("src/BNReluConvolution0.c", ".shift_amount = 11", ".shift_amount = 12"),
            ("src/BNReluConvolution0.c", ".groups = 1", ".groups = 16"),
            ("src/BNReluConvolution0.c", ".scale = layer_args->L2_weights + 6912",
             ".scale = layer_args->L2_weights + 6913"),
            ("src/BNReluConvolution0.c", "conf->stride_2d = 512", "conf->stride_2d = 513"),
            ("src/BNReluConvolution0.c", "conf->stride_2d = 1536", "conf->stride_2d = 1535"),
            ("src/BNReluConvolution0.c", "* 144;", "* 143;"),
            ("src/BNReluConvolution0.c", "void BNReluConvolution0(void *args)",
             "void OtherConvolution(void *args)"),
            ("src/BNReluConvolution0.c", "3, 0,", "3, 1,"),
            ("inc/execute.h", "0 /*shift_ptr*/", "4 /*shift_ptr*/"),
            ("inc/execute.h", "nnx_task_set_dims(task", "missing_dims(task")):
        generated = temporary / ("altered-" + str(rejected))
        shutil.copytree(args.generated_dir, generated)
        path = generated / relative
        original = path.read_text()
        assert old in original, (relative, old)
        path.write_text(original.replace(old, new, 1))
        candidate = copy.copy(args)
        candidate.generated_dir = generated
        reject(candidate)
    for invalid in (".height =", ".height = 1 + 2", ".height = 1, .height = 2",
                    ".input = { .height = 1", ".height = {1}", ".height =",
                    ".height = 08"):
        try:
            module.initializer(invalid)
        except ValueError:
            rejected += 1
        else:
            raise AssertionError(f"malformed initializer accepted: {invalid}")
    for invalid in (b"256", b"-1", b"1.5", b"\xff"):
        try:
            parse_expected(invalid)
        except (ValueError, UnicodeError):
            rejected += 1
        else:
            raise AssertionError("invalid expected output accepted")
    result = subprocess.run(
        [sys.executable, str(scripts / "import-dory-ne16-cooperative-layer.py"),
         "--generated-dir", str(args.generated_dir),
         "--config", str(config_path), "--config-name", metadata["config"],
         "--config-sha256", "0" * 64, "--dory-commit", metadata["dory_commit"],
         "--hardware-description", str(args.hardware_description),
         "--input", str(paths["input"]), "--weights", str(paths["weights"]),
         "--expected", str(paths["expected"]), "--expected-format", "binary",
         "--output-dir", str(temporary / "bad-cli")],
        capture_output=True, text=True, timeout=30)
    assert result.returncode == 2 and "error:" in result.stderr and "Traceback" not in result.stderr
    assert not (temporary / "bad-cli").exists()
    marker = temporary / "occupied"
    marker.mkdir()
    (marker / "keep").write_text("untouched")
    args.output_dir = marker
    try:
        module.import_layer(args)
    except ValueError:
        assert (marker / "keep").read_text() == "untouched"
    else:
        raise AssertionError("occupied output directory overwritten")
    plan = json.loads((fixture / "cooperative_plan.json").read_text())
    height, width, channels = module.validate_config(config)
    coverage = bytearray(height * width * channels)
    for recipe in plan:
        y, x, channel = (recipe[name] for name in ("output_y", "output_x", "output_channel"))
        assert recipe["input_y"] == max(0, y - 1)
        assert recipe["input_x"] == max(0, x - 1)
        assert recipe["input_height"] == min(height, y + recipe["output_height"] + 1) - recipe["input_y"]
        assert recipe["input_width"] == min(width, x + recipe["output_width"] + 1) - recipe["input_x"]
        for row in range(recipe["output_height"]):
            for column in range(recipe["output_width"]):
                for component in range(16):
                    offset = ((y + row) * width + x + column) * channels + channel + component
                    assert coverage[offset] == 0
                    coverage[offset] = 1
    assert all(coverage) and len(plan) == metadata["cooperative_jobs"]
    print(f"DORY cooperative importer reproducibility/coverage PASS; {rejected} rejection cases")


def check_binding(root: Path, directory: Path, compiler: list[str], timeout: int) -> None:
    tests = root / "tests"
    compat = tests / "dory_ne16_compat"
    fixture = tests / "ne16-dory-generated-layer"
    flags = ["-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror", "-include", "stdio.h"]
    for include in (tests, compat, fixture):
        flags += ["-I", str(include)]
    flags += ["-isystem", str(fixture / "upstream" / "inc")]
    # Retain the actual descriptor constructors; only MMIO entry points and
    # the RISC-V-specific fence are replaced for the host transport model.
    source = (tests / "ne16_driver.c").read_text()
    fence = '__asm__ volatile("fence rw, rw" ::: "memory");'
    assert source.count(fence) == 1
    driver = directory / "driver.c"
    driver.write_text(source.replace(fence, '__asm__ volatile("" ::: "memory");'))
    driver_object, dma_object = directory / "driver.o", directory / "dma.o"
    subprocess.run(compiler + flags +
        ["-Dne16_reset=unused_mmio_reset", "-Dne16_submit=unused_mmio_submit",
         "-Dne16_idle=unused_mmio_idle", "-c", str(driver), "-o", str(driver_object)],
        check=True, timeout=timeout)
    subprocess.run(compiler + flags +
        ["-Dne16_dma_submit=unused_mmio_dma_submit", "-Dne16_dma_poll=unused_mmio_dma_poll",
         "-Dne16_dma_status=unused_mmio_dma_status", "-c", str(tests / "ne16_dma.c"),
         "-o", str(dma_object)], check=True, timeout=timeout)
    sources = [tests / "dory-ne16-generated-layer-host.c", fixture / "dory_generated_layer.c",
               compat / "dory_tiled_layer.c", compat / "dory_pipeline.c",
               compat / "dory_dma.c", compat / "pulp_nnx.c", compat / "pulp_nnx_util.c"]
    executable = directory / ("binding.exe" if os.name == "nt" else "binding")
    subprocess.run(compiler + flags + [str(path) for path in sources] +
                   [str(driver_object), str(dma_object), "-o", str(executable)],
                   check=True, timeout=timeout)
    subprocess.run([str(executable)], check=True, timeout=timeout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    compiler = [args.cc] if Path(args.cc).is_file() else shlex.split(args.cc, posix=os.name != "nt")
    if args.timeout <= 0 or not compiler or shutil.which(compiler[0]) is None:
        parser.error("positive timeout and an existing host C compiler are required")
    root = Path(__file__).resolve().parents[3]
    try:
        with tempfile.TemporaryDirectory(prefix="dory-generated-host-") as temporary:
            directory = Path(temporary)
            check_import(root, directory)
            check_binding(root, directory, compiler, args.timeout)
    except (ValueError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired, AssertionError) as error:
        print(f"DORY generated-layer host validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
