#!/usr/bin/env python3
"""
静态补丁 libil2cpp.so（arm64）工具。

默认偏移针对当前已验证版本：
orig sha256 = 7a9fdc8f621c39246c77cd02d8f291f2f26c6d3cee0da9b1820f67e1d954922b
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


PATCHES = {
    0x172F68C: bytes.fromhex("20008052c0035fd6"),  # get_IsGameControlPassed -> true
    0x17351B4: bytes.fromhex("00008052c0035fd6"),  # VersionCompare -> 0
    0x1736F80: bytes.fromhex("c0035fd61f2003d5"),  # ConfirmVersionForceUpdateJumpCallback -> void
    0x1739C9C: bytes.fromhex("00008052c0035fd6"),  # IsVersionLessThanTargetVersion -> false
    0x1599528: bytes.fromhex("c0035fd61f2003d5"),  # ShowNativeQuitDialog -> void
    0x159AB40: bytes.fromhex("c0035fd61f2003d5"),  # VersionForceUpdateJump -> void
    0x159ABE0: bytes.fromhex("c0035fd61f2003d5"),  # OpenNativeBrowser -> void
}


def sha256_bytes(data: bytes) -> str:
    h = hashlib.sha256()
    h.update(data)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="原始 libil2cpp.so 路径")
    parser.add_argument("--output", required=True, help="输出补丁 so 路径")
    args = parser.parse_args()

    in_path = Path(args.input)
    out_path = Path(args.output)
    raw = bytearray(in_path.read_bytes())
    orig_hash = sha256_bytes(raw)

    for off, code in PATCHES.items():
        if off + len(code) > len(raw):
            raise RuntimeError(f"offset out of range: 0x{off:x}")
        raw[off : off + len(code)] = code

    if raw[:4] != b"\x7fELF":
        raise RuntimeError("ELF magic invalid after patch")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(raw)
    patched_hash = sha256_bytes(raw)

    print(f"input_sha256={orig_hash}")
    print(f"output_sha256={patched_hash}")
    print(f"patch_count={len(PATCHES)}")
    print(f"output={out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
