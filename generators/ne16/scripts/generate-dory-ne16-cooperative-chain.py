#!/usr/bin/env python3
"""Generate two DORY convolutions with linked references and namespaced bindings."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from dory_ne16_import import parse_expected, sha256, validate_dory_checkout


def generate(args) -> dict:
    scripts = Path(__file__).resolve().parent
    specification = importlib.util.spec_from_file_location(
        "cooperative_import", scripts / "import-dory-ne16-cooperative-layer.py")
    importer = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(importer)
    if len(args.config) != 2 or len(args.config_name) != 2:
        raise ValueError("exactly two configurations and configuration names are required")
    if not 0 <= args.seed <= 0xffffffff or args.timeout <= 0:
        raise ValueError("a uint32 seed and positive timeout are required")
    namespaces = [args.namespace_prefix + f"_l{index}" for index in range(2)]
    for namespace in namespaces:
        importer.validate_namespace(namespace)
    config_bytes = [path.read_bytes().replace(b"\r\n", b"\n") for path in args.config]
    dimensions = [importer.validate_config(json.loads(data)) for data in config_bytes]
    if dimensions[0][:2] != dimensions[1][:2] or dimensions[0][2] != 16:
        raise ValueError("first output must match the second 16-channel HWC input")
    root = args.dory_root.resolve()
    hardware = validate_dory_checkout(root, importer.DORY_COMMIT)
    output = args.output_dir.resolve()
    if output.exists():
        raise ValueError("output directory already exists; generate into a fresh directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dory-chain-generation-") as temporary:
        work = Path(temporary)
        configs = [work / f"config{index}.json" for index in range(2)]
        for path, data in zip(configs, config_bytes):
            path.write_bytes(data)
        raw = work / "frontend"
        command = [
            str(args.python.absolute()), str(scripts / "dory-ne16-chain-frontend.py"),
            "--dory-root", str(root), "--output-dir", str(raw), "--seed", str(args.seed)]
        for path in configs:
            command += ["--config", str(path)]
        subprocess.run(command, cwd=work, env=dict(os.environ, PYTHONBREAKPOINT="0"),
                       check=True, timeout=args.timeout)
        frontend = json.loads((raw / "frontend_chain.json").read_text())
        intermediate = (raw / "layer0" / "expected.bin").read_bytes()
        incoming = (raw / "layer1" / "generated" / "hex" / "inputs.hex").read_bytes()
        if intermediate != incoming:
            raise ValueError("generated second input differs from first reference output")
        with tempfile.TemporaryDirectory(prefix=".dory-chain-import-", dir=output.parent) as staged:
            staging = Path(staged)
            records = []
            for index, namespace in enumerate(namespaces):
                bundle = raw / f"layer{index}"
                expected = bundle / "expected.bin"
                reference = raw / frontend["layers"][index]["reference"]
                if expected.read_bytes() != parse_expected(reference.read_bytes()):
                    raise ValueError("frontend binary and original decimal reference disagree")
                metadata = importer.import_layer(SimpleNamespace(
                    dory_commit=importer.DORY_COMMIT, config=configs[index],
                    config_name=args.config_name[index],
                    config_sha256=sha256(config_bytes[index]),
                    hardware_description=hardware, generated_dir=bundle / "generated",
                    input=bundle / "generated" / "hex" / "inputs.hex",
                    weights=bundle / "generated" / "hex" / "BNReluConvolution0_weights.hex",
                    expected=expected, expected_format="binary", namespace=namespace,
                    use_generated_shift=True,
                    output_dir=staging / f"layer{index}"))
                if metadata["quantization_shift"] != frontend["layers"][index]["quantization_shift"]:
                    raise ValueError("generated NNX shift differs from the reference frontend")
                records.append({
                    "namespace": namespace, "config": args.config_name[index],
                    "config_sha256": sha256(config_bytes[index]),
                    "metadata_sha256": sha256(
                        (staging / f"layer{index}" / "generated_layer_metadata.json").read_bytes()),
                    "input_sha256": metadata["tensor_sha256"]["input"],
                    "output_sha256": metadata["tensor_sha256"]["expected"],
                    "upstream_jobs": metadata["upstream"]["jobs"],
                    "cooperative_jobs": metadata["cooperative_jobs"],
                    "quantization_shift": metadata["quantization_shift"],
                })
            manifest = {
                "schema_version": 1, "dory_commit": importer.DORY_COMMIT,
                "seed": args.seed, "layers": records,
                "reference_frontend": frontend,
                "intermediate_sha256": sha256(intermediate),
                "dependency": "layer0 stores complete before layer1 loads from the same DRAM tensor",
                "frontend_script_sha256": sha256(
                    (scripts / "dory-ne16-chain-frontend.py").read_bytes().replace(b"\r\n", b"\n")),
            }
            (staging / "generated_chain_metadata.json").write_bytes(
                (json.dumps(manifest, indent=2) + "\n").encode())
            if output.exists():
                raise ValueError("output directory appeared during generation")
            staging.rename(output)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dory-root", required=True, type=Path)
    parser.add_argument("--python", required=True, type=Path)
    parser.add_argument("--config", required=True, action="append", type=Path)
    parser.add_argument("--config-name", required=True, action="append")
    parser.add_argument("--namespace-prefix", default="dory_chain")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args()
    try:
        manifest = generate(args)
    except (ValueError, OSError, KeyError, UnicodeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print("Imported genuine cooperative chain:", [record["cooperative_jobs"]
                                                  for record in manifest["layers"]])


if __name__ == "__main__":
    main()
