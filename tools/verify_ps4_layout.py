#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline checks against user-supplied private dumps. Never contacts a console.

Build tests/ps4/system/layout.c + system/layout.c as the runner first. Dumps
and mutated images stay local; no firmware bytes are bundled with this tool.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--runner', required=True, type=Path)
    ap.add_argument('--kernel', required=True, type=Path)
    ap.add_argument('--avcap', required=True, type=Path)
    args = ap.parse_args()
    runner = str(args.runner.resolve())
    original = args.kernel.read_bytes()
    reports = []
    with tempfile.TemporaryDirectory(prefix='pbs-layout-') as folder:
        folder = Path(folder)

        def check(name, data, accepted=True, extra=()):
            path = folder / (name + '.bin')
            path.write_bytes(data)
            cmd = [runner, str(path)] if not extra else [runner, '--trap', str(path), *extra]
            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
            if (proc.returncode == 0) != accepted:
                raise RuntimeError(f'{name}: unexpected result {proc.returncode}: {proc.stdout} {proc.stderr}')
            reports.append({'case': name, 'accepted': accepted, 'result': proc.stdout.strip()})
            return proc.stdout

        result = check('original', original)
        match = re.fullmatch(r'acmgr=([0-9a-f]+) allow=([0-9a-f]+) policy=([0-9a-f]+) jump=([0-9a-f]+)\s*', result)
        if not match:
            raise RuntimeError('Unrecognized runner report')
        acmgr, allow, policy = (int(match[i], 16) for i in (1, 2, 3))
        jump = bytes.fromhex(match[4])
        armed = bytearray(original)
        armed[allow] = 0xeb
        armed[policy:policy+5] = jump
        assert check('already-armed', armed) == result
        for name, at in [('wrong-acmgr', acmgr), ('wrong-whitelist', allow+4),
                         ('wrong-policy', policy+6), ('foreign-patch', policy)]:
            changed = bytearray(original)
            changed[at] ^= 0x40
            check(name, changed, False)
        phoff = struct.unpack_from('<Q', original, 32)[0]
        stride, count = struct.unpack_from('<HH', original, 54)
        headers = [struct.unpack_from('<IIQQQQQQ', original, phoff+i*stride) for i in range(count)]
        text_index, text = next((i, p) for i, p in enumerate(headers) if p[0] == 1 and p[1] & 1)
        linked = min(p[3] for p in headers if p[0] == 1)
        assert text[3] == linked and text[2] == 0
        # Shift every instruction and its relative targets. Preserve a separate
        # readable ELF header at the runtime base, with a small non-exec LOAD.
        for shift in (0x3000, 0x9100):
            changed = bytearray(original)
            changed[shift:shift+text[5]] = original[:text[5]]
            changed[:phoff+stride*(count+1)] = original[:phoff+stride*(count+1)]
            moved = list(text)
            moved[2] += shift; moved[3] += shift; moved[4] += shift
            struct.pack_into('<IIQQQQQQ', changed, phoff+text_index*stride, *moved)
            struct.pack_into('<IIQQQQQQ', changed, phoff+count*stride, 1, 4, 0, linked, linked, 0x1000, 0x1000, 0x1000)
            struct.pack_into('<H', changed, 56, count+1)
            moved_result = check(f'relocated-{shift:x}', changed)
            assert f'acmgr={acmgr+shift:x} allow={allow+shift:x} policy={policy+shift:x}' in moved_result
        duplicate_at = 0x3000000
        changed = bytearray(original)
        changed.extend(bytes(duplicate_at+text[5]-len(changed)))
        changed[duplicate_at:duplicate_at+text[5]] = original[:text[5]]
        duplicate = list(text)
        duplicate[2] = duplicate_at; duplicate[3] += duplicate_at; duplicate[4] += duplicate_at
        struct.pack_into('<IIQQQQQQ', changed, phoff+count*stride, *duplicate)
        struct.pack_into('<H', changed, 56, count+1)
        check('duplicate-executable-candidates', changed, False)
        check('truncated', original[:0x1000], False)
        # Map the actual module's LOAD segments and simulate resolved import
        # GOT values. Import stub/padding positions still come from the dump.
        from inspect_orbis import OrbisElf, nid
        module = OrbisElf(args.avcap)
        mapped = bytearray(max(p[3]+p[6] for p in module.ph if p[0] == 1))
        for p in module.ph:
            if p[0] == 1:
                mapped[p[3]:p[3]+p[5]] = module.data[p[2]:p[2]+p[5]]
        for at in module.import_stubs():
            got = at+6+struct.unpack_from('<i', mapped, at+2)[0]
            struct.pack_into('<Q', mapped, got, 0x10001000)
        init = next(s['value'] for s in module.symbols if s['defined'] and s['nid'].split('#')[0] == nid('sceVshAvcapInit'))
        check('actual-avcap-import-padding', mapped, extra=(hex(init),))
    print(json.dumps({'ok': True, 'checks': reports}, indent=2))


if __name__ == '__main__':
    main()
