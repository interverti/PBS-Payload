#!/usr/bin/env python3
"""Generate PS5 Avcap2 request layouts from loaded module dumps.

Requires capstone (`python -m pip install capstone`). 7z/RAR input also requires
7-Zip. Disk .sprx SELF files are deliberately ignored.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import (X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RBX,
                         X86_REG_RBP, X86_REG_R15, X86_REG_RIP)

MODULE = "libSceAvcap2.sprx.prefix.bin"
EBOOT = "eboot.bin.prefix.bin"
FW_RE = re.compile(r"ps5-([0-9a-fA-F]{8})-")
PROLOGUE = b"\x55\x48\x89\xe5"
ERROR = (0x81950001).to_bytes(4, "little")
MAX_INPUT_FILES = 64
MAX_INPUT_BYTES = 64 * 1024 * 1024
MEMORY_LIMITS = {MODULE: 4 * 1024 * 1024, EBOOT: 8 * 1024 * 1024}


def memory_limit(name: str) -> int | None:
    for suffix, limit in MEMORY_LIMITS.items():
        if name.endswith(suffix):
            return limit
    return None


def check_members(members: list[tuple[str, int]]) -> None:
    if len(members) > MAX_INPUT_FILES:
        raise ValueError("too many decrypted memory images in archive")
    if len(set(name for name, _ in members)) != len(members):
        raise ValueError("duplicate memory image member path")
    if sum(length for _, length in members) > MAX_INPUT_BYTES:
        raise ValueError("archive memory dump budget exceeded")
    for name, length in members:
        if not 0 < length <= memory_limit(name):
            raise ValueError(f"memory dump has invalid length: {name} ({length})")



def disassembler():
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    return md


def load_members(archive: Path, seven_zip: str) -> dict[str, bytes]:
    """Read bounded decrypted memory prefixes; ignore disk SELF modules."""
    suffix = archive.suffix.lower()
    if suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            entries = [info for info in z.infolist() if not info.is_dir()
                       and memory_limit(info.filename) is not None]
            check_members([(info.filename, info.file_size) for info in entries])
            return {info.filename: z.read(info) for info in entries}
    if suffix == ".tar":
        with tarfile.open(archive) as t:
            entries = [m for m in t.getmembers() if m.isfile()
                       and memory_limit(m.name) is not None]
            check_members([(m.name, m.size) for m in entries])
            return {m.name: t.extractfile(m).read() for m in entries}
    if suffix not in (".rar", ".7z"):
        return {}
    listing = subprocess.run([seven_zip, "l", "-slt", str(archive)],
                             capture_output=True, text=True, check=True).stdout
    entries = []
    # A 7-Zip -slt record carries both its member path and declared size.
    # Preserve the full path so EBOOTs from different process IDs never collide.
    for block in re.split(r"\n\s*\n", listing):
        fields = dict(line.split(" = ", 1) for line in block.splitlines()
                      if " = " in line)
        name = fields.get("Path")
        if name and memory_limit(name) is not None:
            try:
                size = int(fields["Size"])
            except (KeyError, ValueError) as exc:
                raise ValueError(f"missing size for memory image {name}") from exc
            entries.append((name, size))
    check_members(entries)
    output = {}
    for name, limit in entries:
        data = subprocess.run([seven_zip, "x", "-so", str(archive), name],
                              capture_output=True, check=True).stdout
        if len(data) != limit:
            raise ValueError(f"decompressed size mismatch for {name}")
        output[name] = data
    return output


def mem_store(ins, base, size=None):
    if len(ins.operands) != 2 or ins.mnemonic != "mov":
        return None
    dst, src = ins.operands
    if dst.type != X86_OP_MEM or dst.mem.base != base:
        return None
    if size is not None and dst.size != size:
        return None
    return dst.mem.disp, src.imm if src.type == X86_OP_IMM else None


def avcap_layout(data: bytes) -> dict:
    if len(data) < 0x8000 or data[:4] == b"\x54\x14\xf5\xee":
        raise ValueError("no decrypted Avcap2 memory image")
    md = disassembler()
    entries = []
    for pos in range(0x100, 0x1000):
        if data[pos:pos + 4] != PROLOGUE:
            continue
        end = min(pos + 0x200, len(data))
        if data.find(ERROR, pos, end) < 0:
            continue
        insns = list(md.disasm(data[pos:pos + 0x320], pos))
        kind = []
        for ins in insns:
            if ins.address < pos + 0x80 or ins.address > pos + 0x200:
                continue
            if len(ins.operands) == 2 and ins.mnemonic == "mov":
                src = ins.operands[1]
                if src.type == X86_OP_MEM and src.mem.base == X86_REG_R15 and src.size == 4:
                    kind.append(src.mem.disp)
        if len(kind) == 1 and 0x80 <= kind[0] <= 0x1c0:
            entries.append((pos, kind[0]))
    if len(entries) != 1:
        raise ValueError(f"OpenVideo entry not unique: {entries}")
    entry, kind = entries[0]

    helpers = []
    for ins in md.disasm(data[0x1000:0x10000], 0x1000):
        if ins.mnemonic == "mov" and len(ins.operands) == 2 and \
           ins.operands[0].reg and ins.reg_name(ins.operands[0].reg) == "esi" and \
           ins.operands[1].type == X86_OP_IMM and ins.operands[1].imm == 0xA70001:
            start = max(0, ins.address - 0x90)
            sizes = []
            for prev in md.disasm(data[start:ins.address], start):
                store = mem_store(prev, X86_REG_RBP, 8)
                if store and store[0] == -0x18 and store[1] is not None:
                    sizes.append(store[1])
            if len(sizes) == 1:
                helpers.append((ins.address, sizes[0]))
    if len(helpers) != 1 or not 0x200 <= helpers[0][1] <= 0x400:
        raise ValueError(f"Avcap2 IPC request size not unique: {helpers}")
    return {"open_video": entry, "kind": kind, "size": helpers[0][1],
            "ipc_command": helpers[0][0]}


def constructor_layout_packed_simd(data: bytes, size: int, starts: list) -> dict | None:
    """Decode the alternate 7.xx RemotePlay constructor from its instructions.

    The request header/rate/image groups are copied with VMOVUPS, but the
    display dimensions arrive as one packed 64-bit VMOVQ plus a scalar dword.
    The older SIMD decoder only recognizes three scalar dword dimension stores.
    Reconstruct the actual stores instead of inheriting offsets from 6.02.
    Reject all ambiguous or structurally inconsistent candidates.
    """
    if size != 0x2a8:
        return None
    md = disassembler()
    candidates = []

    for addr, scalar_header in starts:
        if scalar_header is not None:
            continue
        insns = list(md.disasm(data[addr:addr + 0x540], addr))
        pending = {}
        pairs = []
        packed_width = []
        height = []
        paste = []
        mask = []

        for ins in insns:
            op = ins.operands
            if len(op) != 2:
                continue
            if op[0].type == X86_OP_REG:
                pending.pop(op[0].reg, None)
                if (ins.mnemonic == "vmovups" and op[1].type == X86_OP_MEM
                        and op[1].mem.base == X86_REG_RIP and op[1].size == 16):
                    origin = ins.address + ins.size + op[1].mem.disp
                    if 0 <= origin <= len(data) - 16:
                        a = int.from_bytes(data[origin:origin + 8], "little")
                        b = int.from_bytes(data[origin + 8:origin + 16], "little")
                        pending[op[0].reg] = (a, b)
            elif op[0].type == X86_OP_MEM and op[0].mem.base == X86_REG_RBX:
                disp = op[0].mem.disp
                if op[1].type == X86_OP_REG and op[0].size == 16:
                    pair = pending.get(op[1].reg)
                    if pair:
                        pairs.append((ins.address, disp, *pair))
                if ins.mnemonic == "vmovq" and op[0].size == 8:
                    packed_width.append((ins.address, disp))
                if ins.mnemonic == "mov" and op[0].size == 4:
                    if op[1].type == X86_OP_REG:
                        height.append((ins.address, disp))
                    elif op[1].type == X86_OP_IMM and op[1].imm == 2:
                        mask.append((ins.address, disp))
                if (ins.mnemonic == "mov" and op[0].size == 8
                        and op[1].type == X86_OP_IMM and op[1].imm == 0x88):
                    paste.append((ins.address, disp))

        headers = [(a, d) for a, d, v, w in pairs if (v, w) == (size, 0x28)]
        rates = [(a, d, v, w) for a, d, v, w in pairs
                 if (v, w) == (0xd0, 0x20)]
        images = [(a, d, v, w) for a, d, v, w in pairs
                  if (v, w) == (0xf8, 0x40)]
        if not all(len(x) == 1 for x in (headers, rates, images, packed_width, paste, mask)):
            continue
        header_addr, base = headers[0]
        rate_addr, rate_disp, rate_size, rate_desc = rates[0]
        image_addr, image_disp, image_size, image_desc = images[0]
        width_addr, width_disp = packed_width[0]
        paste_addr, paste_disp = paste[0]
        mask_addr, mask_disp = mask[0]
        # Only select the scalar height field following the packed width
        # and located before ImagePaste.
        heights = [(a, d) for a, d in height
                   if width_addr < a < paste_addr and d == width_disp + 8]
        if len(heights) != 1:
            continue
        if not (base <= rate_disp < image_disp < width_disp < paste_disp < mask_disp
                and header_addr < rate_addr < image_addr < width_addr < paste_addr < mask_addr
                and image_disp - base + image_size == size
                and mask_disp - base + 0x24 == size):
            continue
        candidates.append({
            "constructor": addr,
            "rate_group": rate_disp - base,
            "rate_group_size": rate_size,
            "rate_descriptor_size": rate_desc,
            "image_group": image_disp - base,
            "image_group_size": image_size,
            "image_descriptor_size": image_desc,
            "width": width_disp + 4 - base,
            "height": width_disp + 8 - base,
            "image_paste": paste_disp - base,
            "video_mode": mask_disp - 4 - base,
            "mask_mode": mask_disp - base,
        })

    return candidates[0] if len(candidates) == 1 else None

def constructor_layout_simd(data: bytes, size: int) -> dict:
    """Recover AVX-built RemotePlay requests from their RIP-relative constants.

    Older releases may initialize {request_size, header_size} and the grouped
    descriptors with 128-bit SIMD copies rather than immediate qword stores.
    Every accepted offset is recovered from the same, uniquely identified
    constructor; matching the request size alone is not sufficient.
    """
    md = disassembler()
    header = size.to_bytes(8, "little") + (0x28).to_bytes(8, "little")
    candidates = []
    starts = []
    start = 0
    # Decode the instruction itself after finding a compact AVX load prefix.
    # This avoids linear-disassembling megabytes of executable/data padding.
    while (start := data.find(b"\xc5\xf8\x10", start)) >= 0:
        addr = start
        start += 1
        insns = list(md.disasm(data[addr:addr + 16], addr, count=1))
        if not insns:
            continue
        ins = insns[0]
        if ins.mnemonic != "vmovups" or len(ins.operands) != 2:
            continue
        dst, src = ins.operands
        if dst.type != X86_OP_REG or src.type != X86_OP_MEM or src.mem.base != X86_REG_RIP:
            continue
        const = ins.address + ins.size + src.mem.disp
        if const < 0 or data[const:const + 16] != header:
            continue
        starts.append((addr, None))

    # Some early firmware uses a direct qword header size store through RDI,
    # followed by SIMD descriptor copies through its saved alias in RBX.
    scalar_signature = b"\x48\xc7\x47\x10" + size.to_bytes(4, "little")
    start = 0
    while (start := data.find(scalar_signature, start)) >= 0:
        starts.append((start, (start, 0x10)))
        start += 1

    for addr, scalar_header in starts:
        # Decode the contiguous constructor after its signature load. All
        # RIP-relative constants and destination stores must occur here.
        pending = {}
        integer_constants = {}
        pairs = []
        scalar = []
        for instr in md.disasm(data[addr:addr + 0x700], addr):
            op = instr.operands
            if len(op) == 2 and op[0].type == X86_OP_REG:
                pending.pop(op[0].reg, None)
                integer_constants.pop(op[0].reg, None)
                if instr.mnemonic in ("mov", "movabs") and op[1].type == X86_OP_IMM:
                    integer_constants[op[0].reg] = op[1].imm
                if (op[1].type == X86_OP_MEM and op[1].mem.base == X86_REG_RIP
                        and op[1].size == 16
                        and instr.mnemonic in ("vmovups", "vmovdqu", "movups", "movdqu")):
                    origin = instr.address + instr.size + op[1].mem.disp
                    if 0 <= origin <= len(data) - 16:
                        a = int.from_bytes(data[origin:origin + 8], "little")
                        b = int.from_bytes(data[origin + 8:origin + 16], "little")
                        # Some early memory dumps have the data image displaced
                        # by one byte relative to RIP references. Accept this
                        # only when a complete valid descriptor pair occurs at
                        # +1 and the unshifted bytes cannot be a descriptor.
                        def descriptor_pair(x, y):
                            return ((x in (0x68, *range(0xd0, 0xf0, 8)) and y == 0x20)
                                    or (x in (0xe8, 0xf8, 0x100) and y in (0x28, 0x40)))
                        if not descriptor_pair(a, b) and origin + 17 <= len(data):
                            shifted_a = int.from_bytes(data[origin + 1:origin + 9], "little")
                            shifted_b = int.from_bytes(data[origin + 9:origin + 17], "little")
                            if descriptor_pair(shifted_a, shifted_b):
                                a, b = shifted_a, shifted_b
                        pending[op[0].reg] = (a, b)
            elif (len(op) == 2 and op[0].type == X86_OP_MEM
                  and op[0].mem.base == X86_REG_RBX):
                if op[1].type == X86_OP_REG and op[0].size == 16 and op[1].reg in pending:
                    pairs.append((instr.address, op[0].mem.disp, *pending[op[1].reg]))
                else:
                    store = mem_store(instr, X86_REG_RBX)
                    if store:
                        disp, value = store
                        if value is None and op[1].type == X86_OP_REG:
                            value = integer_constants.get(op[1].reg)
                        scalar.append((instr.address, disp, value, op[0].size))

        headers = ([scalar_header] if scalar_header is not None else
                   [(a, d) for a, d, v1, v2 in pairs if (v1, v2) == (size, 0x28)])
        if len(headers) != 1:
            continue
        header_store, base = headers[0]
        if not 0 <= base <= 0x80:
            continue
        rate = [(a, d, sz, desc) for a, d, sz, desc in pairs
                if a > header_store and sz in (0x68, *range(0xd0, 0xf0, 8)) and desc == 0x20]
        image = [(a, d, sz, desc) for a, d, sz, desc in pairs
                 if a > header_store and sz in (0xe8, 0xf8, 0x100)
                 and desc in (0x28, 0x40)]
        if len(rate) != 1 or len(image) != 1:
            continue
        ia, idisp, isize, idesc = image[0]
        markers = [(a, d) for a, d, v, width in scalar
                   if a > ia and d > idisp and width == 8 and v == 0x88]
        if len(markers) != 1:
            continue
        marker_address, paste = markers[0]
        masks = [(a, d) for a, d, v, width in scalar
                 if a > marker_address and paste < d < paste + 0x120
                 and width == 4 and v == 2]
        masks += [(a, d + 4) for a, d, v, width in scalar
                  if a > marker_address and paste < d < paste + 0x120
                  and width == 8 and v == 0x200000000]
        if len(masks) != 1:
            continue
        mode = masks[0][1]
        variable = {d for a, d, v, width in scalar
                    if ia < a < marker_address and idisp < d < paste
                    and width == 4 and v is None}
        triples = [d for d in variable if d + 4 in variable and d + 8 in variable]
        if not triples:
            continue
        dimensions = max(triples)
        rate_address, rate_offset, rsize, rdesc = rate[0]
        if not (header_store < rate_address < ia < marker_address < masks[0][0]):
            continue
        if not (base <= rate_offset < idisp < dimensions < paste < mode):
            continue
        candidate = {
            "constructor": header_store,
            "rate_group": rate_offset - base,
            "rate_group_size": rsize,
            "rate_descriptor_size": rdesc,
            "image_group": idisp - base,
            "image_group_size": isize,
            "image_descriptor_size": idesc,
            "width": dimensions + 4 - base,
            "height": dimensions + 8 - base,
            "image_paste": paste - base,
            "video_mode": mode - 4 - base,
            "mask_mode": mode - base,
        }
        tail = size - candidate["mask_mode"]
        if tail not in (0x14, 0x24):
            continue
        if tail == 0x14:
            candidate["mask_span"] = 0x14
        if (candidate["image_group"] + isize != size
                or candidate["height"] >= candidate["image_paste"]):
            continue
        candidates.append(candidate)
    if len(candidates) != 1:
        if not candidates:
            packed = constructor_layout_packed_simd(data, size, starts)
            if packed is not None:
                return packed
        raise ValueError(f"SIMD RemotePlay constructor not unique: {len(candidates)} candidates")
    return candidates[0]


def constructor_layout(data: bytes, size: int) -> dict:
    if len(data) < 0x100000:
        raise ValueError("no decrypted SceRemotePlay memory image")
    pattern = b"\x48\xc7\x43\x40" + size.to_bytes(4, "little")
    positions = []
    p = 0
    while (p := data.find(pattern, p)) >= 0:
        positions.append(p)
        p += 1
    if not positions:
        return constructor_layout_simd(data, size)
    if len(positions) != 1:
        raise ValueError(f"native constructor size store not unique: {positions}")
    pos = positions[0]
    insns = list(disassembler().disasm(data[pos:pos + 0x1000], pos))
    stores = [(ins.address, *s, ins.operands[0].size) for ins in insns
              if (s := mem_store(ins, X86_REG_RBX)) is not None]

    def descriptor(values, second_values):
        found = []
        for address, disp, imm, width in stores:
            if width == 8 and imm in values and any(
                d == disp + 8 and v in second_values and w == 8 and 0 < a - address < 32
                for a, d, v, w in stores):
                found.append((address, disp, imm, next(v for a, d, v, w in stores
                              if d == disp + 8 and v in second_values and w == 8
                              and 0 < a - address < 32)))
        if len(found) != 1:
            raise ValueError(f"native descriptor not unique: {found}")
        return found[0]

    rate = descriptor(set(range(0xD0, 0xF0, 8)), {0x20})
    image = descriptor({0x100, 0xE8, 0xF8}, {0x40, 0x28})
    markers = [(a, d) for a, d, v, w in stores
               if w == 8 and v == 0x88 and a > image[0] and d > image[1]]
    if not markers:
        raise ValueError("ImagePaste marker absent")
    image_marker = min(markers)[1]
    masks = [(a, d) for a, d, v, w in stores
             if w == 4 and v == 2 and d > image_marker and d < image_marker + 0x120]
    if len(masks) != 1:
        raise ValueError(f"ImagePaste mode not unique: {masks}")
    mask_mode = masks[0][1]
    regular = {d for a, d, v, w in stores if w == 4 and v is None
               and image[1] < d < image_marker}
    triples = [d for d in regular if d + 4 in regular and d + 8 in regular]
    if not triples:
        raise ValueError("native dimensions not found")
    # The final consecutive triple before ImagePaste is format, width, height.
    dimensions = max(triples)
    if dimensions + 8 >= image_marker:
        raise ValueError("native dimensions overlap ImagePaste")
    return {"constructor": pos, "rate_group": rate[1] - 0x40,
            "rate_group_size": rate[2], "rate_descriptor_size": rate[3],
            "image_group": image[1] - 0x40,
            "image_group_size": image[2], "image_descriptor_size": image[3],
            "width": dimensions + 4 - 0x40, "height": dimensions + 8 - 0x40,
            "image_paste": image_marker - 0x40,
            "mask_mode": mask_mode - 0x40,
            "video_mode": mask_mode - 4 - 0x40}


def fw_name(fw: int) -> str:
    return f"{fw >> 24:x}{(fw >> 16) & 0xff:02x}".lstrip("0") or "0"


def render_profile(fw: int, result: dict) -> str:
    h = lambda value: f"0x{value:x}u"
    lines = ["// SPDX-License-Identifier: GPL-3.0-or-later"]
    if fw == 0x04000042:
        lines += ["// EXPERIMENTAL: firmware 4.00 dump has a one-byte RIP/rodata mismatch.",
                  "// The profile is disabled in normal payload builds pending hardware verification."]
    lines += ['#include "platform/ps5/offset/profile.h"', "",
             f"const pbs_ps5_offset_profile pbs_ps5_offsets_{fw_name(fw)} = {{",
             f"  .firmware = 0x{fw:08x}u,",
             "  .avcap2 = {",
             f"    .open_video_rva = {h(result['open_video'])},",
             f"    .ipc_command_instruction_rva = {h(result['ipc_command'])},",
             f"    .request_size = {h(result['size'])},",
             f"    .kind_offset = {h(result['kind'])},"]
    lines += ["  },", "  .remoteplay = {",
              f"    .request_size_store_rva = {h(result['constructor'])},",
              "    .rate = {",
              f"      .offset = {h(result['rate_group'])},",
              f"      .size = {h(result['rate_group_size'])},",
              f"      .descriptor_size = {h(result['rate_descriptor_size'])},",
              "    },", "    .image = {",
              f"      .offset = {h(result['image_group'])},",
              f"      .size = {h(result['image_group_size'])},",
              f"      .descriptor_size = {h(result['image_descriptor_size'])},",
              "    },",
              f"    .width_offset = {h(result['width'])},",
              f"    .height_offset = {h(result['height'])},",
              f"    .paste_offset = {h(result['image_paste'])},",
              f"    .video_mode_offset = {h(result['video_mode'])},",
              f"    .mask_mode_offset = {h(result['mask_mode'])},"]
    if result.get("mask_span", 0x24) != 0x24:
        lines.append(f"    .mask_span = {h(result['mask_span'])},")
    lines += ["  },", "};"]
    return "\n".join(lines) + "\n"


def existing_profiles(directory: Path) -> dict[int, Path]:
    """Keep every already supported firmware even if its dump is absent."""
    known = {}
    for path in sorted(directory.glob("*.c")):
        if path.name == "registry.c":
            continue
        text = path.read_text(encoding="utf-8")
        fw_match = re.search(r"\.firmware\s*=\s*0x([0-9a-fA-F]{8})u", text)
        symbol = re.search(r"const pbs_ps5_offset_profile pbs_ps5_offsets_([0-9a-f]+)\s*=", text)
        if not fw_match or not symbol:
            raise ValueError(f"not a recognized firmware profile: {path}")
        fw = int(fw_match.group(1), 16)
        if symbol.group(1) != fw_name(fw) or path.stem != fw_name(fw):
            raise ValueError(f"firmware/profile filename mismatch: {path}")
        if fw in known:
            raise ValueError(f"duplicate firmware profile for 0x{fw:08x}")
        known[fw] = path
    return known


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dumps", type=Path,
                        help="directory containing firmware ZIP, RAR, 7z or TAR dumps")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1] /
                        "payload/src/platform/ps5/offset")
    parser.add_argument("--overwrite-existing", action="store_true",
                        help="explicitly replace existing profiles if a newer dump differs")
    parser.add_argument("--reference-940", type=Path,
                        help="decrypted 9.40 Avcap2 image; inherit 9.20 native layout")
    parser.add_argument("--seven-zip", default=shutil.which("7z") or
                        r"C:\Program Files\7-Zip\7z.exe")
    parser.add_argument("--verbose", action="store_true",
                        help="print individual rejected dump reasons")
    args = parser.parse_args()
    reference_explicit = args.reference_940 is not None
    if args.reference_940 is None:
        sibling_reference = (Path(__file__).resolve().parents[2] / "PBS" /
                             "notes/re/session-20261015/avcap2.bin")
        if sibling_reference.is_file():
            args.reference_940 = sibling_reference
    gathered: dict[int, dict[str, bytes]] = {}
    for archive in sorted(args.dumps.iterdir()):
        if archive.suffix.lower() not in (".zip", ".rar", ".7z", ".tar"):
            continue
        for name, data in load_members(archive, args.seven_zip).items():
            match = FW_RE.search(name)
            if not match:
                continue
            fw = int(match.group(1), 16)
            key = "avcap" if name.endswith(MODULE) else "eboot"
            gathered.setdefault(fw, {}).setdefault(key, []).append((data, archive.name + ":" + name))
    profiles = {}
    for fw, members in sorted(gathered.items()):
        avcap = []
        for data, origin in members.get("avcap", []):
            try:
                avcap.append((avcap_layout(data), origin))
            except ValueError as exc:
                if args.verbose:
                    print(f"{fw:08x}: rejected Avcap2 {origin}: {exc}")
        if not avcap:
            if not (fw == 0x09400008 and args.reference_940):
                print(f"{fw:08x}: skipped (no usable Avcap2 memory dump)")
            continue
        if len({tuple(sorted(a.items())) for a, _ in avcap}) != 1:
            print(f"{fw:08x}: skipped (conflicting Avcap2 dumps)")
            continue
        abi, avcap_source = avcap[0]
        natives = []
        for data, origin in members.get("eboot", []):
            try:
                natives.append((constructor_layout(data, abi["size"]), origin))
            except ValueError as exc:
                if args.verbose:
                    print(f"{fw:08x}: rejected RemotePlay {origin}: {exc}")
        if not natives or len({tuple(sorted(x.items())) for x, _ in natives}) != 1:
            print(f"{fw:08x}: skipped (missing or conflicting native constructor)")
            continue
        native, native_source = natives[0]
        profiles[fw] = (abi | native, avcap_source + " | " + native_source)
        if fw == 0x04000042:
            print("04000042: EXPERIMENTAL - one-byte RIP/rodata mismatch in source dump")
        print(f"{fw:08x}: size=0x{abi['size']:x} kind=0x{abi['kind']:x} "
              f"constructor=0x{native['constructor']:x}")
    if args.reference_940 and 0x09200005 not in profiles:
        if reference_explicit:
            raise ValueError("cannot inherit 9.40 layout without validated 9.20 dump")
        print("09400008: skipped inherited layout (9.20 dump not in this batch)")
    elif args.reference_940:
        fw = 0x09400008
        abi = avcap_layout(args.reference_940.read_bytes())
        base = profiles.get(0x09200005)
        if base is None or (abi["size"], abi["kind"]) != (base[0]["size"], base[0]["kind"]):
            raise ValueError("9.40 Avcap2 ABI does not match verified 9.20 layout")
        native = {k: v for k, v in base[0].items() if k not in abi}
        native["constructor"] = 0  # no native 9.40 SceRemotePlay image in this set
        profiles[fw] = (abi | native, args.reference_940.name + " | 9.20 constructor layout")
        print("09400008: Avcap2 verified; request constructor inherited from 9.20")
    args.out.mkdir(parents=True, exist_ok=True)
    known = existing_profiles(args.out)
    stems = [fw_name(fw) for fw in set(profiles) | set(known)]
    if len(stems) != len(set(stems)):
        raise ValueError("multiple firmware builds share a short filename; keep separate profiles")
    for fw, (result, _) in sorted(profiles.items()):
        path = args.out / f"{fw_name(fw)}.c"
        generated = render_profile(fw, result)
        if fw in known and not args.overwrite_existing:
            if path.read_text(encoding="utf-8") != generated:
                print(f"{fw:08x}: preserved existing profile (differs from dump; "
                      "pass --overwrite-existing to update)")
            continue
        if not path.exists() or path.read_text(encoding="utf-8") != generated:
            path.write_text(generated, encoding="utf-8")
            print(f"{fw:08x}: wrote {path.name}")
        known[fw] = path
    firmware_ids = sorted(known)
    symbols = [f"pbs_ps5_offsets_{fw_name(fw)}" for fw in firmware_ids]
    registry = ['// SPDX-License-Identifier: GPL-3.0-or-later',
                '#include "platform/ps5/offset/profile.h"', ""]
    registry += [f"extern const pbs_ps5_offset_profile {name};" for name in symbols]
    registry += ["", "const pbs_ps5_offset_profile *pbs_ps5_find_offsets(uint32_t firmware) {",
                 "  switch (firmware) {"]
    for fw in firmware_ids:
        case = f"    case 0x{fw:08x}u: return &pbs_ps5_offsets_{fw_name(fw)};"
        if fw == 0x04000042:
            # The 4.00 loaded dump has RIP-relative data displaced by one
            # byte; never select this inferred profile in normal builds.
            registry += ["#ifdef PBS_PS5_EXPERIMENTAL_FW400", case, "#endif"]
        else:
            registry.append(case)
    if 0x13600007 in known:
        registry += ["    default:",
                     "      /* 13.40 and 13.60 have identical executable Avcap2 segments. The",
                     "       * 13.40 SceRemotePlay constructor is not available for comparison. */",
                     "      if ((firmware & 0xffff0000u) == 0x13400000u)",
                     "        return &pbs_ps5_offsets_1360;",
                     "      return 0;"]
    else:
        registry += ["    default: return 0;"]
    registry += ["  }", "}", ""]
    registry_path = args.out / "registry.c"
    registry_text = "\n".join(registry)
    if not registry_path.exists() or registry_path.read_text(encoding="utf-8") != registry_text:
        registry_path.write_text(registry_text, encoding="utf-8")
        print(f"updated {registry_path.name}")
    print(f"registered {len(firmware_ids)} profiles in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
