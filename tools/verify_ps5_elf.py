#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate payload ELF structure without mistaking OS/ABI for its target.

Official prospero-clang/LLD builds can use ELFOSABI_NONE (0); other compatible
toolchains emit ELFOSABI_FREEBSD (9). Never rewrite the branding byte.
This is an offline format check, not proof of runtime compatibility.
"""
import argparse
from pathlib import Path
import struct


def verify(data):
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("expected little-endian ELF64 version 1")
    if data[7] not in (0, 9):
        raise ValueError(f"unsupported ELF OS/ABI {data[7]} (expected System V 0 or FreeBSD 9)")
    kind, machine, version, entry, phoff, _, _, ehsize, phsize, phnum = struct.unpack_from("<HHIQQQIHHH", data, 16)
    if machine != 62 or kind not in (2, 3) or version != 1:
        raise ValueError("expected an x86-64 executable or shared payload")
    if ehsize != 64 or phsize != 56 or not phnum or phoff < 64 or phoff + phsize * phnum > len(data):
        raise ValueError("invalid program header table")
    loads, executable_entry = 0, False
    for i in range(phnum):
        ptype, flags, offset, va, _, size, memsize, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize)
        if offset + size > len(data):
            raise ValueError("program segment extends past the file")
        if ptype == 3:
            raise ValueError("host interpreter PT_INTERP is not valid for a console payload")
        if ptype == 1:
            if size > memsize or va + memsize > 0xffffffffffffffff:
                raise ValueError("invalid LOAD extent")
            loads += 1
            if flags & 1 and va <= entry < va + size:
                executable_entry = True
    if not loads or not executable_entry:
        raise ValueError("entry point is not in a file-backed executable LOAD")
    return data[7]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    try:
        abi = verify(args.elf.read_bytes())
    except (OSError, ValueError) as error:
        parser.exit(1, f"PS5 build rejected: {error}\n")
    print(f"PS5 ELF verified: x86-64, OS/ABI={abi}, valid executable LOADs")


if __name__ == "__main__":
    main()
