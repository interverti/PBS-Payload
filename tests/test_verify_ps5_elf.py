"""Offline regressions for the release build's payload ELF check."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from verify_ps5_elf import verify


def fixture(abi=0):
    data = bytearray(0x101)
    data[:8] = b"\x7fELF\x02\x01\x01" + bytes([abi])
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 3, 62, 1, 0x4000, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0x100, 0x4000, 0, 1, 1, 0x4000)
    data[-1] = 0xc3
    return data


class VerifyPS5ELFTests(unittest.TestCase):
    def test_both_sdk_brandings_are_accepted_without_modification(self):
        for abi in (0, 9):
            data = fixture(abi)
            original = bytes(data)
            self.assertEqual(verify(data), abi)
            self.assertEqual(data, original)

    def test_unknown_abi_rejected(self):
        with self.assertRaisesRegex(ValueError, "OS/ABI"):
            verify(fixture(3))

    def test_wrong_machine_or_relocatable_object_rejected(self):
        for offset, value in ((18, 183), (16, 1)):
            data = fixture()
            struct.pack_into("<H", data, offset, value)
            with self.assertRaisesRegex(ValueError, "x86-64"):
                verify(data)

    def test_host_interpreter_rejected(self):
        data = fixture()
        struct.pack_into("<I", data, 64, 3)
        with self.assertRaisesRegex(ValueError, "PT_INTERP"):
            verify(data)

    def test_bad_header_and_segment_extents_rejected(self):
        for data in (fixture()[:20], fixture()[:70], fixture()[:-1]):
            with self.assertRaises(ValueError):
                verify(data)

    def test_entry_outside_executable_segment_rejected(self):
        data = fixture()
        struct.pack_into("<Q", data, 24, 0x8000)
        with self.assertRaisesRegex(ValueError, "entry point"):
            verify(data)


if __name__ == "__main__":
    unittest.main()
