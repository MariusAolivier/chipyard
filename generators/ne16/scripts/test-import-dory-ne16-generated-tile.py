#!/usr/bin/env python3
"""Exercise importer reproducibility and rejection paths using the checked-in tile."""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional


def extract_array(source: str, name: str) -> bytes:
    match = re.search(rf"const uint8_t {name}\[[^]]+\] = \{{(.*?)\}};", source, re.S)
    if match is None:
        raise ValueError(f"missing fixture array {name}")
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))


def invoke(importer: Path, generated: Path, input_path: Path, weights_path: Path,
           expected_path: Path, output: Path, metadata: dict, l1: Optional[int] = None) -> subprocess.CompletedProcess:
    command = [
        sys.executable, str(importer),
        "--generated-dir", str(generated),
        "--input", str(input_path),
        "--weights", str(weights_path),
        "--expected", str(expected_path),
        "--output-dir", str(output),
        "--dory-commit", metadata["dory_commit"],
        "--config", metadata["config"],
        "--config-sha256", metadata["config_sha256"],
        "--l1-requirement", str(metadata["l1_requirement"] if l1 is None else l1),
        "--padding-guard", str(metadata["padding_guard"]),
    ]
    return subprocess.run(command, capture_output=True, text=True)


def expect_rejected(label: str, result: subprocess.CompletedProcess) -> None:
    if result.returncode == 0:
        raise AssertionError(f"{label} was accepted")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    importer = repo / "generators/ne16/scripts/import-dory-ne16-generated-tile.py"
    fixture = repo / "tests/ne16-dory-generated"
    metadata = json.loads((fixture / "generated_tile_metadata.json").read_text())
    fixture_source = (fixture / "dory_generated_tile_data.c").read_text()

    with tempfile.TemporaryDirectory(prefix="dory-import-test-") as temporary:
        root = Path(temporary)
        generated = root / "generated"
        shutil.copytree(fixture, generated, ignore=shutil.ignore_patterns("dory_generated_tile_data.*", "generated_tile_metadata.json"))
        input_path = root / "input.bin"
        weights_path = root / "weights.bin"
        expected_path = root / "expected.bin"
        input_path.write_bytes(extract_array(fixture_source, "dory_tile_input"))
        weights_path.write_bytes(extract_array(fixture_source, "dory_tile_weights"))
        expected_path.write_bytes(extract_array(fixture_source, "dory_tile_expected"))

        first = invoke(importer, generated, input_path, weights_path, expected_path, root / "out-1", metadata)
        if first.returncode != 0:
            raise AssertionError(first.stderr)
        second = invoke(importer, generated, input_path, weights_path, expected_path, root / "out-2", metadata)
        if second.returncode != 0:
            raise AssertionError(second.stderr)
        first_files = sorted(path.relative_to(root / "out-1") for path in (root / "out-1").rglob("*") if path.is_file())
        second_files = sorted(path.relative_to(root / "out-2") for path in (root / "out-2").rglob("*") if path.is_file())
        if first_files != second_files:
            raise AssertionError("repeated imports produced different file sets")
        for relative in first_files:
            if (root / "out-1" / relative).read_bytes() != (root / "out-2" / relative).read_bytes():
                raise AssertionError(f"repeated imports differ in {relative}")

        truncated = root / "input-truncated.bin"
        truncated.write_bytes(input_path.read_bytes()[:-1])
        expect_rejected("wrong-sized input", invoke(importer, generated, truncated, weights_path, expected_path, root / "bad-input", metadata))

        missing_nnx = root / "missing-nnx"
        shutil.copytree(generated, missing_nnx)
        execute = missing_nnx / "inc/execute.h"
        execute.write_text(execute.read_text().replace("nnx_task_set_dims", "missing_nnx_dims", 1))
        expect_rejected("missing NNX call", invoke(importer, missing_nnx, input_path, weights_path, expected_path, root / "bad-nnx", metadata))

        generic_conv = root / "generic-conv"
        shutil.copytree(generated, generic_conv)
        source = generic_conv / "src/BNReluConvolution0.c"
        source.write_text(source.read_text() + "\nvoid pulp_nn_conv_forbidden(void) {}\n")
        expect_rejected("generic convolution", invoke(importer, generic_conv, input_path, weights_path, expected_path, root / "bad-generic", metadata))

        wrong_tiles = root / "wrong-tiles"
        shutil.copytree(generated, wrong_tiles)
        source = wrong_tiles / "src/BNReluConvolution0.c"
        source.write_text(source.read_text().replace("static const TileIndex end_index = {\n    .height = 1,", "static const TileIndex end_index = {\n    .height = 2,", 1))
        expect_rejected("multiple tiles", invoke(importer, wrong_tiles, input_path, weights_path, expected_path, root / "bad-tiles", metadata))

        expect_rejected("oversized guarded L1", invoke(importer, generated, input_path, weights_path, expected_path, root / "bad-l1", metadata, 64 * 1024))
        bad_hash_metadata = dict(metadata)
        bad_hash_metadata["config_sha256"] = "0" * 64
        expect_rejected("corrupted generation config", invoke(importer, generated, input_path, weights_path, expected_path, root / "bad-config", bad_hash_metadata))

    print("DORY importer reproducibility and rejection tests PASS")


if __name__ == "__main__":
    main()
