"""Shared byte-preserving DORY fixture serialization."""

import hashlib


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
