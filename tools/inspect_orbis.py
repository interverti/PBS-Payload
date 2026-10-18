#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect a privately dumped Orbis ELF without redistributing console binaries."""
import argparse
import base64
import hashlib
import json
import struct
from pathlib import Path


def nid(name):
    digest = hashlib.sha1(name.encode() + bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')).digest()
    return base64.b64encode(digest[:8][::-1]).decode().rstrip('=').replace('/', '-')


class OrbisElf:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if self.data[:6] != b'\x7fELF\x02\x01':
            raise ValueError('Expected little-endian ELF64')
        phoff = struct.unpack_from('<Q', self.data, 32)[0]
        stride, count = struct.unpack_from('<HH', self.data, 54)
        self.ph = [struct.unpack_from('<IIQQQQQQ', self.data, phoff + i * stride) for i in range(count)]
        self.blob = next(p[2] for p in self.ph if p[0] == 0x61000000)
        dynamic = next(p for p in self.ph if p[0] == 2)
        self.tags = dict(struct.iter_unpack('<QQ', self.data[dynamic[2]:dynamic[2] + dynamic[5]]))
        self.strtab = self.blob + self.tags[0x61000035]
        start = self.blob + self.tags[0x61000039]
        size = self.tags[0x6100003f]
        self.symbols = []
        for pos in range(start, start + size, 24):
            name, info, other, section, value, length = struct.unpack_from('<IBBHQQ', self.data, pos)
            name = self.data[self.strtab + name:].split(b'\0', 1)[0].decode()
            self.symbols.append(dict(nid=name, value=value, size=length, defined=section != 0, type=info & 15))

    def code(self, address, size):
        for p in self.ph:
            if p[0] == 1 and p[3] <= address < p[3] + p[5]:
                off = p[2] + address - p[3]
                return self.data[off:off + size]
        raise ValueError(f'Address outside LOAD segments: {address:x}')

    def import_stubs(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        relocations = {}
        for offset_tag, size_tag in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
            start = self.blob + self.tags[offset_tag]
            for offset, info, addend in struct.iter_unpack('<QQq', self.data[start:start + self.tags[size_tag]]):
                index = info >> 32
                if index < len(self.symbols):
                    relocations[offset] = self.symbols[index]['nid'].split('#')[0]
        result = {}
        for p in self.ph:
            if p[0] != 1 or not p[1] & 1:
                continue
            cs = Cs(CS_ARCH_X86, CS_MODE_64)
            cs.skipdata = True
            for ins in cs.disasm(self.data[p[2]:p[2] + p[5]], p[3]):
                if ins.bytes[:2] == b'\xff\x25' and len(ins.bytes) == 6:
                    target = ins.address + ins.size + struct.unpack('<i', ins.bytes[2:])[0]
                    if target in relocations:
                        result[ins.address] = relocations[target]
        return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('elf', type=Path)
    ap.add_argument('--names', type=Path)
    ap.add_argument('--disassemble', type=lambda s: int(s, 0))
    ap.add_argument('--size', type=lambda s: int(s, 0), default=256)
    ap.add_argument('--calls', help='Show call sites for this imported function')
    args = ap.parse_args()
    elf = OrbisElf(args.elf)
    if args.calls:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        targets = {va for va, name in elf.import_stubs().items() if name == nid(args.calls)}
        for p in elf.ph:
            if p[0] != 1 or not p[1] & 1:
                continue
            cs = Cs(CS_ARCH_X86, CS_MODE_64)
            cs.skipdata = True
            instructions = list(cs.disasm(elf.data[p[2]:p[2] + p[5]], p[3]))
            for i, ins in enumerate(instructions):
                if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x') and int(ins.op_str, 16) in targets:
                    print(f'--- {args.calls} at {ins.address:x} ---')
                    for item in instructions[max(0, i - 24):i + 8]:
                        print(f'{item.address:08x}: {item.mnemonic:9} {item.op_str}')
    elif args.disassemble is not None:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        for ins in Cs(CS_ARCH_X86, CS_MODE_64).disasm(elf.code(args.disassemble, args.size), args.disassemble):
            print(f'{ins.address:08x}: {ins.mnemonic:9} {ins.op_str}')
    else:
        names = {}
        if args.names:
            names = {nid(n): n for n in args.names.read_text().splitlines() if n}
        for sym in elf.symbols:
            sym['name'] = names.get(sym['nid'].split('#')[0], '')
        print(json.dumps(elf.symbols, indent=2))


if __name__ == '__main__':
    main()
