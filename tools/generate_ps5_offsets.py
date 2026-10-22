#!/usr/bin/env python3
"""Generate PS5 Avcap2 request layouts from loaded module dumps.

Requires capstone (`python -m pip install capstone`). RAR input also requires
7-Zip. Disk .sprx SELF files are deliberately ignored.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RBX, X86_REG_RBP, X86_REG_R15

MODULE = "libSceAvcap2.sprx.prefix.bin"
EBOOT = "eboot.bin.prefix.bin"
FW_RE = re.compile(r"ps5-([0-9a-fA-F]{8})-")
PROLOGUE = b"\x55\x48\x89\xe5"
ERROR = (0x81950001).to_bytes(4, "little")


def disassembler():
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    return md


def load_members(archive: Path, seven_zip: str) -> dict[str, bytes]:
    if archive.suffix.lower() == ".zip":
        with zipfile.ZipFile(archive) as z:
            return {n: z.read(n) for n in z.namelist()
                    if n.endswith((MODULE, EBOOT))}
    if archive.suffix.lower() != ".rar":
        return {}
    listing = subprocess.run([seven_zip, "l", "-slt", str(archive)],
                             capture_output=True, text=True, check=True).stdout
    names = [line[7:] for line in listing.splitlines() if line.startswith("Path = ")]
    out = {}
    for name in names:
        if name.endswith((MODULE, EBOOT)):
            out[name] = subprocess.run([seven_zip, "x", "-so", str(archive), name],
                                       capture_output=True, check=True).stdout
    return out


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
        if len(kind) == 1:
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


def constructor_layout(data: bytes, size: int) -> dict:
    if len(data) < 0x100000:
        raise ValueError("no decrypted SceRemotePlay memory image")
    pattern = b"\x48\xc7\x43\x40" + size.to_bytes(4, "little")
    positions = []
    p = 0
    while (p := data.find(pattern, p)) >= 0:
        positions.append(p)
        p += 1
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
    lines = ["// SPDX-License-Identifier: GPL-3.0-or-later",
             '#include "platform/ps5/offset/profile.h"', "",
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
              f"    .mask_mode_offset = {h(result['mask_mode'])},",
              "  },", "};"]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dumps", type=Path, help="directory containing fw*.zip and fw*.rar")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1] /
                        "payload/src/platform/ps5/offset")
    parser.add_argument("--reference-940", type=Path,
                        help="decrypted 9.40 Avcap2 image; inherit 9.20 native layout")
    parser.add_argument("--seven-zip", default=shutil.which("7z") or
                        r"C:\Program Files\7-Zip\7z.exe")
    args = parser.parse_args()
    if args.reference_940 is None:
        sibling_reference = (Path(__file__).resolve().parents[2] / "PBS" /
                             "notes/re/session-20261015/avcap2.bin")
        if sibling_reference.is_file():
            args.reference_940 = sibling_reference
    gathered: dict[int, dict[str, bytes]] = {}
    for archive in sorted(args.dumps.iterdir()):
        if archive.suffix.lower() not in (".zip", ".rar"):
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
            except ValueError:
                pass
        if not avcap:
            if not (fw == 0x09400008 and args.reference_940):
                print(f"{fw:08x}: skipped (no usable Avcap2 memory dump)")
            continue
        if len({(a["size"], a["kind"], a["open_video"]) for a, _ in avcap}) != 1:
            print(f"{fw:08x}: skipped (conflicting Avcap2 dumps)")
            continue
        abi, avcap_source = avcap[0]
        natives = []
        for data, origin in members.get("eboot", []):
            try:
                natives.append((constructor_layout(data, abi["size"]), origin))
            except ValueError:
                pass
        if not natives or len({tuple(sorted(x.items())) for x, _ in natives}) != 1:
            print(f"{fw:08x}: skipped (missing or conflicting native constructor)")
            continue
        native, native_source = natives[0]
        profiles[fw] = (abi | native, avcap_source + " | " + native_source)
        print(f"{fw:08x}: size=0x{abi['size']:x} kind=0x{abi['kind']:x} "
              f"constructor=0x{native['constructor']:x}")
    if args.reference_940:
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
    stems = [fw_name(fw) for fw in profiles]
    if len(stems) != len(set(stems)):
        raise ValueError("multiple firmware builds share a short filename; keep separate profiles")
    for fw, (result, _) in profiles.items():
        (args.out / f"{fw_name(fw)}.c").write_text(render_profile(fw, result), encoding="utf-8")
    symbols = [f"pbs_ps5_offsets_{fw_name(fw)}" for fw in sorted(profiles)]
    registry = ['// SPDX-License-Identifier: GPL-3.0-or-later',
                '#include "platform/ps5/offset/profile.h"', ""]
    registry += [f"extern const pbs_ps5_offset_profile {name};" for name in symbols]
    registry += ["", "const pbs_ps5_offset_profile *pbs_ps5_find_offsets(uint32_t firmware) {",
                 "  switch (firmware) {"]
    registry += [f"    case 0x{fw:08x}u: return &pbs_ps5_offsets_{fw_name(fw)};"
                 for fw in sorted(profiles)]
    if 0x13600007 in profiles:
        registry += ["    default:",
                     "      /* 13.40 and 13.60 have identical executable Avcap2 segments. The",
                     "       * 13.40 SceRemotePlay constructor is not available for comparison. */",
                     "      if ((firmware & 0xffff0000u) == 0x13400000u)",
                     "        return &pbs_ps5_offsets_1360;",
                     "      return 0;"]
    else:
        registry += ["    default: return 0;"]
    registry += ["  }", "}", ""]
    (args.out / "registry.c").write_text("\n".join(registry), encoding="utf-8")
    print(f"wrote {len(profiles)} profiles to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
