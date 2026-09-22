#!/usr/bin/env python3
"""Generate a deterministic all-NE16 DORY network without modifying DORY.

The pinned DORY layer generator creates one layer at a time.  This wrapper
reuses its graph, NE16 parser, and C parser in a temporary DORY checkout,
feeding each generated layer's quantized output into the next layer.
"""

import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_hex(path):
    data = path.read_bytes()
    if not data:
        raise ValueError(f"{path} is empty")
    return data


def find_layer_source(app_dir, function_name):
    candidates = []
    for path in sorted((app_dir / "src").rglob("*.c")):
        text = path.read_text(encoding="utf-8")
        if re.search(rf"\bvoid\s+{re.escape(function_name)}\s*\(", text):
            candidates.append((path, text))
    if len(candidates) != 1:
        raise ValueError(
            f"expected one generated source for {function_name}, found "
            f"{len(candidates)}"
        )
    return candidates[0]


def find_function_name(source, preferred):
    match = re.search(
        r"(?m)^\s*void\s+([A-Za-z_][A-Za-z0-9_]*)\s*"
        r"\(\s*void\s*\*args\s*\)",
        source,
    )
    if match is None:
        raise ValueError(f"could not identify generated layer entry point in {preferred}")
    return match.group(1)


def parse_tile_grid(source):
    match = re.search(
        r"\b(?:static\s+)?const\s+TileIndex\s+end_index\s*=\s*\{(.*?)\};",
        source,
        re.S,
    )
    if match is None:
        raise ValueError("generated source does not declare end_index")
    fields = dict(
        (name, int(value))
        for name, value in re.findall(
            r"\.(height|width|output_channel)\s*=\s*(-?\d+)", match.group(1)
        )
    )
    if set(fields) != {"height", "width", "output_channel"} or any(
        value < 1 for value in fields.values()
    ):
        raise ValueError("generated source declares an invalid tile grid")
    return [
        fields["height"],
        fields["width"],
        fields["output_channel"],
    ]


def parse_l1_usage(source, live_limit):
    usage = 0
    for name in ("input", "output", "weights", "scale", "bias"):
        base = re.search(
            rf"l1_buffer_{name}\s*=\s*l1_buffer\s*\+\s*(\d+)",
            source,
        )
        if base is None:
            continue
        usage = max(usage, int(base.group(1)))
        double_buffer = re.search(
            rf"=\s*l1_buffer_{name}\s*\+\s*(\d+)",
            source,
        )
        if double_buffer is not None:
            usage = max(usage, int(base.group(1)) + int(double_buffer.group(1)))
    if usage == 0:
        raise ValueError("could not find generated L1 offsets")
    if usage <= 0 or usage > live_limit:
        raise ValueError(f"generated L1 usage {usage} exceeds live limit {live_limit}")
    return usage


