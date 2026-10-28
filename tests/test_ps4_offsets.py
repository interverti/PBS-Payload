#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for tools/ps4_offsets.py. No console, no private binaries."""
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    'ps4_offsets', ROOT / 'tools' / 'ps4_offsets.py')
offsets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(offsets)

REPORT = """PS4-DUMP v1 firmware=0x11000000 kernel_base=0xffffffff80000000 image_size=0x1234000
kernel layout status=discovered firmware=0x11000000 acmgr=0x3d0de0 gate0=0x384285 before0=0x77,0x1c after0=0xeb,0x1c size0=2 gate1=0x384771 before1=0x48,0x8b,0x41,0x08,0x44 after1=0xe9,0x7c,0x02,0x00,0x00 size1=5
process name=SceRemotePlay pid=812
module process=SceRemotePlay name=libSceAvcap.sprx handle=3 base=0x7f000000 size=0x300000 entry=0x7f000100
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_init address=0x7f000940 rva=0x940
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_read_video address=0x7f001c10 rva=0x1c10
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_close address=0x0 rva=unresolved
PS4-DUMP finished failures=1
"""

OFFSETS = """# PS4-DUMP v1 firmware=0x11000000
acmgr_offset=0x3d0de0
gate0_offset=0x384285
gate1_offset=0x384771
avcap_init=0x940
avcap_trap=unresolved
# failures=1
"""


def stub_run(count):
    """Six-byte FF25 stub plus ten padding bytes, sixteen bytes per slot."""
    out = bytearray()
    for _ in range(count):
        out += b'\xff\x25' + struct.pack('<i', 0x100) + b'\xcc' * 10
    return bytes(out)


def make_elf(segment, vaddr):
    """Minimal ELF64 with one executable PT_LOAD, as the probe payload emits."""
    header_size = 64
    phent_size = 56
    data = bytearray(header_size + phent_size + len(segment))
    data[:4] = b'\x7fELF'
    data[4] = 2  # ELFCLASS64
    data[5] = 1  # little endian
    struct.pack_into('<H', data, 16, 2)   # ET_EXEC
    struct.pack_into('<H', data, 18, 62)  # EM_X86_64
    struct.pack_into('<Q', data, 32, header_size)  # e_phoff
    struct.pack_into('<H', data, 52, header_size)  # e_ehsize
    struct.pack_into('<HH', data, 54, phent_size, 1)  # e_phentsize, e_phnum
    offset = header_size + phent_size
    struct.pack_into('<IIQQQQQQ', data, header_size, 1, 5, offset, vaddr,
                     vaddr, len(segment), len(segment), 0x1000)
    data[offset:offset + len(segment)] = segment
    return bytes(data)


class ReportTest(unittest.TestCase):
    def test_dump_report(self):
        firmware, kernel, parsed, symbols = offsets.parse_report(REPORT)
        self.assertEqual(firmware, 0x11000000)
        self.assertEqual(kernel['status'], 'discovered')
        self.assertEqual(kernel['acmgr'], 0x3d0de0)
        self.assertEqual(kernel['gate1'], 0x384771)
        self.assertEqual(kernel['before1'], [0x48, 0x8b, 0x41, 0x08, 0x44])
        self.assertEqual(kernel['size1'], 5)
        self.assertEqual(len(symbols), 3)
        self.assertIsNone(symbols[2]['rva'])
        self.assertEqual(parsed, {})

    def test_offsets_report(self):
        firmware, kernel, parsed, symbols = offsets.parse_report(OFFSETS)
        self.assertEqual(firmware, 0x11000000)
        self.assertIsNone(kernel)
        self.assertEqual(parsed['avcap_init'], 0x940)
        self.assertIsNone(parsed['avcap_trap'])
        self.assertEqual(symbols, [])

    def test_missing_header(self):
        firmware, _, _, _ = offsets.parse_report('nothing useful\n')
        self.assertIsNone(firmware)


class PaddingRunTest(unittest.TestCase):
    def test_twenty_slots_detected(self):
        segment = b'\x90' * 0x40 + stub_run(20) + b'\x90' * 0x40
        runs = offsets.import_padding_runs(make_elf(segment, 0x1000))
        self.assertEqual(runs, [(0x1000 + 0x40, 20)])

    def test_nineteen_slots_rejected(self):
        segment = stub_run(19)
        self.assertEqual(offsets.import_padding_runs(make_elf(segment, 0x1000)), [])

    def test_two_runs_sorted(self):
        segment = stub_run(20) + b'\x90' * 0x80 + stub_run(21)
        runs = offsets.import_padding_runs(make_elf(segment, 0x2000))
        self.assertEqual(runs, [(0x2000, 20), (0x2000 + 20 * 16 + 0x80, 21)])

    def test_rejects_non_elf(self):
        with self.assertRaises(ValueError):
            offsets.import_padding_runs(b'not an elf')


