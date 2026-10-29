#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay measured 6.72 instruction contexts without shipping a kernel image."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


@unittest.skipUnless(shutil.which(os.environ.get('CC', 'cc')), 'host C compiler required')
class Layout672Test(unittest.TestCase):
    def test_measured_context_and_adversarial_variants(self):
        fixture = json.loads((ROOT / 'tests/fixtures/ps4/6.72/kernel-context.json').read_text())
        # Preserve the measured instruction addresses and all rel32 links.
        # The rest of this test ELF is zero-filled, not a firmware image.
        image = bytearray(0x240000)
        image[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', image, 18, 62)
        struct.pack_into('<Q', image, 32, 64)
        struct.pack_into('<HH', image, 54, 56, 1)
        struct.pack_into('<IIQQQQQQ', image, 64, 1, 5, 0,
                         0xffffffff80000000, 0xffffffff80000000,
                         len(image), len(image), 0x1000)
        for region in fixture['regions']:
            at, data = int(region['offset'],16), bytes.fromhex(region['hex'])
            image[at:at+len(data)] = data
        with tempfile.TemporaryDirectory() as folder:
            runner = Path(folder) / 'layout'
            kernel = Path(folder) / 'context.bin'
            kernel.write_bytes(image)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2',
                            '-Wall', '-Wextra', '-Werror', '-Ipayload/src',
                            'tests/ps4/system/layout.c',
                            'payload/src/platform/ps4/system/layout.c',
                            '-o', str(runner)], cwd=ROOT, check=True,
                           capture_output=True, text=True)
            result = subprocess.run([sys.executable, 'tools/verify_ps4_layout.py',
                                     '--runner', str(runner), '--kernel', str(kernel)],
                                    cwd=ROOT, check=True, capture_output=True, text=True)
            report = json.loads(result.stdout)
            self.assertTrue(report['ok'])
            expected = fixture['expected']
            self.assertEqual(report['checks'][0]['result'],
                             'acmgr=%x allow=%x policy=%x jump=%s' % (
                                 int(expected['acmgr'],16), int(expected['allow'],16),
                                 int(expected['policy'],16), expected['jump']))
            refused = {case['case'] for case in report['checks'] if not case['accepted']}
            self.assertTrue({'wrong-acmgr', 'wrong-whitelist', 'wrong-policy',
                             'foreign-patch', 'duplicate-executable-candidates',
                             'truncated', 'mismatched-credential-helper',
                             'wrong-bypass-instruction'}.issubset(refused))


if __name__ == '__main__':
    unittest.main()
