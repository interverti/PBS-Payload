#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests for tools/data/ps4_firmware_offsets.json and its reader.

The 9.00 values come from a real PS4-DUMP report plus the matching pbs smoke
log; the 11.00 values from the legacy profile. No console, no kernel.
"""
import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    'ps4_firmware_table', ROOT / 'tools' / 'ps4_firmware_table.py')
table = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(table)

REPORT = """PS4-DUMP v1 firmware=0x09008031 kernel_base=0xffffffffd152c000 image_size=0x2820268
kernel layout status=unavailable firmware=0x09008031
process name=SceRemotePlay pid=48
module process=SceRemotePlay name=libSceAvcap.sprx handle=127 base=0x81e68c000 size=0x1c000 entry=0x81e68c000
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_init address=0x81e68c940 rva=0x940
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_open_video address=0x81e68d0c0 rva=0x10c0
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_read_video address=0x81e68d4a0 rva=0x14a0
module process=SceRemotePlay name=libSceLibcInternal.sprx handle=2 base=0x8090d4000 size=0x14c000 entry=0x8090d4000
symbol process=SceRemotePlay module=libSceLibcInternal.sprx label=libc_malloc address=0x809125220 rva=0x51220
process name=SceAvCapture pid=44
module process=SceAvCapture name=libSceAvcap.sprx status=unavailable
symbol process=SceAvCapture module=libSceAvcap.sprx label=avcap_init address=0x0 rva=unresolved
PS4-DUMP finished failures=1
"""

REPORT_1100 = """PS4-DUMP v1 firmware=0x11008001 kernel_base=0xffffffffd9390000 image_size=0x281da48
kernel layout status=unavailable firmware=0x11008001
process name=SceRemotePlay pid=47
module process=SceRemotePlay name=libSceAvcap.sprx handle=129 base=0x83a69c000 size=0x1c000 entry=0x83a69c000
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_init address=0x83a69c940 rva=0x940
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_read_video address=0x83a69d4a0 rva=0x14a0
symbol process=SceRemotePlay module=libSceAvcap.sprx label=avcap_close address=0x83a69d730 rva=0x1730
module process=SceRemotePlay name=libSceLibcInternal.sprx handle=2 base=0x837de8000 size=0x158000 entry=0x837de8000
symbol process=SceRemotePlay module=libSceLibcInternal.sprx label=libc_malloc address=0x837e10d60 rva=0x28d60
module process=SceRemotePlay name=libkernel_sys.sprx handle=8193 base=0x83549c000 size=0x90000 entry=0x8354c5238
symbol process=SceRemotePlay module=libkernel_sys.sprx label=kernel_usleep address=0x8354aafd0 rva=0xefd0
PS4-DUMP finished failures=12
"""


class TableTest(unittest.TestCase):
    def setUp(self):
        self.data, self.firmwares = table.load()

    def test_schema_and_validation(self):
        self.assertEqual(self.data['platform'], 'ps4')
        self.assertEqual(sorted(self.firmwares), ['11.00', '6.72', '9.00'])
        table.validate(self.data, self.firmwares)

    def test_900_kernel_values(self):
        kernel = self.firmwares['9.00']['kernel']
        self.assertEqual(table.as_hex(kernel['acmgr_offset'], 'x'), 0x8BC20)
        self.assertEqual([gate['offset'] for gate in kernel['gates']],
                         ['0x41f4e5', '0x41f9d1'])
        self.assertIsNone(kernel['acmgr_signature'])

    def test_672_measured_kernel_and_exports_do_not_inherit_trap(self):
        entry = self.firmwares['6.72']
        self.assertEqual(entry['version'], '0x06720001')
        self.assertEqual(entry['version_mask'], '0xffffffff')
        self.assertEqual(entry['kernel']['acmgr_offset'], '0x233bd0')
        self.assertEqual([gate['offset'] for gate in entry['kernel']['gates']],
                         ['0x10f879', '0x10fd22'])
        self.assertEqual(entry['kernel']['gates'][1]['after'], [233,226,2,0,0])
        self.assertIsNone(entry['avcap']['trap'])
        self.assertEqual(entry['avcap']['exports']['init'], '0x910')
        self.assertEqual(entry['avcap']['exports']['read_video'], '0x1490')
        self.assertEqual(entry['libraries']['libc_malloc']['rva'], '0x58c90')

    def test_supplied_report_fixtures(self):
        for release in ('6.72', '9.00'):
            path = ROOT / 'tests' / 'fixtures' / 'ps4' / release / 'dump.report'
            with contextlib.redirect_stdout(io.StringIO()) as output:
                failures = table.report_check(self.data, self.firmwares, path, release)
            self.assertEqual(failures, 0, output.getvalue())

    def test_unknown_kernel_gate_cannot_carry_patch_bytes(self):
        data = json.loads(json.dumps(self.data))
        entries = {entry['release']: entry for entry in data['firmwares']}
        entries['6.72']['kernel']['gates'][0]['offset'] = None
        entries['6.72']['kernel']['gates'][0].update(
            size=2, before=[0x77,0x1c], after=[0xeb,0x1c])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            table.validate(data, entries)

    def test_compact_report_fixtures_retain_every_measured_export(self):
        from ps4_offsets import parse_report
        for release in ('6.72', '9.00'):
            path = ROOT / 'tests' / 'fixtures' / 'ps4' / release / 'offsets.report'
            _, _, parsed, _ = parse_report(path.read_text(encoding='utf-8'))
            for label, value in self.firmwares[release]['avcap']['exports'].items():
                self.assertEqual(parsed['avcap_' + label], int(value,16))

    def test_900_avcap_values(self):
        avcap = self.firmwares['9.00']['avcap']
        self.assertEqual(table.as_hex(avcap['init'], 'x'), 0x940)
        self.assertEqual(table.as_hex(avcap['trap'], 'x'), 0xBE)
        self.assertEqual(table.as_hex(avcap['module_size'], 'x'), 0x1C000)
        exports = {key: table.as_hex(value, key) for key, value in
                   avcap['exports'].items()}
        self.assertEqual(exports['read_video'], 0x14A0)
        self.assertEqual(exports['stop'], 0x1700)
        self.assertEqual(exports['close'], 0x1730)
        self.assertIsNone(avcap['ready_delta'])

    def test_1100_matches_legacy_profile(self):
        avcap = self.firmwares['11.00']['avcap']
        self.assertEqual(table.as_hex(avcap['init'], 'x'), 0x940)
        self.assertEqual(table.as_hex(avcap['trap'], 'x'), 0xBE)
        self.assertEqual(table.as_hex(avcap['module_size'], 'x'), 0x1C000)
        self.assertEqual(table.as_hex(avcap['ready_delta'], 'x'), 0x18E0 - 0x940)
        self.assertEqual(table.as_hex(avcap['dequeue_delta'], 'x'), 0x1C10 - 0x940)
        self.assertEqual(table.as_hex(avcap['consume_delta'], 'x'), 0x1DB0 - 0x940)
        capture = self.firmwares['11.00']['capture']
        self.assertEqual(table.as_hex(capture['latch_site']['offset'], 'x'), 0x9D9C)

    def test_1100_exports_come_from_the_dump(self):
        avcap = self.firmwares['11.00']['avcap']
        self.assertEqual(self.firmwares['11.00']['measured_on'], '0x11008001')
        self.assertEqual(table.as_hex(avcap['exports']['read_video'], 'x'), 0x14A0)
        self.assertEqual(table.as_hex(avcap['exports']['close'], 'x'), 0x1730)
        # Same table as 9.00: the exported Avcap ABI did not change.
        self.assertEqual(avcap['exports'],
                         self.firmwares['9.00']['avcap']['exports'])

    def test_library_exports_are_per_firmware(self):
        nine = self.firmwares['9.00']['libraries']
        eleven = self.firmwares['11.00']['libraries']
        self.assertEqual(table.as_hex(nine['libc_malloc']['rva'], 'x'), 0x51220)
        self.assertEqual(table.as_hex(eleven['libc_malloc']['rva'], 'x'), 0x28D60)
        self.assertEqual(table.as_hex(nine['kernel_usleep']['rva'], 'x'), 0xF290)
        self.assertEqual(table.as_hex(eleven['kernel_usleep']['rva'], 'x'), 0xEFD0)
        # getpid is the one export that happens to match.
        self.assertEqual(nine['kernel_getpid']['rva'], eleven['kernel_getpid']['rva'])
        self.assertEqual(eleven['libc_malloc']['module'], 'libSceLibcInternal.sprx')

    def test_kernel_offsets_differ_between_releases(self):
        nine = self.firmwares['9.00']['kernel']
        eleven = self.firmwares['11.00']['kernel']
        self.assertNotEqual(nine['acmgr_offset'], eleven['acmgr_offset'])
        self.assertNotEqual(nine['gates'][0]['offset'], eleven['gates'][0]['offset'])

    def test_delta_text(self):
        self.assertEqual(table.delta_text('0x940', '0x940'), 'same')
        self.assertEqual(table.delta_text('0x940', '0xf00'), '+0x5c0')
        self.assertEqual(table.delta_text('0x940', None), 'unmeasured')
        self.assertEqual(table.delta_text([1, 2], [1, 3]), 'changed')

    def test_report_matches_table(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'dump.txt'
            path.write_text(REPORT, encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()) as output:
                failures = table.report_check(self.data, self.firmwares,
                                              path, '9.00')
            self.assertEqual(failures, 0)
            self.assertIn('symbol.avcap_read_video', output.getvalue())
            self.assertNotIn('FAIL', output.getvalue())

    def test_report_mismatch_is_reported(self):
        tampered = REPORT.replace('rva=0x14a0', 'rva=0x14b0')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'dump.txt'
            path.write_text(tampered, encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()) as output:
                failures = table.report_check(self.data, self.firmwares,
                                              path, '9.00')
            self.assertEqual(failures, 1)
            self.assertIn('FAIL symbol.avcap_read_video', output.getvalue())

    def test_report_with_wrong_firmware(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'dump.txt'
            path.write_text(REPORT, encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()) as output:
                failures = table.report_check(self.data, self.firmwares,
                                              path, '11.00')
            # Wrong release and wrong libc export: both are per firmware.
            self.assertEqual(failures, 2)
            self.assertIn('FAIL firmware', output.getvalue())
            self.assertIn('FAIL symbol.libc_malloc', output.getvalue())

    def test_1100_report_replays_clean(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'dump.txt'
            path.write_text(REPORT_1100, encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()) as output:
                failures = table.report_check(self.data, self.firmwares,
                                              path, '11.00')
            self.assertEqual(failures, 0, output.getvalue())
            self.assertIn('symbol.libc_malloc', output.getvalue())
            self.assertIn('OK   symbol.kernel_usleep', output.getvalue())

    def test_1100_report_does_not_pass_as_900(self):
        # The library RVAs differ per firmware; replaying with the wrong release
        # must fail on both the release and the library exports.
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'dump.txt'
            path.write_text(REPORT_1100, encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()) as output:
                failures = table.report_check(self.data, self.firmwares,
                                              path, '9.00')
            self.assertGreaterEqual(failures, 2)
            self.assertIn('FAIL symbol.libc_malloc', output.getvalue())


class CommandLineTest(unittest.TestCase):
    def run_tool(self, *arguments):
        return subprocess.run(
            [sys.executable, str(ROOT / 'tools' / 'ps4_firmware_table.py')]
            + list(arguments), capture_output=True, text=True, check=False)

    def test_check_succeeds(self):
        done = self.run_tool('--check')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('3 firmware(s) valid', done.stdout)

    def test_compare_shows_differences(self):
        done = self.run_tool('--compare', '9.00', '11.00')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('kernel.acmgr_offset', done.stdout)
        self.assertIn('different', done.stdout)
        self.assertIn('avcap.trap', done.stdout)

    def test_initializer_marks_unmeasured_fields(self):
        done = self.run_tool('--c', '9.00')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('.acmgr_offset = 0x8bc20,', done.stdout)
        self.assertIn('.avcap_init_offset = 0x940,', done.stdout)
        self.assertIn('TODO ready_delta', done.stdout)
        self.assertIn('TODO gate 0', done.stdout)

    def test_markdown_has_both_sections(self):
        done = self.run_tool('--markdown')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('### 9.00', done.stdout)
        self.assertIn('## Differences', done.stdout)

    def test_unknown_firmware_is_rejected(self):
        done = self.run_tool('--c', '12.00')
        self.assertNotEqual(done.returncode, 0)
        self.assertIn('unknown firmware', done.stderr)
        self.assertNotIn('Traceback', done.stderr)

    def test_version_selects_the_same_entry(self):
        by_release = self.run_tool('--c', '9.00')
        by_version = self.run_tool('--c', '0x09008031')
        self.assertEqual(by_version.returncode, 0, by_version.stderr)
        self.assertEqual(by_version.stdout, by_release.stdout)

    def test_broken_table_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'table.json'
            data = json.loads((ROOT / 'tools' / 'data' /
                               'ps4_firmware_offsets.json').read_text(encoding='utf-8'))
            data['firmwares'][0]['kernel']['gates'][0]['offset'] = 'nonsense'
            path.write_text(json.dumps(data), encoding='utf-8')
            done = self.run_tool('--table', str(path), '--check')
            self.assertNotEqual(done.returncode, 0)
            self.assertIn('expected a 0x-prefixed hex string', done.stderr)


if __name__ == '__main__':
    unittest.main()
