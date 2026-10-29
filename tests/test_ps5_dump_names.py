import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import generate_ps5_offsets as gen  # noqa: E402


class ArchiveNameTest(unittest.TestCase):
    def test_version_names_pin_major_minor(self):
        self.assertEqual(gen.archive_release("fw13.40.rar"), (0x13400000, 0xFFFF0000))
        self.assertEqual(gen.archive_release("fw11.20.zip"), (0x11200000, 0xFFFF0000))
        self.assertEqual(gen.archive_release("FW-8.40.zip"), (0x08400000, 0xFFFF0000))
        self.assertEqual(gen.archive_release("fw9.40.zip"), (0x09400000, 0xFFFF0000))
        self.assertEqual(gen.archive_release("pbs-dump-fw13.00.zip"), (0x13000000, 0xFFFF0000))

    def test_exact_ids_win_over_versions(self):
        self.assertEqual(gen.archive_release("ps5-13420000-1793122.tar"), (0x13420000, 0xFFFFFFFF))

    def test_names_without_a_release_are_ignored(self):
        for name in ("dump.tar", "fw1140_ps5pro-PBS.zip", "fw12.0.zip", "pbs-dump.zip"):
            self.assertIsNone(gen.archive_release(name), name)

    def test_a_13_00_dump_in_a_13_40_archive_is_a_mismatch(self):
        prefix, mask = gen.archive_release("fw13.40.rar")
        self.assertNotEqual(0x13000040 & mask, prefix)
        self.assertEqual(gen.release_label(0x13000040), "13.00")
        self.assertEqual(gen.release_label(0x11200005), "11.20")

    def test_matching_names_are_not_flagged(self):
        for name, firmware in (("fw11.20.zip", 0x11200005), ("fw13.20.zip", 0x13200006),
                               ("FW-8.40.zip", 0x08400005), ("fw4.00.7z", 0x04000042)):
            prefix, mask = gen.archive_release(name)
            self.assertEqual(firmware & mask, prefix, name)


if __name__ == "__main__":
    unittest.main()
