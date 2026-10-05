#!/usr/bin/env python3
"""Validate genuine DORY chain provenance/imports and host transport (NOT MAC/RTL)."""

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
from unittest.mock import patch

from dory_ne16_host_test import build_binding
from dory_ne16_import import sha256, validate_dory_checkout


def extract(source: str, namespace: str, name: str) -> bytes:
    match = re.search(
        rf"const uint8_t {re.escape(namespace)}_{name}\[(\d+)\] = \{{(.*?)\}};",
        source, re.S)
    assert match is not None, f"missing {namespace}_{name}"
    result = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", match[2]))
    assert len(result) == int(match[1]), f"incorrect {namespace}_{name} length"
    return result


def check_coverage(fixture: Path, metadata: dict) -> None:
    plan_bytes = (fixture / "cooperative_plan.json").read_bytes()
    assert sha256(plan_bytes) == metadata["plan_sha256"]
    plan = json.loads(plan_bytes)
    coverage = bytearray(48 * 48 * 16)
    assert len(plan) == metadata["cooperative_jobs"] == 16
    seen = set()
    for recipe in plan:
        y, x, channel = (recipe[key] for key in ("output_y", "output_x", "output_channel"))
        assert channel == 0 and y in (0, 12, 24, 36) and x in (0, 12, 24, 36)
        assert (y, x) not in seen
        seen.add((y, x))
        assert recipe["output_height"] == recipe["output_width"] == 12
        iy, ix = max(0, y - 1), max(0, x - 1)
        ih, iw = min(48, y + 13) - iy, min(48, x + 13) - ix
        assert (recipe["input_y"], recipe["input_x"]) == (iy, ix)
        assert (recipe["input_height"], recipe["input_width"]) == (ih, iw)
        assert recipe["padding"] == dict(top=int(y == 0), bottom=int(y == 36),
                                         left=int(x == 0), right=int(x == 36))
        assert len(recipe["loads"]) == 4
        load = recipe["loads"][0]
        assert load == dict(src=(iy * 48 + ix) * 16, dst=2048, line_bytes=iw * 16,
                            rows=ih, planes=1, src_row_stride=768, dst_row_stride=iw * 16)
        for row in range(12):
            for column in range(12):
                for component in range(16):
                    offset = ((y + row) * 48 + x + column) * 16 + component
                    assert coverage[offset] == 0
                    coverage[offset] = 1
    assert all(coverage)


