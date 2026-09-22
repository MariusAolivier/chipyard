#!/usr/bin/env python3
"""Host-side tests for the generalized DORY NE16 network importer."""

import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))


def source(commit, height, width, channels, tile_grid):
    return f"""/* DORY_COMMIT: {commit} */
#include "execute.h"
#include "pulp_nnx.h"
#include "pulp_nnx_util.h"

static const TileIndex end_index = {{
    .height = {tile_grid[0]},
    .width = {tile_grid[1]},
    .output_channel = {tile_grid[2]}
}};
static const Layer body = {{
    .input = {{
        .height = {height},
        .width = {width},
        .channel = {channels}
    }},
    .output = {{
        .height = {height},
        .width = {width},
        .channel = {channels}
    }}
}};
static void generated(void) {{
    nnx_task_set_dims();
    nnx_task_set_ptrs();
    nnx_dispatch_check_blocking();
    nnx_dispatch_task();
    nnx_resolve_check();
}}
"""


def make_fixture(root):
    commit = "dory-test-commit-2026"
    config = b'{"engine":"ne16","quantization":"int8"}\n'
    write(root / "config.json", config)
    headers = {}
    for name in ("execute.h", "pulp_nnx.h", "pulp_nnx_util.h"):
        data = f"/* {name} */\n".encode()
        write(root / "inc" / name, data)
        headers[name] = {"path": f"inc/{name}", "size": len(data), "sha256": sha256(data)}

    layers = []
    for index in range(3):
        dims = (2 + index, 3, 2)
        grid = (2, 2, 1) if index == 1 else (1, 1, 1)
        source_data = source(commit, *dims, grid).encode()
        source_path = root / "src" / f"layer{index}.c"
        write(source_path, source_data)
        input_data = bytes((index + n) & 0xff for n in range(dims[0] * dims[1] * dims[2]))
        parameters = bytes((0x40 + index + n) & 0xff for n in range(12))
        reference = bytes((0x80 + index + n) & 0xff for n in range(dims[0] * dims[1] * dims[2]))
        records = {}
        for key, filename, data in (
            ("input", f"input{index}.bin", input_data),
            ("parameters", f"parameters{index}.bin", parameters),
            ("reference", f"reference{index}.bin", reference),
        ):
            write(root / filename, data)
            records[key] = {"path": filename, "size": len(data), "sha256": sha256(data)}
        layers.append({
            "name": f"layer{index}", "order": index,
            "source": {"path": f"src/layer{index}.c", "size": len(source_data), "sha256": sha256(source_data)},
            "headers": list(headers.values()),
            "dimensions": {
                "input": list(dims), "output": list(dims),
                "input_width_bits": 8, "parameter_width_bits": 8, "output_width_bits": 8,
            },
            "tile_grid": list(grid),
            "l1_regions": [
                {"name": "input", "offset": 128 + index * 1024, "size": 128, "guard": 8},
                {"name": "output", "offset": 512 + index * 1024, "size": 128, "guard": 8},
            ],
            **records,
        })
    manifest = {
        "schema_version": 1, "engine": "ne16", "dory": {"commit": commit},
        "config": {"path": "config.json", "size": len(config), "sha256": sha256(config)},
        "l1": {
            "capacity_bytes": 65536,
            "padding_guard_bytes": 2048,
            "suffix_guard_bytes": 256,
            "live_limit_bytes": 63232,
        },
        "layers": layers,
    }
    write(root / "manifest.json", json.dumps(manifest, indent=2) + "\n")


def run(importer, manifest, output):
    return subprocess.run(
        [sys.executable, str(importer), "--manifest", str(manifest), "--output-dir", str(output)],
        capture_output=True, text=True,
    )


def rejected(importer, root, label):
    result = run(importer, root / "manifest.json", root / "output")
    if result.returncode == 0:
        raise AssertionError(f"{label} was accepted")


def mutate_case(base, name, mutate, importer):
    case = base.parent / name
    shutil.copytree(base, case)
    mutate(case)
    rejected(importer, case, name)


