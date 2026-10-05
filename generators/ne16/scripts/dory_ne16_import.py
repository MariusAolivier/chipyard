"""Shared byte-preserving DORY fixture serialization."""

import hashlib
from pathlib import Path
import subprocess


def validate_dory_checkout(root: Path, commit: str) -> Path:
    revision = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
        capture_output=True, text=True, timeout=30).stdout.strip()
    if revision != commit:
        raise ValueError(f"DORY checkout must be pinned to {commit}, found {revision}")
    hardware = Path("dory/Hardware_targets/PULP/GAP9_NE16/HW_description.json")
    changed = subprocess.run(
        ["git", "-C", str(root), "diff", "HEAD", "--name-only"], check=True,
        capture_output=True, text=True, timeout=30).stdout.splitlines()
    if any(name != hardware.as_posix() for name in changed):
        raise ValueError("pinned DORY generator sources must be clean; only HW_description.json may differ")
    return root / hardware


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def c_array(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}[{len(data)}] = {{"]
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in data[offset:offset + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def parse_expected(data: bytes) -> bytes:
    values = [int(token) for token in data.decode("ascii").replace(",", " ").split()]
    if any(value < 0 or value > 255 for value in values):
        raise ValueError("expected output contains a non-uint8 value")
    return bytes(values)
