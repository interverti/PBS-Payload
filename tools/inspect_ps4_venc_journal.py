#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a PBS printf journal without executing captured format strings."""
import argparse
import json
from pathlib import Path
import struct

DATA_SIZE = 8 + 64 * 64


def decode(blob, elf=None):
    if len(blob) != 48 + DATA_SIZE or blob[:8] != b'PBSVJNL1':
        raise ValueError('Expected exact 4152-byte PBSVJNL1 journal')
    base, pid, epoch, original_printf, size = struct.unpack_from('<5Q', blob, 8)
    if size != DATA_SIZE or not base:
        raise ValueError('Invalid journal header')
    count = struct.unpack_from('<I', blob, 48)[0]
    if count == 0xffffffff:
        raise ValueError('Counter exhausted; ticket order cannot be trusted')

    def string_at(address):
        if elf is None or address < base:
            return None
        try:
            # Resolve only bytes within a single file-backed LOAD segment.
            rva = address - base
            for p in elf.ph:
                if p[0] == 1 and p[3] <= rva < p[3] + p[5]:
                    raw = elf.code(rva, min(256, p[3] + p[5] - rva))
                    if b'\0' not in raw:
                        return None
                    value = raw.split(b'\0', 1)[0].decode('ascii')
                    if value and all(c.isprintable() or c in '\r\n\t' for c in value):
                        return value
        except (ValueError, UnicodeDecodeError):
            pass
        return None

    records = []
    for index in range(64):
        ticket, *args, reserved = struct.unpack_from('<8Q', blob, 56 + index * 64)
        if not ticket or ticket > count or (ticket - 1) % 64 != index:
            continue  # Uncommitted, overwritten or malformed record.
        if count - ticket >= 64:
            continue
        records.append(dict(ticket=ticket, args=[hex(a) for a in args],
                            strings=[string_at(a) for a in args]))
    records.sort(key=lambda r: r['ticket'])
    return dict(module_base=hex(base), pid=pid, epoch=epoch,
                original_printf=hex(original_printf), total_calls=count,
                records=records,
                limitation='Six GP inputs only; no stack/SIMD arguments. '
                           'Incomplete records omitted; concurrent wrap may lose records.')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('journal', type=Path)
    ap.add_argument('--elf', type=Path, help='Matching privately dumped libSceVencCore ELF')
    args = ap.parse_args()
    elf = None
    if args.elf:
        from inspect_orbis import OrbisElf
        elf = OrbisElf(args.elf)
    print(json.dumps(decode(args.journal.read_bytes(), elf), indent=2))


if __name__ == '__main__':
    main()
