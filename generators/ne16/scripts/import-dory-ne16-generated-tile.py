#!/usr/bin/env python3
"""Import and validate a single verbatim DORY NE16 generated tile."""

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

REQUIRED_NNX = (
    "nnx_task_set_dims",
    "nnx_task_set_ptrs",
    "nnx_dispatch_check_blocking",
    "nnx_dispatch_task",
    "nnx_resolve_check",
)
REQUIRED_HEADERS = (
    "BNReluConvolution0.h",
    "dory_get_tile.h",
    "execute.h",
    "layer.h",
    "tile_index.h",
    "tile_status.h",
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def c_array(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}[{len(data)}] = {{"]
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in data[offset : offset + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def validate_layer(source: str) -> tuple[str, dict[str, int]]:
    if "#include \"execute.h\"" not in source:
        raise ValueError("generated layer does not include execute.h")
    missing = [symbol for symbol in REQUIRED_NNX
              if re.search(rf"\b{re.escape(symbol)}\s*\(", source) is None]
    if missing:
        raise ValueError(f"generated layer is missing NNX calls: {', '.join(missing)}")
    if "pulp_nn_conv_" in source:
        raise ValueError("generic PULP-NN convolution output is not an NE16 tile")

    function = re.search(r"\nvoid\s+([A-Za-z_]\w*)\s*\(void \*args\)", source)
    if function is None:
        raise ValueError("could not identify generated layer function")

    end_index = re.search(r"static const TileIndex end_index\s*=\s*\{(.*?)\};", source, re.S)
    if end_index is None:
        raise ValueError("generated layer has no end_index")
    dimensions = {
        name: int(value)
        for name, value in re.findall(r"\.(height|width|output_channel)\s*=\s*(\d+)", end_index.group(1))
    }
    if dimensions != {"height": 1, "width": 1, "output_channel": 1}:
        raise ValueError(f"generated layer is not one tile: {dimensions}")

    body = re.search(r"static const Layer body\s*=\s*\{(.*?)\n\};", source, re.S)
    if body is None:
        raise ValueError("generated layer has no body dimensions")
    values = [int(value) for value in re.findall(r"\.(?:height|width|channel)\s*=\s*(\d+)", body.group(1))]
    if len(values) != 6:
        raise ValueError("could not read generated body dimensions")
    return function.group(1), {
        "input_height": values[0],
        "input_width": values[1],
        "input_channels": values[2],
        "output_height": values[3],
        "output_width": values[4],
        "output_channels": values[5],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated-dir", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--weights", required=True, type=Path)
    parser.add_argument("--expected", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--dory-commit", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--config-sha256", required=True)
    parser.add_argument("--l1-requirement", required=True, type=int)
    parser.add_argument("--padding-guard", required=True, type=int)
    args = parser.parse_args()

    config_path = Path(args.config)
    if not config_path.is_file():
        raise ValueError(f"missing generation config: {config_path}")
    actual_config_sha256 = sha256(config_path.read_bytes())
    if actual_config_sha256 != args.config_sha256:
        raise ValueError(
            f"generation config hash {actual_config_sha256} does not match {args.config_sha256}"
        )

    source_path = args.generated_dir / "src" / "BNReluConvolution0.c"
    source = source_path.read_text()
    execute = (args.generated_dir / "inc" / "execute.h").read_text()
    layer_name, dimensions = validate_layer(source + "\n" + execute)
    input_data = args.input.read_bytes()
    weights = args.weights.read_bytes()
    expected = args.expected.read_bytes()
    expected_input = dimensions["input_height"] * dimensions["input_width"] * dimensions["input_channels"]
    expected_output = dimensions["output_height"] * dimensions["output_width"] * dimensions["output_channels"]
    if len(input_data) != expected_input:
        raise ValueError(f"input size {len(input_data)} does not match {expected_input}")
    if len(expected) != expected_output:
        raise ValueError(f"expected size {len(expected)} does not match {expected_output}")
    if len(weights) != 3 * 3 * 16 * 16 + 64 + 64:
        raise ValueError(f"weights size {len(weights)} does not match packed NE16 layout")
    total_l1 = args.padding_guard + args.l1_requirement
    if total_l1 > 64 * 1024:
        raise ValueError(f"guarded L1 allocation {total_l1} exceeds 64 KiB")

    fixture = args.output_dir
    (fixture / "src").mkdir(parents=True, exist_ok=True)
    (fixture / "inc").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source_path, fixture / "src" / source_path.name)
    for header in REQUIRED_HEADERS:
        source_header = args.generated_dir / "inc" / header
        if not source_header.exists():
            raise ValueError(f"missing generated header {header}")
        shutil.copyfile(source_header, fixture / "inc" / header)

    header = f"""#ifndef NE16_DORY_GENERATED_TILE_DATA_H
#define NE16_DORY_GENERATED_TILE_DATA_H
#include <stdint.h>
#define DORY_TILE_INPUT_BYTES {len(input_data)}
#define DORY_TILE_WEIGHTS_BYTES {len(weights)}
#define DORY_TILE_OUTPUT_BYTES {len(expected)}
extern const uint8_t dory_tile_input[DORY_TILE_INPUT_BYTES];
extern const uint8_t dory_tile_weights[DORY_TILE_WEIGHTS_BYTES];
extern const uint8_t dory_tile_expected[DORY_TILE_OUTPUT_BYTES];
#endif
"""
    (fixture / "dory_generated_tile_data.h").write_text(header)
    source_data = """/* Generated by import-dory-ne16-generated-tile.py. */
#include \"dory_generated_tile_data.h\"

""" + c_array("dory_tile_input", input_data) + "\n\n" + c_array("dory_tile_weights", weights) + "\n\n" + c_array("dory_tile_expected", expected) + "\n"
    (fixture / "dory_generated_tile_data.c").write_text(source_data)

    metadata = {
        "layer_name": layer_name,
        "tile_count": 1,
        "dimensions": dimensions,
        "l1_requirement": args.l1_requirement,
        "padding_guard": args.padding_guard,
        "guarded_l1_requirement": total_l1,
        "dory_commit": args.dory_commit,
        "config": args.config,
        "config_sha256": args.config_sha256,
        "input_sha256": sha256(input_data),
        "weights_sha256": sha256(weights),
        "expected_sha256": sha256(expected),
    }
    (fixture / "generated_tile_metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