def copy_headers(app_dir, destination):
    headers = {}
    for path in sorted((app_dir / "inc").rglob("*.h")):
        relative = path.relative_to(app_dir / "inc")
        target = destination / "inc" / relative
        data = path.read_bytes()
        if relative in headers and headers[relative] != data:
            raise ValueError(f"generated header differs between layers: {relative}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        headers[relative] = data
    if not headers:
        raise ValueError(f"generated application has no headers: {app_dir}")
    return headers


def adapt_single_core_source(source):
    replacements = {
        "#define EXECUTER_ID (1)": "#define EXECUTER_ID (0)",
        "#define STORER_ID (2)": "#define STORER_ID (0)",
        "pi_cl_team_fork(CORES, (void *)layer_task_fork, args);":
        "layer_task_fork(args);",
    }
    for old, new in replacements.items():
        if old not in source:
            raise ValueError(f"generated source is missing expected text: {old}")
        source = source.replace(old, new)
    if "pi_cl_" in source:
        raise ValueError("generated source still contains multi-core PULP calls")
    return source


def write_single_core_monitor_header(destination):
    (destination / "inc" / "monitor.h").write_text(
        """#ifndef CHIPYARD_DORY_MONITOR_H
#define CHIPYARD_DORY_MONITOR_H

typedef struct {
    int unused;
} Monitor;

static inline int monitor_init(Monitor *monitor, int buffer_size) {
    (void)monitor;
    (void)buffer_size;
    return 0;
}

static inline void monitor_term(Monitor monitor) {
    (void)monitor;
}

static inline void monitor_produce_begin(Monitor monitor) {
    (void)monitor;
}

static inline void monitor_produce_end(Monitor monitor) {
    (void)monitor;
}

static inline void monitor_consume_begin(Monitor monitor) {
    (void)monitor;
}

static inline void monitor_consume_end(Monitor monitor) {
    (void)monitor;
}

#endif
""",
        encoding="utf-8",
        newline="\n",
    )


def write_network_header(destination):
    (destination / "inc" / "network.h").write_text(
        """#ifndef CHIPYARD_DORY_NETWORK_H
#define CHIPYARD_DORY_NETWORK_H
#endif
""",
        encoding="utf-8",
        newline="\n",
    )


def write_compatibility_headers(destination, chipyard_root):
    compatibility_dir = chipyard_root / "tests" / "dory_ne16_compat"
    for name in ("ne16_hal.h", "pulp_nnx.h", "pulp_nnx_util.h"):
        source = compatibility_dir / name
        if not source.is_file():
            raise ValueError(f"missing Chipyard compatibility header: {source}")
        shutil.copy2(source, destination / "inc" / name)


def record(path, root):
    data = path.read_bytes()
    return {
        "path": path.relative_to(root).as_posix(),
        "size": len(data),
        "sha256": sha256(data),
    }


def load_generator(dory_copy):
    sys.path.insert(0, str(dory_copy))
    path = dory_copy / "layer_generate_ne16.py"
    spec = importlib.util.spec_from_file_location("chipyard_layer_generate_ne16", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generate_layer(generator, params, layer_index, network_dir, app_dir, input_tensor):
    layer_node = generator.create_layer_node(params, layer_index * 2)
    dory_node = generator.create_dory_node(params, layer_index * 2 + 1)
    output_tensor = generator.create_layer(
        layer_index,
        layer_node,
        dory_node,
        str(network_dir),
        "PULP.GAP9_NE16",
        input=input_tensor,
    )
    parser_module = importlib.import_module(
        "dory.Hardware_targets.PULP.GAP9_NE16.HW_Parser"
    )
    graph = parser_module.onnx_manager(
        [layer_node, dory_node],
        params,
        str(network_dir),
    ).full_graph_parsing()
    c_parser_module = importlib.import_module(
        "dory.Hardware_targets.PULP.GAP9_NE16.C_Parser"
    )
    c_parser_module.C_Parser(
        graph,
        params,
        str(network_dir),
        "None",
        "No",
        "8bit",
        str(app_dir),
    ).full_graph_parsing()
    return output_tensor


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dory-root", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--chipyard-root",
        type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    args = parser.parse_args()

    config_path = args.config.resolve()
    config = json.loads(config_path.read_text(encoding="utf-8"))
    if config.get("hardware_target") != "PULP.GAP9_NE16":
        raise ValueError("network config must target PULP.GAP9_NE16")
    chipyard_root = args.chipyard_root.resolve()
    base_config = chipyard_root / config["base_config"]
    pinned_base = subprocess.run(
        ["git", "show", f"HEAD:{Path(config['base_config']).as_posix()}"],
        cwd=chipyard_root,
        check=False,
        capture_output=True,
    )
    base_hash = (
        sha256(pinned_base.stdout)
        if pinned_base.returncode == 0
        else sha256(base_config.read_bytes())
    )
    if base_hash != config["base_config_sha256"]:
        raise ValueError(f"pinned base config hash does not match {base_config}")
    dory_root = args.dory_root.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    raw = output / "raw"
    if raw.exists():
        shutil.rmtree(raw)
    raw.mkdir()
    shutil.copy2(config_path, raw / "generation_config.json")
    hardware_description = (
        args.chipyard_root.resolve()
        / config["hardware_description"]
    )
    dory_description = (
        dory_root
        / "dory"
        / "Hardware_targets"
        / "PULP"
        / "GAP9_NE16"
        / "HW_description.json"
    )

    with tempfile.TemporaryDirectory(prefix="dory-chipyard-") as temporary:
        dory_copy = Path(temporary) / "dory"
        dory_copy.mkdir()
        shutil.copy2(dory_root / "layer_generate_ne16.py", dory_copy)
        shutil.copytree(
            dory_root / "dory",
            dory_copy / "dory",
            symlinks=True,
            ignore=shutil.ignore_patterns("__pycache__", ".pytest_cache"),
        )
        dory_description.unlink()
        shutil.copy2(hardware_description, dory_description)
        gap9_templates = (
            dory_copy
            / "dory"
            / "Hardware_targets"
            / "PULP"
            / "GAP9"
            / "Templates"
        )
        compatibility_templates = (
            dory_copy
            / "dory"
            / "Hardware_targets"
            / "Chipyard"
            / "NE16"
            / "Templates"
        )
        if gap9_templates.exists():
            shutil.copytree(
                gap9_templates,
                compatibility_templates,
                dirs_exist_ok=True,
            )
        generator = load_generator(dory_copy)
        import torch

        torch.manual_seed(int(config.get("seed", 0)))
        input_tensor = None
        manifest_layers = []
        shared_headers = {}
        for index, params in enumerate(config["layers"]):
            parser_params = dict(params)
            parser_params["onnx_file"] = config["onnx_file"]
            parser_params["code reserved space"] = config["code reserved space"]
            layer_raw = raw / f"layer{index}"
            layer_raw.mkdir()
            for previous_index in range(index):
                previous_output = (
                    raw
                    / f"layer{previous_index}"
                    / f"out_layer{previous_index}.txt"
                )
                shutil.copy2(
                    previous_output,
                    layer_raw / f"out_layer{previous_index}.txt",
                )
            app_dir = layer_raw / "application"
            input_tensor = generate_layer(
                generator,
                parser_params,
                index,
                layer_raw,
                app_dir,
                input_tensor,
            )
            source_candidates = sorted((app_dir / "src").rglob("*.c"))
            source_path = None
            for candidate in source_candidates:
                candidate_text = candidate.read_text(encoding="utf-8")
                if re.search(
                    r"\bvoid\s+[A-Za-z_][A-Za-z0-9_]*\s*\(\s*void\s*\*args\s*\)",
                    candidate_text,
                ):
                    source_path = candidate
                    break
            if source_path is None:
                raise ValueError(f"layer {index} has no generated entry point")
            source = source_path.read_text(encoding="utf-8")
            generated_function_name = find_function_name(source, source_path.name)
            function_name = re.sub(
                r"\d+$",
                str(index),
                generated_function_name,
            )
            if not re.search(r"\d+$", generated_function_name):
                function_name = f"{generated_function_name}{index}"
            source = re.sub(
                rf"\b{re.escape(generated_function_name)}\b",
                function_name,
                source,
            )
            source = adapt_single_core_source(source)
            source = f"/* DORY_COMMIT: {config['dory_commit']} */\n" + source
            source_path = layer_raw / f"{function_name}.c"
            source_path.write_text(source, encoding="utf-8", newline="\n")
            headers = copy_headers(app_dir, layer_raw)
            generated_header = layer_raw / "inc" / f"{generated_function_name}.h"
            if generated_header.exists():
                header_path = layer_raw / "inc" / f"{function_name}.h"
                header_text = generated_header.read_text(encoding="utf-8")
                header_text = re.sub(
                    rf"\b{re.escape(generated_function_name)}\b",
                    function_name,
                    header_text,
                )
                header_path.write_text(header_text, encoding="utf-8", newline="\n")
                if header_path != generated_header:
                    generated_header.unlink()
                    headers.pop(Path(f"{generated_function_name}.h"), None)
                    headers[Path(f"{function_name}.h")] = True
            write_single_core_monitor_header(layer_raw)
            write_network_header(layer_raw)
            write_compatibility_headers(layer_raw, chipyard_root)
            headers[Path("monitor.h")] = True
            headers[Path("network.h")] = True
            shared_headers.update(headers)
            input_hex = app_dir / "hex" / "inputs.hex"
            weights_hex = (
                app_dir / "hex" / f"{generated_function_name}_weights.hex"
            )
            if not input_hex.exists() or not weights_hex.exists():
                raise ValueError(f"layer {index} is missing generated hex data")
            input_data = read_hex(input_hex)
            parameter_data = read_hex(weights_hex)
            reference_values = [
                int(value)
                for value in re.findall(
                    r"-?\d+",
                    (layer_raw / f"out_layer{index}.txt").read_text(
                        encoding="utf-8"
                    ),
                )
            ]
            reference_data = bytes(value & 0xFF for value in reference_values)
            layer_input = raw / f"layer{index}_input.bin"
            layer_parameters = raw / f"layer{index}_parameters.bin"
            layer_reference = raw / f"layer{index}_reference.bin"
            layer_input.write_bytes(input_data)
            layer_parameters.write_bytes(parameter_data)
            layer_reference.write_bytes(reference_data)
            dims = {
                "input": [
                    params["input_dimensions"][0],
                    params["input_dimensions"][1],
                    params["input_channels"],
                ],
                "output": [
                    params["output_dimensions"][0],
                    params["output_dimensions"][1],
                    params["output_channels"],
                ],
                "input_width_bits": params["input_bits"],
                "parameter_width_bits": params["weight_bits"],
                "output_width_bits": params["output_bits"],
            }
            l1_usage = parse_l1_usage(
                source, config["l1"]["live_limit_bytes"]
            )
            manifest_layers.append(
                {
                    "name": function_name,
                    "order": index,
                    "source": record(source_path, raw),
                    "headers": [
                        record(layer_raw / "inc" / path, raw)
                        for path in sorted(headers)
                    ],
                    "dimensions": dims,
                    "tile_grid": parse_tile_grid(source),
                    "l1_regions": [
                        {
                            "name": "generated-live-l1",
                            "offset": config["l1"]["padding_guard_bytes"],
                            "size": l1_usage,
                            "guard_before": config["l1"]["padding_guard_bytes"],
                            "guard_after": config["l1"]["suffix_guard_bytes"],
                        }
                    ],
                    "input": record(layer_input, raw),
                    "parameters": record(layer_parameters, raw),
                    "reference": record(layer_reference, raw),
                }
            )
            # The next layer consumes the actual quantized output produced by
            # the DORY reference calculation, not a freshly randomized tensor.
            input_tensor = input_tensor.to(dtype=torch.int64)
            if len(reference_data) != int(input_tensor.numel()):
                raise ValueError(
                    f"layer {index} reference size does not match generated output"
                )

        manifest = {
            "schema_version": 1,
            "engine": "ne16",
            "dory": {
                "repository": config["dory_repository"],
                "commit": config["dory_commit"],
            },
            "config": {
                "path": "generation_config.json",
                "size": (raw / "generation_config.json").stat().st_size,
                "sha256": sha256((raw / "generation_config.json").read_bytes()),
            },
            "l1": config["l1"],
            "layers": manifest_layers,
        }
        (raw / "network_manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        importer = (
            args.chipyard_root.resolve()
            / "generators"
            / "ne16"
            / "scripts"
            / "import-dory-ne16-generated-network.py"
        )
        subprocess.run(
            [
                sys.executable,
                str(importer),
                "--manifest",
                str(raw / "network_manifest.json"),
                "--output-dir",
                str(output),
            ],
            check=True,
        )
    print(f"Generated deterministic DORY NE16 network in {output}")


if __name__ == "__main__":
    main()
