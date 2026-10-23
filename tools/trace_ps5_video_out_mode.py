"""Compare firmware dumps and disassemble the client-side video mode IPC path.

This proves where the returned 32-bit value enters libSceAvcap2; it cannot
identify the server-side 4K/HDR/VRR enum without a decrypted backend dump.
No dumps are modified. Requires: pip install capstone.
"""

import argparse
import hashlib
import zipfile
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64

FIRMWARE_ARCHIVES = ("fw11.60.zip", "fw12.40.zip", "fw12.70.zip")
MODULE = "libSceAvcap2.sprx.prefix.bin"
VENC = "libSceVenc.sprx.prefix.bin"
ARBITRATOR = "libSceResourceArbitrator.sprx.prefix.bin"
VIDEO_OUT = "libSceVideoOut.sprx"
GET_MODE_RVA = {"fw11.60.zip": 0x1840, "fw12.40.zip": 0x1850, "fw12.70.zip": 0x1850}
GET_MODE_IPC = (0x4B10, 0x4B6F)
OPEN_VIDEO_IPC = (0x4430, 0x44C2)
GETTER_WRAPPER = (0x1850, 0x18D0)
OPEN_WRAPPER = (0x360, 0x480)
TEXT_WINDOW = 0x10000


def load(z: zipfile.ZipFile, filename: str, last: bool = False) -> bytes | None:
    names = [n for n in z.namelist() if n.endswith("/" + filename)]
    if not names:
        return None
    return z.read(names[-1 if last else 0])


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]


def lines(data: bytes, begin: int, end: int) -> list[str]:
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    return [f"{i.address:06x}: {i.mnemonic:<10} {i.op_str}"
            for i in md.disasm(data[begin:end], begin)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dumps", type=Path)
    args = ap.parse_args()
    payloads: dict[str, dict[str, bytes]] = {}
    for fw in FIRMWARE_ARCHIVES:
        archive = args.dumps / fw
        with zipfile.ZipFile(archive) as z:
            modules = {}
            for mod in (MODULE, VENC, ARBITRATOR):
                blob = load(z, mod, last=mod == VENC)
                if blob:
                    modules[mod] = blob
            video_out = load(z, VIDEO_OUT)
            if video_out:
                print(f"{fw} libSceVideoOut disk header={video_out[:4].hex()} "
                      f"(decrypted image={video_out[:4] != bytes.fromhex('5414f5ee')})")
            payloads[fw] = modules
        b = modules[MODULE]
        print(f"\n{fw} GetVideoOutMode RVA=0x{GET_MODE_RVA[fw]:x}")
        print("  mode values 0x13c and 0x144 encoded in first 64 KiB:",
              b[:TEXT_WINDOW].count((0x13C).to_bytes(4, "little")),
              b[:TEXT_WINDOW].count((0x144).to_bytes(4, "little")))
        print("  getter IPC command occurrences in first 64 KiB:",
              b[:TEXT_WINDOW].count((0xA7000E).to_bytes(4, "little")))
        for mod, data in modules.items():
            print(f"  {mod}: first 64 KiB SHA256 prefix {digest(data[:TEXT_WINDOW])}")
    ref = payloads["fw12.70.zip"][MODULE]
    for name, (start, end) in {
        "Getter IPC 0xA7000E": GET_MODE_IPC,
        "OpenVideo IPC 0xA70001": OPEN_VIDEO_IPC,
    }.items():
        print(f"\n12.70 disassembly of {name}:")
        for row in lines(ref, start, end):
            print(" ", row)
    base = payloads["fw12.40.zip"]
    next_ = payloads["fw12.70.zip"]
    print("\n12.40 versus 12.70:")
    for name, (start, end) in {
        "getter wrapper": GETTER_WRAPPER,
        "getter IPC": GET_MODE_IPC,
        "OpenVideo wrapper": OPEN_WRAPPER,
        "OpenVideo IPC": OPEN_VIDEO_IPC,
    }.items():
        a, b = base[MODULE][start:end], next_[MODULE][start:end]
        print(f"  {name} 0x{start:x}..0x{end:x}: identical={a == b}")
    for mod in (VENC, ARBITRATOR):
        a, b = base[mod][:TEXT_WINDOW], next_[mod][:TEXT_WINDOW]
        differences = sum(x != y for x, y in zip(a, b))
        print(f"  {mod}: first 64 KiB differing bytes={differences}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())