def check_import(root: Path, work: Path, timeout: int) -> None:
    scripts = root / "generators" / "ne16" / "scripts"
    specification = importlib.util.spec_from_file_location(
        "chain_cooperative_import", scripts / "import-dory-ne16-cooperative-layer.py")
    importer = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(importer)
    fixture = root / "tests" / "ne16-dory-generated-chain"
    manifest = json.loads((fixture / "generated_chain_metadata.json").read_bytes())
    assert manifest["schema_version"] == 1 and manifest["dory_commit"] == importer.DORY_COMMIT
    assert len(manifest["layers"]) == 2
    assert manifest["dependency"] == (
        "layer0 stores complete before layer1 loads from the same DRAM tensor")
    assert manifest["frontend_script_sha256"] == sha256(
        (scripts / "dory-ne16-chain-frontend.py").read_bytes().replace(b"\r\n", b"\n"))
    frontend = manifest["reference_frontend"]
    assert frontend["seed"] == manifest["seed"] == 0
    assert frontend["frontend"] == "layer_generate_ne16.create_layer"
    assert frontend["link"] == "layer0.output -> layer1.input"
    assert len(frontend["layers"]) == 2
    tensor_sets, arguments = [], []
    for index, shift in enumerate((11, 9)):
        namespace = f"dory_chain_l{index}"
        layer = fixture / f"layer{index}"
        metadata_bytes = (layer / "generated_layer_metadata.json").read_bytes()
        metadata = json.loads(metadata_bytes)
        record = manifest["layers"][index]
        reference = frontend["layers"][index]
        assert record["namespace"] == metadata["namespace"] == namespace
        assert record["metadata_sha256"] == sha256(metadata_bytes)
        assert metadata["dory_commit"] == manifest["dory_commit"]
        assert record["config"] == metadata["config"]
        assert record["config_sha256"] == metadata["config_sha256"]
        assert metadata["config_hash_normalization"] == "LF"
        config_path = root / metadata["config"]
        config_bytes = config_path.read_bytes().replace(b"\r\n", b"\n")
        assert sha256(config_bytes) == metadata["config_sha256"]
        assert importer.validate_config(json.loads(config_bytes)) == (48, 48, 16)
        assert metadata["dimensions"] == dict(
            height=48, width=48, input_channels=16, output_channels=16)
        assert record["quantization_shift"] == metadata["quantization_shift"] == shift
        assert metadata["upstream"]["quantization_shift"] == reference["quantization_shift"] == shift
        assert record["upstream_jobs"] == metadata["upstream"]["jobs"] == 4
        assert metadata["upstream"]["grid"] == dict(height=2, width=2, output_channel=1)
        assert record["cooperative_jobs"] == metadata["cooperative_jobs"] == 16
        assert reference["index"] == index and reference["reference"].startswith(f"layer{index}/")
        for relative, digest in metadata["source_sha256"].items():
            assert sha256((layer / "upstream" / relative).read_bytes()) == digest, relative
        source = (layer / (namespace + "_layer_data.c")).read_text()
        tensors = {name: extract(source, namespace, name)
                   for name in ("input", "weights", "expected")}
        assert len(tensors["input"]) == len(tensors["expected"]) == 48 * 48 * 16
        assert len(tensors["weights"]) == 16 * 152
        for name, data in tensors.items():
            assert sha256(data) == metadata["tensor_sha256"][name]
        assert record["input_sha256"] == reference["input_sha256"] == sha256(tensors["input"])
        assert record["output_sha256"] == reference["output_sha256"] == sha256(tensors["expected"])
        check_coverage(layer, metadata)
        paths = {}
        for name, data in tensors.items():
            paths[name] = work / f"layer{index}-{name}.bin"
            paths[name].write_bytes(data)
        candidate = SimpleNamespace(
            generated_dir=layer / "upstream", config=config_path,
            hardware_description=layer / "upstream" / "HW_description.json",
            config_name=metadata["config"], config_sha256=metadata["config_sha256"],
            dory_commit=metadata["dory_commit"], expected_format="binary",
            namespace=namespace, use_generated_shift=True, **paths)
        files = {path.relative_to(layer) for path in layer.rglob("*") if path.is_file()}
        for repetition in range(2):
            candidate.output_dir = work / f"reproduce-{index}-{repetition}"
            importer.import_layer(candidate)
            assert files == {path.relative_to(candidate.output_dir)
                             for path in candidate.output_dir.rglob("*") if path.is_file()}
            for relative in files:
                assert (candidate.output_dir / relative).read_bytes() == (
                    layer / relative).read_bytes(), relative
        arguments.append(copy.copy(candidate))
        tensor_sets.append(tensors)
    assert tensor_sets[0]["expected"] == tensor_sets[1]["input"]
    assert manifest["intermediate_sha256"] == sha256(tensor_sets[0]["expected"])
    assert tensor_sets[0]["weights"] != tensor_sets[1]["weights"]
    assert tensor_sets[0]["expected"] != tensor_sets[1]["expected"]

    base = arguments[1]
    rejected = 0

    def reject(candidate):
        nonlocal rejected
        candidate.output_dir = work / f"rejected-{rejected}"
        try:
            importer.import_layer(candidate)
        except (ValueError, OSError, UnicodeError):
            assert not candidate.output_dir.exists(), "rejection left partial artifacts"
            rejected += 1
        else:
            raise AssertionError("unsupported chain import was accepted")

    original = (base.generated_dir / "src" / "BNReluConvolution0.c").read_text()

    def altered_source(old, new):
        generated = work / f"source-{len(list(work.glob('source-*')))}"
        shutil.copytree(base.generated_dir, generated)
        assert original.count(old) >= 1, old
        (generated / "src" / "BNReluConvolution0.c").write_text(original.replace(old, new, 1))
        candidate = copy.copy(base)
        candidate.generated_dir = generated
        return candidate

    for shift in (9, 0, 31):
        candidate = altered_source(".shift_amount = 9", f".shift_amount = {shift}")
        candidate.namespace = f"dory_positive_s{shift}"
        candidate.output_dir = work / f"positive-{shift}"
        metadata = importer.import_layer(candidate)
        assert metadata["quantization_shift"] == metadata["upstream"]["quantization_shift"] == shift
        header = (candidate.output_dir / (candidate.namespace + "_layer.h")).read_text()
        assert f"#define {candidate.namespace.upper()}_QUANT_SHIFT {shift}u" in header
        binding = (candidate.output_dir / (candidate.namespace + "_layer.c")).read_text()
        assert f".shift_amount = {shift}," in binding
        assert metadata["source_sha256"]["src/BNReluConvolution0.c"] == sha256(
            (candidate.generated_dir / "src" / "BNReluConvolution0.c").read_bytes())

    for shift in ("32", "-1", "1 + 8", "SHIFT", "9.0", "0x9", "09", "true"):
        reject(altered_source(".shift_amount = 9", f".shift_amount = {shift}"))
    for shift in (0, 31):
        candidate = altered_source(".shift_amount = 9", f".shift_amount = {shift}")
        candidate.use_generated_shift = False
        del candidate.namespace
        reject(candidate)
    candidate = copy.copy(base)
    candidate.use_generated_shift = False
    reject(candidate)  # A nondefault namespace alone must not enable shifts.
    del candidate.namespace
    reject(candidate)  # Legacy default rejects the second layer's shift 9.
    candidate.use_generated_shift = True
    reject(candidate)  # Opt-in with implicit default namespace.
    candidate.namespace = "dory_generated"
    reject(candidate)  # Opt-in with explicit default namespace.
    candidate = copy.copy(arguments[0])
    candidate.namespace = "dory_generated"
    reject(candidate)  # Even shift 11 must not opt in under the legacy namespace.
    del candidate.namespace
    reject(candidate)
    for namespace in ("", "Upper", "_leading", "3leading", "has-dash", "has space",
                      "has/slash", "has\\slash", "nonascii_é", "unicode_١",
                      "x" * 49, None, 7):
        candidate = copy.copy(base)
        candidate.namespace = namespace
        reject(candidate)
    for old, new in (
            (".groups = 1", ".groups = 16"),
            (".mode = quantMode8Bit", ".mode = quantMode16Bit"),
            (".function = quantFunctionRelu", ".function = quantFunctionIdentity"),
            (".flag_rounding = NE16_FLAG_UNUSED", ".flag_rounding = NE16_FLAG_USED"),
            (".mode  = normMode32Bit", ".mode  = normMode8Bit"),
            (".flag_bias  = NE16_FLAG_USED", ".flag_bias  = NE16_FLAG_UNUSED"),
            (".flag_shift = NE16_FLAG_UNUSED", ".flag_shift = NE16_FLAG_USED"),
            ("weightOffsetModeLayerWise", "weightOffsetModeChannelWise"),
            ("3, 0,", "3, 1,"),
            ("conf->stride_2d = 768", "conf->stride_2d = 769"),
            (".height = 24,", ".height = 23,")):
        reject(altered_source(old, new))
    config = json.loads(base.config.read_bytes())
    for field, value in (("group", 16), ("stride", [2, 2]), ("input_channels", 32),
                         ("weight_bits", 4), ("input_bits", 16), ("output_bits", 16),
                         ("output_type", "int"), ("kernel_shape", [1, 1]),
                         ("batchnorm", False), ("branch_in", 1),
                         ("input_dimensions", [True, 48])):
        path = work / f"invalid-config-{rejected}.json"
        path.write_text(json.dumps(dict(config, **{field: value})))
        candidate = copy.copy(base)
        candidate.config, candidate.config_sha256 = path, sha256(path.read_bytes())
        reject(candidate)
    for field in ("input", "weights", "expected"):
        path = work / f"truncated-{field}"
        path.write_bytes(getattr(base, field).read_bytes()[:-1])
        candidate = copy.copy(base)
        setattr(candidate, field, path)
        reject(candidate)
    for field, value in (("dory_commit", "0" * 40), ("config_sha256", "0" * 64)):
        candidate = copy.copy(base)
        setattr(candidate, field, value)
        reject(candidate)

    command = [sys.executable, str(scripts / "import-dory-ne16-cooperative-layer.py")]
    for field in ("generated_dir", "config", "hardware_description", "config_name",
                  "config_sha256", "dory_commit", "input", "weights", "expected",
                  "expected_format"):
        command += ["--" + field.replace("_", "-"), str(getattr(base, field))]
    cli_output = work / "cli-positive"
    result = subprocess.run(command + [
        "--namespace", "dory_cli", "--use-generated-shift", "--output-dir", str(cli_output)],
        capture_output=True, text=True, timeout=timeout)
    assert result.returncode == 0, result.stderr
    assert json.loads((cli_output / "generated_layer_metadata.json").read_bytes())[
        "quantization_shift"] == 9
    for flags in ([], ["--use-generated-shift"],
                  ["--use-generated-shift", "--namespace", "Bad"],
                  ["--namespace", "dory_cli"]):
        output = work / f"cli-rejected-{rejected}"
        result = subprocess.run(command + flags + ["--output-dir", str(output)],
                                capture_output=True, text=True, timeout=timeout)
        assert result.returncode == 2 and "error:" in result.stderr
        assert "Traceback" not in result.stderr and not output.exists()
        rejected += 1
    occupied = work / "occupied"
    occupied.mkdir()
    (occupied / "keep").write_text("untouched")
    candidate = copy.copy(base)
    candidate.output_dir = occupied
    try:
        importer.import_layer(candidate)
    except ValueError:
        assert list(occupied.iterdir()) == [occupied / "keep"]
        assert (occupied / "keep").read_text() == "untouched"
        rejected += 1
    else:
        raise AssertionError("occupied output directory overwritten")
    print(f"DORY genuine chain reproducibility/linkage/provenance/coverage PASS; "
          f"positive shifts 9/0/31; {rejected} rejection cases")


