#!/usr/bin/env python3
"""Run pinned DORY public frontend/backend APIs with genuinely linked tensors."""

import argparse
import importlib
import json
from pathlib import Path
import sys

from dory_ne16_import import sha256


def generate(args) -> dict:
    sys.path.insert(0, str(args.dory_root.resolve()))
    frontend = importlib.import_module("layer_generate_ne16")
    torch = importlib.import_module("torch")
    target = "PULP.GAP9_NE16"
    hardware_parser = importlib.import_module(f"dory.Hardware_targets.{target}.HW_Parser")
    c_parser = importlib.import_module(f"dory.Hardware_targets.{target}.C_Parser")
    output = args.output_dir.resolve()
    if output.exists():
        raise ValueError("frontend output directory already exists")
    config_bytes = [path.read_bytes().replace(b"\r\n", b"\n") for path in args.config]
    configurations = [json.loads(data) for data in config_bytes]
    first, second = configurations
    if (first["output_channels"] != second["input_channels"] or
            first["input_dimensions"] != second["input_dimensions"]):
        raise ValueError("chain tensor shapes do not match")
    output.mkdir(parents=True)
    torch.manual_seed(args.seed)
    previous = None
    records = []
    with torch.no_grad():
        for index, (data, parameters) in enumerate(zip(config_bytes, configurations)):
            bundle = output / f"layer{index}"
            config_path = bundle / "config" / "config.json"
            network = (config_path.parent / parameters["onnx_file"]).parent.resolve()
            if not network.is_relative_to(bundle.resolve()):
                raise ValueError("onnx_file escapes its private layer bundle")
            config_path.parent.mkdir(parents=True)
            config_path.write_bytes(data)
            network.mkdir(parents=True)
            convolution = frontend.create_layer_node(parameters, 0)
            normalization = frontend.create_dory_node(parameters, 1)
            incoming = None if previous is None else previous.to(torch.int64)
            previous = frontend.create_layer(
                0, convolution, normalization, str(network), target, input=incoming)
            if torch.any(previous < 0) or torch.any(previous > 255):
                raise ValueError("frontend produced a non-uint8 chain output")
            expected = previous.permute(0, 2, 3, 1).contiguous().to(torch.uint8).numpy().tobytes()
            shift = int(normalization.outshift["value"])
            graph = hardware_parser.onnx_manager(
                [convolution, normalization], parameters,
                str(config_path.parent)).full_graph_parsing()
            application = bundle / "generated"
            c_parser.C_Parser(
                graph, parameters, str(config_path.parent),
                "Check_all", "No", "8bit", str(application)).full_graph_parsing()
            packed_input = (application / "hex" / "inputs.hex").read_bytes()
            if index and packed_input != (output / "layer0" / "expected.bin").read_bytes():
                raise ValueError("second generated input differs from first full-layer output")
            (bundle / "expected.bin").write_bytes(expected)
            records.append({
                "index": index, "quantization_shift": shift,
                "input_sha256": sha256(packed_input),
                "output_sha256": sha256(expected),
                "reference": str((network / "out_layer0.txt").relative_to(output)),
            })
    manifest = {
        "seed": args.seed, "frontend": "layer_generate_ne16.create_layer",
        "link": "layer0.output -> layer1.input", "layers": records,
    }
    (output / "frontend_chain.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dory-root", required=True, type=Path)
    parser.add_argument("--config", required=True, action="append", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()
    if len(args.config) != 2 or not 0 <= args.seed <= 0xffffffff:
        parser.error("exactly two configurations and a uint32 seed are required")
    try:
        manifest = generate(args)
    except (ValueError, OSError, KeyError) as error:
        parser.error(str(error))
    print("DORY genuine chain references:", json.dumps(manifest["layers"]))


if __name__ == "__main__":
    main()
