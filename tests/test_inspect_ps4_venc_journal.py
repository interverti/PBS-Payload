# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('journal', Path(__file__).resolve().parents[1] / 'tools/inspect_ps4_venc_journal.py')
journal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(journal)


class JournalTests(unittest.TestCase):
    def blob(self, count):
        blob = bytearray(b'PBSVJNL1' + struct.pack('<5Q', 0x100000, 42, 9, 0x200000, journal.DATA_SIZE))
        blob.extend(bytes(journal.DATA_SIZE))
        struct.pack_into('<I', blob, 48, count)
        return blob

    def test_wrap_order_and_uncommitted(self):
        blob = self.blob(70)
        for ticket in range(7, 71):
            struct.pack_into('<8Q', blob, 56 + ((ticket - 1) % 64) * 64,
                             ticket, 0x100010, ticket, 2, 3, 4, 5, 0)
        struct.pack_into('<Q', blob, 56 + 5 * 64, 0)  # In-flight ticket70.
        result = journal.decode(blob)
        self.assertEqual([r['ticket'] for r in result['records']], list(range(7, 70)))
        self.assertEqual(result['records'][0]['args'][1], '0x7')
        self.assertEqual(result['records'][0]['strings'], [None] * 6)

    def test_reject_truncated_header_and_size(self):
        blob = self.blob(0)
        for bad in (blob[:-1], blob + b'0', b'XXXXXXXX' + blob[8:]):
            with self.assertRaises(ValueError):
                journal.decode(bad)
        struct.pack_into('<Q', blob, 40, 1)
        with self.assertRaises(ValueError):
            journal.decode(blob)

    def test_string_resolution_never_formats(self):
        class Elf:
            ph = [(1, 4, 0, 0x10, 0, 5)]
            def code(self, address, size):
                return b'%n%x\0'[:size]
        blob = self.blob(1)
        struct.pack_into('<8Q', blob, 56, 1, 0x100010, 0, 0, 0, 0, 0, 0)
        self.assertEqual(journal.decode(blob, Elf())['records'][0]['strings'][0], '%n%x')


if __name__ == '__main__':
    unittest.main()