def tree_bytes(directory: Path) -> dict:
    return {path.relative_to(directory): path.read_bytes()
            for path in directory.rglob("*") if path.is_file()}


def check_checkout(work: Path, commit: str) -> None:
    assert commit == "add0d9c1be889b5f802b2606ced8c59acff8aa02"
    checkout = work / "checkout"
    relative = Path("dory") / "Hardware_targets" / "PULP" / "GAP9_NE16" / "HW_description.json"
    allowed = relative.as_posix()
    cases = [
        (commit, "", None),
        (commit, allowed + "\n", None),
        ("0" * 40, "", ValueError),
        (commit, "layer_generate_ne16.py\n", ValueError),
        (commit, allowed + "\nlayer_generate_ne16.py\n", ValueError),
        (subprocess.CalledProcessError(1, ["git"]), "", subprocess.CalledProcessError),
        (subprocess.TimeoutExpired(["git"], 30), "", subprocess.TimeoutExpired),
    ]
    for revision, changed, expected_error in cases:
        commands = []

        def git_run(command, **kwargs):
            commands.append(command)
            assert kwargs == dict(check=True, capture_output=True, text=True, timeout=30)
            assert command[:3] == ["git", "-C", str(checkout)]
            if len(commands) == 1:
                assert command[3:] == ["rev-parse", "HEAD"]
                if isinstance(revision, Exception):
                    raise revision
                return subprocess.CompletedProcess(command, 0, revision + "\n", "")
            assert len(commands) == 2 and command[3:] == ["diff", "HEAD", "--name-only"]
            return subprocess.CompletedProcess(command, 0, changed, "")

        with patch("dory_ne16_import.subprocess.run", side_effect=git_run):
            try:
                result = validate_dory_checkout(checkout, commit)
            except (ValueError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                assert expected_error is not None and isinstance(error, expected_error)
            else:
                assert expected_error is None and result == checkout / relative
        assert len(commands) == (2 if revision == commit else 1)
    print("DORY pinned checkout validation PASS; 2 allowed/5 rejected cases (mocked git)")


def check_workflow(root: Path, work: Path, timeout: int) -> None:
    """Exercise the real coordinator/importer, with fixture-backed frontend output."""
    scripts = root / "generators" / "ne16" / "scripts"
    specification = importlib.util.spec_from_file_location(
        "chain_coordinator", scripts / "generate-dory-ne16-cooperative-chain.py")
    coordinator = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(coordinator)
    fixture = root / "tests" / "ne16-dory-generated-chain"
    manifest = json.loads((fixture / "generated_chain_metadata.json").read_bytes())
    work = work / "workflow"
    work.mkdir()
    check_checkout(work, manifest["dory_commit"])
    checkout = work / "checkout"
    hardware = checkout / "dory" / "Hardware_targets" / "PULP" / "GAP9_NE16" / "HW_description.json"
    hardware.parent.mkdir(parents=True)
    hardware.write_bytes((fixture / "layer0" / "upstream" / "HW_description.json").read_bytes())
    tensors = [
        {name: extract((fixture / f"layer{index}" / f"dory_chain_l{index}_layer_data.c").read_text(),
                       f"dory_chain_l{index}", name) for name in ("input", "weights", "expected")}
        for index in range(2)]
    base = SimpleNamespace(
        dory_root=checkout, python=Path(sys.executable), namespace_prefix="dory_chain",
        config=[root / record["config"] for record in manifest["layers"]],
        config_name=[record["config"] for record in manifest["layers"]],
        seed=manifest["seed"], timeout=timeout)
    rejected = 0

    def run_case(name, *, changes=None, failure=None, expected_error=None, message="",
                 staged_layers=None, occupied=False, frontend_calls=1, checkout_calls=1):
        nonlocal rejected
        case = work / name
        case.mkdir()
        args = copy.copy(base)
        args.output_dir = case / "published"
        for field, value in (changes or {}).items():
            setattr(args, field, value)
        if occupied:
            args.output_dir.mkdir()
            (args.output_dir / "keep").write_bytes(b"untouched existing output\n")
            original_output = tree_bytes(args.output_dir)

        def frontend_run(command, **kwargs):
            raw = Path(command[command.index("--output-dir") + 1])
            configs = [Path(command[index + 1]) for index, value in enumerate(command)
                       if value == "--config"]
            assert command[:2] == [str(args.python.absolute()),
                                    str(scripts / "dory-ne16-chain-frontend.py")]
            assert command[command.index("--dory-root") + 1] == str(checkout.resolve())
            assert command[command.index("--seed") + 1] == str(args.seed)
            assert kwargs["cwd"] == raw.parent and raw.parent.is_relative_to(case)
            assert kwargs["check"] is True and kwargs["timeout"] == args.timeout
            assert kwargs["env"]["PYTHONBREAKPOINT"] == "0"
            assert len(configs) == 2
            for generated_config, original_config in zip(configs, args.config):
                assert generated_config.read_bytes() == original_config.read_bytes().replace(b"\r\n", b"\n")
            if failure in ("timeout", "frontend-exit"):
                raw.mkdir()
                (raw / "partial").write_bytes(b"unfinished frontend")
                if failure == "timeout":
                    raise subprocess.TimeoutExpired(command, args.timeout)
                raise subprocess.CalledProcessError(1, command)
            frontend = copy.deepcopy(manifest["reference_frontend"])
            for index in range(2):
                bundle = raw / f"layer{index}"
                generated = bundle / "generated"
                shutil.copytree(fixture / f"layer{index}" / "upstream", generated)
                hex_directory = generated / "hex"
                hex_directory.mkdir()
                incoming = tensors[index]["input"]
                if index == 1 and failure == "link":
                    incoming = bytes([incoming[0] ^ 1]) + incoming[1:]
                (hex_directory / "inputs.hex").write_bytes(incoming)
                (hex_directory / "BNReluConvolution0_weights.hex").write_bytes(tensors[index]["weights"])
                (bundle / "expected.bin").write_bytes(tensors[index]["expected"])
                decimal = tensors[index]["expected"]
                if index == 1 and failure == "oracle":
                    decimal = bytes([decimal[0] ^ 1]) + decimal[1:]
                oracle = raw / frontend["layers"][index]["reference"]
                oracle.parent.mkdir(parents=True)
                oracle.write_bytes((",\n".join(str(value) for value in decimal) + "\n").encode("ascii"))
                if index == 1 and failure in ("invalid-source", "invalid-task"):
                    source = generated / "src" / "BNReluConvolution0.c"
                    old, new = ((".groups = 1", ".groups = 16") if failure == "invalid-source"
                                else (".shift_amount = 9", ".shift_amount = 32"))
                    data = source.read_bytes()
                    assert data.count(old.encode()) == 1
                    source.write_bytes(data.replace(old.encode(), new.encode()))
            if failure == "shift":
                frontend["layers"][1]["quantization_shift"] = 10
            (raw / "frontend_chain.json").write_bytes((json.dumps(frontend, indent=2) + "\n").encode())
            return subprocess.CompletedProcess(command, 0)

        def validate_checkout(actual_root, commit):
            assert actual_root == checkout.resolve() and commit == manifest["dory_commit"]
            return hardware

        # Configure real TemporaryDirectory allocations to stay inside this
        # test's project-local scratch; do not mock staging or the importer.
        previous_tempdir = tempfile.tempdir
        tempfile.tempdir = str(case)
        try:
            with patch.object(coordinator, "validate_dory_checkout",
                              side_effect=validate_checkout) as checkout_mock, \
                    patch.object(coordinator.subprocess, "run",
                                 side_effect=frontend_run) as frontend_mock:
                try:
                    result = coordinator.generate(args)
                except (ValueError, OSError, subprocess.CalledProcessError,
                        subprocess.TimeoutExpired) as error:
                    assert expected_error is not None and isinstance(error, expected_error), name
                    assert message in str(error), (name, str(error))
                    if staged_layers is not None:
                        trace = error.__traceback__
                        while trace and trace.tb_frame.f_code is not coordinator.generate.__code__:
                            trace = trace.tb_next
                        assert trace is not None
                        records = trace.tb_frame.f_locals.get("records", [])
                        assert len(records) == staged_layers, (name, records)
                        if staged_layers:
                            assert records[0] == manifest["layers"][0], name
                        staging = trace.tb_frame.f_locals.get("staging")
                        assert staging is None or not staging.exists(), name
                    rejected += 1
                else:
                    assert expected_error is None, f"{name} unexpectedly succeeded"
                    assert result == json.loads(
                        (args.output_dir / "generated_chain_metadata.json").read_bytes())
                assert checkout_mock.call_count == checkout_calls, name
                assert frontend_mock.call_count == frontend_calls, name
        finally:
            tempfile.tempdir = previous_tempdir
        if occupied:
            assert tree_bytes(args.output_dir) == original_output
            assert list(case.iterdir()) == [args.output_dir]
        elif expected_error is not None:
            assert not args.output_dir.exists() and not list(case.iterdir()), name
        else:
            assert list(case.iterdir()) == [args.output_dir], "staging artifacts leaked"
        return args.output_dir

    output = run_case("reproduce")
    assert tree_bytes(output) == tree_bytes(fixture), "mocked coordinator changed fixture bytes"
    custom = run_case("custom-prefix", changes={"namespace_prefix": "custom_chain"})
    for index, shift in enumerate((11, 9)):
        namespace = f"custom_chain_l{index}"
        layer = custom / f"layer{index}"
        metadata = json.loads((layer / "generated_layer_metadata.json").read_bytes())
        assert metadata["namespace"] == namespace and metadata["quantization_shift"] == shift
        assert (layer / (namespace + "_layer.c")).is_file()
        assert not list(layer.glob("dory_chain*"))
    for failure, message, staged in (
            ("link", "generated second input differs", 0),
            ("oracle", "binary and original decimal reference disagree", 1),
            ("shift", "NNX shift differs from the reference frontend", 1),
            ("invalid-source", "unsupported generated kernel", 1),
            ("invalid-task", "unsupported generated quantization shift", 1)):
        run_case(failure, failure=failure, expected_error=ValueError,
                 message=message, staged_layers=staged)
    run_case("timeout", failure="timeout", expected_error=subprocess.TimeoutExpired)
    run_case("frontend-exit", failure="frontend-exit", expected_error=subprocess.CalledProcessError)
    run_case("occupied", occupied=True, expected_error=ValueError,
             message="output directory already exists", frontend_calls=0)
    config = json.loads(base.config[0].read_bytes())
    for name, index, change in (("mismatched-shape", 1, {"input_dimensions": [48, 36]}),
                                ("mismatched-channels", 0, {"output_channels": 32})):
        path = work / (name + ".json")
        path.write_bytes(json.dumps(dict(config, **change)).encode())
        configs = list(base.config)
        configs[index] = path
        run_case(name, changes={"config": configs}, expected_error=ValueError,
                 message="first output must match", frontend_calls=0, checkout_calls=0)
    preflight_cases = [
        ("one-config", {"config": base.config[:1]}, "exactly two configurations"),
        ("one-config-name", {"config_name": base.config_name[:1]}, "exactly two configurations"),
        ("negative-seed", {"seed": -1}, "uint32 seed"),
        ("large-seed", {"seed": 0x100000000}, "uint32 seed"),
        ("invalid-timeout", {"timeout": 0}, "positive timeout"),
    ]
    preflight_cases += [(f"invalid-prefix-{index}", {"namespace_prefix": prefix}, "namespace")
                        for index, prefix in enumerate(("", "Upper", "has-dash", "x" * 46))]
    for name, changes, message in preflight_cases:
        run_case(name, changes=changes, expected_error=ValueError, message=message,
                 frontend_calls=0, checkout_calls=0)
    print(f"DORY coordinator workflow/atomicity PASS; byte-identical fixture, custom prefix, "
          f"{rejected} rejection cases (MOCKED frontend generation; NOT MAC/frontend proof)")


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
        with tempfile.TemporaryDirectory(prefix=".dory-chain-host-", dir=root) as temporary:
            directory = Path(temporary)
            check_import(root, directory, args.timeout)
            check_workflow(root, directory, args.timeout)
            fixture = root / "tests" / "ne16-dory-generated-chain"
            build_binding(root, directory, compiler, args.timeout,
                          [(fixture / f"layer{index}", f"dory_chain_l{index}")
                           for index in range(2)], "dory-ne16-generated-chain-host.c")
    except (ValueError, OSError, KeyError, UnicodeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired, AssertionError) as error:
        print(f"DORY generated-chain host validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