class NearestRunTest(unittest.TestCase):
    def test_nearest_before_init(self):
        runs = [(0x100, 20), (0x5000, 20), (0x20000, 20)]
        self.assertEqual(offsets.nearest_run(runs, 0x940), (0x100, 20))

    def test_no_run_before_init(self):
        self.assertIsNone(offsets.nearest_run([(0x2000, 20)], 0x940))

    def test_empty(self):
        self.assertIsNone(offsets.nearest_run([], 0x940))
        self.assertIsNone(offsets.nearest_run([(0x100, 20)], None))


class SnippetTest(unittest.TestCase):
    def test_snippet_carries_measured_values(self):
        firmware, kernel, parsed, symbols = offsets.parse_report(REPORT)
        init = next(s['rva'] for s in symbols if s['label'] == 'avcap_init')
        text = offsets.snippet(firmware, kernel, parsed, [], init)
        self.assertIn('.acmgr_offset = 0x3d0de0,', text)
        self.assertIn('{0x384285, {0x77, 0x1c}, {0xeb, 0x1c}, 2},', text)
        self.assertIn('.avcap_init_offset = 0x940,', text)
        self.assertIn('TODO avcap_trap_offset', text)
        self.assertNotIn('.avcap_trap_offset =', text)

    def test_snippet_without_kernel_layout(self):
        text = offsets.snippet(0x11000000, {'status': 'unavailable'}, {}, [], None)
        self.assertIn('TODO kernel layout', text)
        self.assertIn('TODO avcap_init_offset', text)


class CommandLineTest(unittest.TestCase):
    def test_cli_writes_files(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            report = folder / 'dump.txt'
            report.write_text(REPORT, encoding='utf-8')
            data = folder / 'data.json'
            snippet = folder / 'snippet.h'
            done = subprocess.run(
                [sys.executable, str(ROOT / 'tools' / 'ps4_offsets.py'),
                 str(report), '--json', str(data), '--snippet', str(snippet)],
                capture_output=True, text=True, check=False)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('.avcap_init_offset = 0x940,', done.stdout)
            self.assertEqual(json.loads(data.read_text(encoding='utf-8'))['firmware'],
                             '0x11000000')
            self.assertIn('.acmgr_offset = 0x3d0de0,',
                          snippet.read_text(encoding='utf-8'))

    def test_cli_rejects_report_without_header(self):
        with tempfile.TemporaryDirectory() as folder:
            report = Path(folder) / 'empty.txt'
            report.write_text('symbol process=x\n', encoding='utf-8')
            done = subprocess.run(
                [sys.executable, str(ROOT / 'tools' / 'ps4_offsets.py'), str(report)],
                capture_output=True, text=True, check=False)
            self.assertNotEqual(done.returncode, 0)
            self.assertIn('no PS4-DUMP header', done.stderr)

    def test_cli_reports_missing_files(self):
        with tempfile.TemporaryDirectory() as folder:
            report = Path(folder) / 'dump.txt'
            report.write_text(REPORT, encoding='utf-8')
            missing = Path(folder) / 'absent.bin'
            for argument in ([str(missing)],
                             [str(report), '--module', str(missing)]):
                done = subprocess.run(
                    [sys.executable, str(ROOT / 'tools' / 'ps4_offsets.py')] + argument,
                    capture_output=True, text=True, check=False)
                self.assertNotEqual(done.returncode, 0)
                self.assertIn('cannot read', done.stderr)
                self.assertNotIn('Traceback', done.stderr)

    def test_cli_rejects_module_that_is_not_elf(self):
        with tempfile.TemporaryDirectory() as folder:
            report = Path(folder) / 'dump.txt'
            report.write_text(REPORT, encoding='utf-8')
            module = Path(folder) / 'module.bin'
            module.write_bytes(b'not an elf at all')
            done = subprocess.run(
                [sys.executable, str(ROOT / 'tools' / 'ps4_offsets.py'),
                 str(report), '--module', str(module)],
                capture_output=True, text=True, check=False)
            self.assertNotEqual(done.returncode, 0)
            self.assertIn('little-endian ELF64', done.stderr)
            self.assertNotIn('Traceback', done.stderr)


if __name__ == '__main__':
    unittest.main()