def main():
    repo = Path(__file__).resolve().parents[3]
    importer = repo / "generators" / "ne16" / "scripts" / "import-dory-ne16-generated-network.py"
    work = repo / ".dory-ne16-network-host-test"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    try:
        base = work / "valid"
        make_fixture(base)
        first = run(importer, base / "manifest.json", base / "output-1")
        if first.returncode != 0:
            raise AssertionError(first.stderr)
        second = run(importer, base / "manifest.json", base / "output-2")
        if second.returncode != 0:
            raise AssertionError(second.stderr)
        files = sorted(path.relative_to(base / "output-1") for path in (base / "output-1").rglob("*") if path.is_file())
        other = sorted(path.relative_to(base / "output-2") for path in (base / "output-2").rglob("*") if path.is_file())
        if files != other or any(
            (base / "output-1" / path).read_bytes() != (base / "output-2" / path).read_bytes() for path in files
        ):
            raise AssertionError("repeated imports are not byte-identical")
        metadata = json.loads((base / "output-1" / "generated_network_metadata.json").read_text())
        if len(metadata["layers"]) != 3 or metadata["layers"][1]["tile_grid"] != [2, 2, 1]:
            raise AssertionError("three-layer or multi-tile metadata was not generated")
        if not (base / "output-1/src/layer0.c").is_file() or not (
            base / "output-1/inc/execute.h"
        ).is_file():
            raise AssertionError("generated sources and headers were not copied")

        def wrong_config(case):
            data = json.loads((case / "manifest.json").read_text())
            data["config"]["sha256"] = "0" * 64
            write(case / "manifest.json", json.dumps(data))
        mutate_case(base, "wrong-config-hash", wrong_config, importer)

        def wrong_commit(case):
            data = json.loads((case / "manifest.json").read_text())
            data["dory"]["commit"] = "wrong-commit"
            write(case / "manifest.json", json.dumps(data))
        mutate_case(base, "wrong-dory-commit", wrong_commit, importer)
        mutate_case(base, "missing-source", lambda case: (case / "src/layer1.c").unlink(), importer)
        mutate_case(base, "missing-header", lambda case: (case / "inc/execute.h").unlink(), importer)

        def reorder(case):
            data = json.loads((case / "manifest.json").read_text())
            data["layers"][0], data["layers"][1] = data["layers"][1], data["layers"][0]
            write(case / "manifest.json", json.dumps(data))
        mutate_case(base, "reordered-layers", reorder, importer)

        for filename, label in (("input0.bin", "truncated-input"), ("parameters0.bin", "truncated-parameters"),
                                ("reference0.bin", "truncated-reference")):
            def truncate(case, filename=filename):
                path = case / filename
                path.write_bytes(path.read_bytes()[:-1])
            mutate_case(base, label, truncate, importer)

        def bad_dimensions(case):
            data = json.loads((case / "manifest.json").read_text())
            data["layers"][0]["dimensions"]["input"][0] = 99
            write(case / "manifest.json", json.dumps(data))
        mutate_case(base, "bad-dimensions", bad_dimensions, importer)

        def remove_nnx(case):
            path = case / "src/layer0.c"
            path.write_text(path.read_text().replace("nnx_task_set_dims();", "missing_task_set_dims();"))
        mutate_case(base, "missing-nnx-symbol", remove_nnx, importer)
        def pulp_call(case):
            path = case / "src/layer0.c"
            path.write_text(path.read_text() + "\nvoid pulp_nn_conv_fallback(void) {}\n")
        mutate_case(base, "pulp-nn-call", pulp_call, importer)

        def l1_out_of_range(case):
            data = json.loads((case / "manifest.json").read_text())
            data["layers"][0]["l1_regions"][0]["offset"] = 65530
            write(case / "manifest.json", json.dumps(data))
        mutate_case(base, "l1-out-of-range", l1_out_of_range, importer)
        def l1_overlap(case):
            data = json.loads((case / "manifest.json").read_text())
            data["layers"][0]["l1_regions"][1]["offset"] = 200
            write(case / "manifest.json", json.dumps(data))
        mutate_case(base, "l1-overlap", l1_overlap, importer)

        def l1_live_limit(case):
            data = json.loads((case / "manifest.json").read_text())
            data["layers"][0]["l1_regions"][0]["offset"] = 2048
            data["layers"][0]["l1_regions"][0]["size"] = 63233
            write(case / "manifest.json", json.dumps(data))
        mutate_case(base, "l1-live-limit", l1_live_limit, importer)
        def integer_overflow(case):
            data = json.loads((case / "manifest.json").read_text())
            data["layers"][0]["dimensions"]["input"] = [2**62, 2, 2]
            write(case / "manifest.json", json.dumps(data))
        mutate_case(base, "integer-overflow", integer_overflow, importer)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("DORY NE16 network importer tests PASS")


if __name__ == "__main__":
    main()
