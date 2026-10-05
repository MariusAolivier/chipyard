#!/usr/bin/env python3
"""Generate a pinned DORY convolution and import its cooperative Chipyard binding."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from dory_ne16_import import sha256


def generate(args) -> dict:
    scripts = Path(__file__).resolve().parent
    specification = importlib.util.spec_from_file_location(
        "cooperative_import", scripts / "import-dory-ne16-cooperative-layer.py")
    importer = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(importer)
    root = args.dory_root.resolve()
    revision = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
        capture_output=True, text=True, timeout=30).stdout.strip()
    if revision != importer.DORY_COMMIT:
        raise ValueError(f"DORY checkout must be pinned to {importer.DORY_COMMIT}, found {revision}")
    hardware = Path("dory/Hardware_targets/PULP/GAP9_NE16/HW_description.json")
    changed = subprocess.run(
        ["git", "-C", str(root), "diff", "HEAD", "--name-only"], check=True,
        capture_output=True, text=True, timeout=30).stdout.splitlines()
    if any(name != hardware.as_posix() for name in changed):
        raise ValueError("pinned DORY generator sources must be clean; only HW_description.json may differ")
    config_bytes = args.config.read_bytes().replace(b"\r\n", b"\n")
    config = json.loads(config_bytes)
    importer.validate_config(config)
    if type(config.get("onnx_file")) is not str:
        raise ValueError("onnx_file must be a relative staging path")
    output = args.output_dir.resolve()
    if output.exists():
        raise ValueError("output directory already exists; generate into a fresh directory")
    with tempfile.TemporaryDirectory(prefix="dory-ne16-generation-") as temporary:
        staging = Path(temporary)
        staged_config = staging / "config" / "config.json"
        tensor_directory = (staged_config.parent / config["onnx_file"]).parent.resolve()
        if not tensor_directory.is_relative_to(staging):
            raise ValueError("onnx_file must place generated tensors inside the staging directory")
        staged_config.parent.mkdir()
        staged_config.write_bytes(config_bytes)
        generated = staging / "generated"
        environment = dict(os.environ, PYTHONBREAKPOINT="0")
        subprocess.run(
            [str(args.python.absolute()), str(root / "layer_generate_ne16.py"), "PULP.GAP9_NE16",
             "--config_file", str(staged_config), "--app_dir", str(generated),
             "--optional", "8bit"],
            cwd=staging, env=environment, check=True, timeout=args.timeout)
        # DORY names these .hex, but they contain raw packed bytes, not text.
        return importer.import_layer(SimpleNamespace(
            dory_commit=revision, config=staged_config,
            config_name=args.config_name, config_sha256=sha256(config_bytes),
            hardware_description=root / hardware, generated_dir=generated,
            input=generated / "hex" / "inputs.hex",
            weights=generated / "hex" / "BNReluConvolution0_weights.hex",
            expected=tensor_directory / "out_layer0.txt",
            expected_format="text", output_dir=output))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dory-root", required=True, type=Path)
    parser.add_argument("--python", required=True, type=Path,
                        help="Python interpreter from the existing DORY environment")
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--config-name", required=True,
                        help="Stable repository-relative configuration name for provenance")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        metadata = generate(args)
    except (ValueError, OSError, KeyError, UnicodeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(f"Generated {metadata['upstream']['jobs']} upstream tiles; imported "
          f"{metadata['cooperative_jobs']} cooperative jobs")


if __name__ == "__main__":
    main()
