#!/usr/bin/env python3
"""Import a manifest-described, multi-layer DORY NE16 network.

The manifest is deliberately explicit: paths, layer order, tensor sizes, hashes,
tile grids, and L1 regions are all declared rather than inferred from filenames.
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


MAX_INT = (1 << 63) - 1
L1_BYTES = 64 * 1024
REQUIRED_NNX_CALLS = (
    "nnx_task_set_dims",
    "nnx_task_set_ptrs",
    "nnx_dispatch_check_blocking",
    "nnx_dispatch_task",
    "nnx_resolve_check",
)
REQUIRED_INCLUDES = ("execute.h", "pulp_nnx.h", "pulp_nnx_util.h")
FORBIDDEN_CALLS = (
    re.compile(r"\bpulp_nn_[A-Za-z0-9_]*\s*\("),
    re.compile(r"\bcluster_[A-Za-z0-9_]*\s*\("),
    re.compile(r"\bpi_cl_[A-Za-z0-9_]*\s*\("),
    re.compile(r"\b(?:pulp|cluster)_[A-Za-z0-9_]*(?:fallback|fallback_[A-Za-z0-9_]*)\s*\("),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked_int(value, label, minimum=0):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} must be an integer")
    if value < minimum or value > MAX_INT:
        raise ValueError(f"{label} is outside the checked integer range")
    return value


def checked_product(values, label):
    result = 1
    for value in values:
        checked_int(value, label, 1)
        if result > MAX_INT // value:
            raise ValueError(f"{label} overflows checked integer arithmetic")
        result *= value
    return result


def checked_multiply(left, right, label):
    checked_int(left, f"{label} left operand", 0)
    checked_int(right, f"{label} right operand", 0)
    if left and right > MAX_INT // left:
        raise ValueError(f"{label} overflows checked integer arithmetic")
    return left * right


def resolve_file(root, value, label):
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} path must be a non-empty string")
    path = (root / value).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        raise ValueError(f"{label} path escapes the manifest directory: {value}")
    if not path.is_file():
        raise ValueError(f"missing {label}: {value}")
    return path


def file_record(root, record, label):
    if isinstance(record, str):
        raise ValueError(f"{label} must declare path, size, and sha256")
    if not isinstance(record, dict):
        raise ValueError(f"{label} must be an object")
    path = resolve_file(root, record.get("path"), label)
    expected_size = checked_int(record.get("size"), f"{label} size")
    expected_hash = record.get("sha256")
    if not isinstance(expected_hash, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected_hash):
        raise ValueError(f"{label} sha256 must be a 64-character hexadecimal digest")
    data = path.read_bytes()
    if len(data) != expected_size:
        raise ValueError(f"{label} size {len(data)} does not match manifest size {expected_size}")
    actual_hash = digest(data)
    if actual_hash.lower() != expected_hash.lower():
        raise ValueError(f"{label} sha256 {actual_hash} does not match manifest {expected_hash}")
    return path, data


def tensor_record(root, layer, key, label):
    record = layer.get(key)
    if record is None:
        raise ValueError(f"layer {layer.get('name', '?')} is missing {label}")
    return file_record(root, record, f"layer {layer.get('name', '?')} {label}")


def parse_triplet(value, label):
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError(f"{label} must be a three-element list")
    return tuple(checked_int(item, f"{label}[{index}]", 1) for index, item in enumerate(value))


def parse_body_dimensions(source, layer_name):
    match = re.search(r"\b(?:static\s+)?const\s+Layer\s+body\s*=\s*\{(.*?)\n\s*\};", source, re.S)
    if match is None:
        raise ValueError(f"layer {layer_name} source has no Layer body dimensions")
    body = match.group(1)
    fields = re.findall(
        r"\.(input|output)\s*=\s*\{\s*"
        r"\.height\s*=\s*(-?\d+)\s*,\s*"
        r"\.width\s*=\s*(-?\d+)\s*,\s*"
        r"\.channel\s*=\s*(-?\d+)",
        body,
        re.S,
    )
    if len(fields) != 2:
        raise ValueError(f"layer {layer_name} source has incomplete body dimensions")
    result = {}
    for name, height, width, channel in fields:
        values = (int(height), int(width), int(channel))
        if any(value < 1 for value in values):
            raise ValueError(f"layer {layer_name} source has non-positive {name} dimensions")
        result[name] = values
    return result


def parse_tile_grid(source, layer_name):
    match = re.search(r"\b(?:static\s+)?const\s+TileIndex\s+end_index\s*=\s*\{(.*?)\};", source, re.S)
    if match is None:
        raise ValueError(f"layer {layer_name} source has no tile grid")
    fields = dict((name, int(value)) for name, value in re.findall(
        r"\.(height|width|output_channel)\s*=\s*(-?\d+)", match.group(1)
    ))
    if set(fields) != {"height", "width", "output_channel"} or any(value < 1 for value in fields.values()):
        raise ValueError(f"layer {layer_name} source has invalid tile grid")
    return (fields["height"], fields["width"], fields["output_channel"])


def validate_generated_source(source, layer_name, expected_commit, support_text=""):
    source_and_support = source + "\n" + support_text
    commit = re.search(r"DORY(?:[_ ]COMMIT)\s*[:=]\s*([0-9A-Za-z._-]+)", source, re.I)
    if commit is None or commit.group(1) != expected_commit:
        actual = commit.group(1) if commit else "missing"
        raise ValueError(f"layer {layer_name} DORY commit {actual!r} does not match manifest {expected_commit!r}")
    for include in REQUIRED_INCLUDES:
        if re.search(rf"#include\s+[<\"]{re.escape(include)}[>\"]", source) is None:
            raise ValueError(f"layer {layer_name} is missing required include {include}")
    missing = [
        name
        for name in REQUIRED_NNX_CALLS
        if re.search(rf"\b{re.escape(name)}\s*\(", source_and_support) is None
    ]
    if missing:
        raise ValueError(f"layer {layer_name} is missing NNX calls: {', '.join(missing)}")
    for pattern in FORBIDDEN_CALLS:
        if pattern.search(source_and_support):
            raise ValueError(f"layer {layer_name} contains a forbidden cluster/PULP fallback call")


def validate_l1(layer, capacity, padding_guard, suffix_guard, live_limit):
    regions = layer.get("l1_regions")
    if not isinstance(regions, list) or not regions:
        raise ValueError(f"layer {layer.get('name', '?')} must declare l1_regions")
    intervals = []
    for index, region in enumerate(regions):
        if not isinstance(region, dict):
            raise ValueError(f"l1_regions[{index}] must be an object")
        name = region.get("name", f"region-{index}")
        offset = checked_int(region.get("offset"), f"{name} offset")
        size = checked_int(region.get("size"), f"{name} size", 1)
        before = checked_int(region.get("guard_before", 0), f"{name} guard_before")
        after = checked_int(region.get("guard_after", region.get("guard", 0)), f"{name} guard_after")
        start = offset - before
        end = offset + size + after
        if start < 0 or end > capacity:
            raise ValueError(
                f"{name} including guards is outside the {capacity}-byte L1"
            )
        intervals.append((start, end, name))
    live_end_limit = padding_guard + live_limit
    for region in regions:
        offset = checked_int(region.get("offset"), "L1 region offset")
        size = checked_int(
            region.get("size"), "L1 region size", 1
        )
        if offset + size > live_end_limit:
            raise ValueError(
                f"{region.get('name', 'unnamed')} live region exceeds "
                f"the guarded {live_limit}-byte L1 limit"
            )
    for left, right in zip(sorted(intervals), sorted(intervals)[1:]):
        if left[1] > right[0]:
            raise ValueError(f"L1 regions {left[2]} and {right[2]} overlap")
    return intervals


def safe_name(name):
    result = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not result or result[0].isdigit():
        result = "layer_" + result
    return result


def c_array(name, data):
    lines = [f"const uint8_t {name}[{len(data)}] = {{"]
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def import_network(manifest_path, output_dir):
    manifest_path = manifest_path.resolve()
    root = manifest_path.parent
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read manifest: {error}")
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        raise ValueError("manifest schema_version must be 1")
    if manifest.get("engine") != "ne16":
        raise ValueError("manifest engine must be exactly 'ne16'")
    l1 = manifest.get("l1", {})
    if not isinstance(l1, dict):
        raise ValueError("manifest l1 metadata must be an object")
    l1_capacity = checked_int(l1.get("capacity_bytes", L1_BYTES), "L1 capacity", 1)
    padding_guard = checked_int(l1.get("padding_guard_bytes", 0), "L1 prefix guard")
    suffix_guard = checked_int(l1.get("suffix_guard_bytes", 0), "L1 suffix guard")
    live_limit = checked_int(
        l1.get("live_limit_bytes", l1_capacity - padding_guard - suffix_guard),
        "L1 live limit",
    )
    if padding_guard + live_limit + suffix_guard > l1_capacity:
        raise ValueError("guarded L1 live limit exceeds physical L1 capacity")

    dory = manifest.get("dory")
    if not isinstance(dory, dict) or not isinstance(dory.get("commit"), str) or not dory["commit"]:
        raise ValueError("manifest must declare dory.commit metadata")
    commit = dory["commit"]
    config = manifest.get("config")
    if not isinstance(config, dict):
        raise ValueError("manifest must declare config.path and config.sha256")
    config_path = resolve_file(root, config.get("path"), "generation config")
    config_hash = config.get("sha256")
    if not isinstance(config_hash, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", config_hash):
        raise ValueError("generation config sha256 must be a 64-character hexadecimal digest")
    actual_config_hash = digest(config_path.read_bytes())
    if actual_config_hash.lower() != config_hash.lower():
        raise ValueError(f"generation config sha256 {actual_config_hash} does not match manifest {config_hash}")

    layers = manifest.get("layers")
    if not isinstance(layers, list) or not layers:
        raise ValueError("manifest must contain a non-empty layers list")
    validated = []
    names = set()
    for index, layer in enumerate(layers):
        if not isinstance(layer, dict):
            raise ValueError(f"layers[{index}] must be an object")
        if layer.get("order") != index:
            raise ValueError(f"layer {layer.get('name', '?')} is out of manifest order")
        name = layer.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("layer names must be unique, non-empty strings")
        names.add(name)
        source_path, source_bytes = file_record(root, layer.get("source"), f"layer {name} source")
        source = source_bytes.decode("utf-8")
        dimensions = layer.get("dimensions")
        if not isinstance(dimensions, dict):
            raise ValueError(f"layer {name} must declare dimensions")
        input_dims = parse_triplet(dimensions.get("input"), f"layer {name} input dimensions")
        output_dims = parse_triplet(dimensions.get("output"), f"layer {name} output dimensions")
        widths = {}
        for key in ("input_width_bits", "parameter_width_bits", "output_width_bits"):
            widths[key] = checked_int(dimensions.get(key), f"layer {name} {key}", 1)
            if widths[key] not in (1, 2, 4, 8, 16, 32):
                raise ValueError(f"layer {name} {key} must be one of 1, 2, 4, 8, 16, or 32")
        body_dims = parse_body_dimensions(source, name)
        if body_dims != {"input": input_dims, "output": output_dims}:
            raise ValueError(f"layer {name} source dimensions {body_dims} do not match manifest")
        grid = parse_triplet(layer.get("tile_grid"), f"layer {name} tile_grid")
        source_grid = parse_tile_grid(source, name)
        if source_grid != grid:
            raise ValueError(f"layer {name} source tile grid {source_grid} does not match manifest {grid}")
        checked_product(grid, f"layer {name} tile count")
        input_path, input_data = tensor_record(root, layer, "input", "input")
        parameter_path, parameter_data = tensor_record(root, layer, "parameters", "parameters")
        reference_path, reference_data = tensor_record(root, layer, "reference", "reference")
        input_elements = checked_product(input_dims, f"layer {name} input dimensions")
        output_elements = checked_product(output_dims, f"layer {name} output dimensions")
        input_bits = checked_multiply(input_elements, widths["input_width_bits"], f"layer {name} input size")
        output_bits = checked_multiply(output_elements, widths["output_width_bits"], f"layer {name} output size")
        if input_bits % 8 or output_bits % 8:
            raise ValueError(f"layer {name} element widths do not produce whole bytes")
        input_bytes = input_bits // 8
        output_bytes = output_bits // 8
        if len(input_data) != input_bytes:
            raise ValueError(f"layer {name} input size {len(input_data)} does not match {input_bytes}")
        if len(reference_data) != output_bytes:
            raise ValueError(f"layer {name} reference size {len(reference_data)} does not match {output_bytes}")
        validate_l1(
            layer,
            l1_capacity,
            padding_guard,
            suffix_guard,
            live_limit,
        )
        headers = layer.get("headers")
        if not isinstance(headers, list) or not headers:
            raise ValueError(f"layer {name} must declare headers")
        header_records = []
        header_text = []
        for header in headers:
            header_path, header_bytes = file_record(root, header, f"layer {name} header")
            header_records.append((header_path, header_bytes, header))
            header_text.append(header_bytes.decode("utf-8"))
        validate_generated_source(source, name, commit, "\n".join(header_text))
        declared_header_names = {path.name for path, _, _ in header_records}
        missing_headers = [include for include in REQUIRED_INCLUDES if include not in declared_header_names]
        if missing_headers:
            raise ValueError(f"layer {name} is missing header records: {', '.join(missing_headers)}")
        all_source = source + "\n" + "\n".join(header_text)
        for include in REQUIRED_INCLUDES:
            if re.search(rf"#include\s+[<\"]{re.escape(include)}[>\"]", all_source) is None:
                raise ValueError(f"layer {name} is missing required include {include}")
        validated.append({
            "name": name, "safe_name": safe_name(name), "source_path": source_path,
            "source_hash": digest(source_bytes), "source": source, "headers": header_records,
            "input_path": input_path, "input": input_data, "parameters_path": parameter_path,
            "parameters": parameter_data, "reference_path": reference_path, "reference": reference_data,
            "dimensions": dict({"input": list(input_dims), "output": list(output_dims)}, **widths),
            "tile_grid": list(grid), "l1_regions": layer["l1_regions"],
        })

    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    arrays = []
    declarations = []
    metadata_layers = []
    output_src = output_dir / "src"
    output_inc = output_dir / "inc"
    copied_headers = {}
    for layer in validated:
        output_source = output_src / layer["source_path"].name
        if output_source.exists() and output_source.read_bytes() != layer["source"].encode("utf-8"):
            raise ValueError(f"generated source name collision: {output_source.name}")
        output_source.parent.mkdir(parents=True, exist_ok=True)
        output_source.write_text(layer["source"], encoding="utf-8", newline="\n")
        output_header_records = []
        for path, data, _ in layer["headers"]:
            relative = Path(path.name)
            if relative in copied_headers and copied_headers[relative] != data:
                raise ValueError(f"generated header name collision: {relative}")
            target = output_inc / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            copied_headers[relative] = data
            output_header_records.append(
                {"path": str(Path("inc") / relative).replace("\\", "/"),
                 "sha256": digest(data)}
            )
        prefix = "dory_" + layer["safe_name"]
        for suffix, data in (("input", layer["input"]), ("parameters", layer["parameters"]), ("reference", layer["reference"])):
            symbol = f"{prefix}_{suffix}"
            declarations.append(f"extern const uint8_t {symbol}[{len(data)}];")
            arrays.append(c_array(symbol, data))
        metadata_layers.append({
            "name": layer["name"], "order": len(metadata_layers),
            "source": str(output_source.relative_to(output_dir)).replace("\\", "/"),
            "source_sha256": layer["source_hash"],
            "headers": output_header_records,
            "dimensions": layer["dimensions"], "tile_grid": layer["tile_grid"],
            "l1_regions": layer["l1_regions"],
            "input": {"bytes": len(layer["input"]), "sha256": digest(layer["input"])},
            "parameters": {"bytes": len(layer["parameters"]), "sha256": digest(layer["parameters"])},
            "reference": {"bytes": len(layer["reference"]), "sha256": digest(layer["reference"])},
        })
    macros = [
        f"#define DORY_NETWORK_LAYER_COUNT {len(validated)}",
        f"#define DORY_NETWORK_L1_CAPACITY {l1_capacity}u",
        f"#define DORY_NETWORK_L1_PADDING_GUARD {padding_guard}u",
        f"#define DORY_NETWORK_L1_SUFFIX_GUARD {suffix_guard}u",
        f"#define DORY_NETWORK_L1_LIVE_LIMIT {live_limit}u",
    ]
    for index, layer in enumerate(validated):
        prefix = "dory_" + layer["safe_name"]
        live_offset = min(
            int(region["offset"]) for region in layer["l1_regions"]
        )
        live_end = max(
            int(region["offset"]) + int(region["size"])
            for region in layer["l1_regions"]
        )
        macros.extend(
            (
                f"#define DORY_LAYER_{index}_INPUT_BYTES {len(layer['input'])}",
                f"#define DORY_LAYER_{index}_PARAMETER_BYTES {len(layer['parameters'])}",
                f"#define DORY_LAYER_{index}_OUTPUT_BYTES {len(layer['reference'])}",
                f"#define DORY_LAYER_{index}_TILE_COUNT "
                f"{layer['tile_grid'][0] * layer['tile_grid'][1] * layer['tile_grid'][2]}",
                f"#define DORY_LAYER_{index}_L1_OFFSET {live_offset}u",
                f"#define DORY_LAYER_{index}_L1_USAGE {live_end - live_offset}u",
            )
        )
    header = (
        "#ifndef DORY_NE16_GENERATED_NETWORK_DATA_H\n"
        "#define DORY_NE16_GENERATED_NETWORK_DATA_H\n"
        "#include <stdint.h>\n\n"
        + "\n".join(macros)
        + "\n\n"
        + "\n".join(declarations)
        + "\n#endif\n"
    )
    source = (
        "/* Generated by import-dory-ne16-generated-network.py. */\n"
        '#include "dory_generated_network_data.h"\n\n'
        + "\n\n".join(arrays)
        + "\n"
    )
    metadata = {
        "schema_version": 1, "engine": "ne16", "dory_commit": commit,
        "config": {"path": str(config_path.relative_to(root)).replace("\\", "/"), "sha256": config_hash.lower()},
        "l1": {
            "capacity_bytes": l1_capacity,
            "padding_guard_bytes": padding_guard,
            "suffix_guard_bytes": suffix_guard,
            "live_limit_bytes": live_limit,
        },
        "layers": metadata_layers,
    }
    (output_dir / "dory_generated_network_data.h").write_text(
        header, encoding="utf-8", newline="\n"
    )
    (output_dir / "dory_generated_network_data.c").write_text(
        source, encoding="utf-8", newline="\n"
    )
    (output_dir / "generated_network_metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )


def main():
    parser = argparse.ArgumentParser(description="Import a manifest-described DORY NE16 network")
    parser.add_argument("--manifest", required=True, type=Path, help="network manifest JSON")
    parser.add_argument("--output-dir", required=True, type=Path, help="generated output directory")
    args = parser.parse_args()
    try:
        import_network(args.manifest, args.output_dir)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
