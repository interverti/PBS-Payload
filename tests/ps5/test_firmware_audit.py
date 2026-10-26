"""The audit groups observed geometry, never silently expands runtime support."""
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import audit_ps5_firmware as audit


class FirmwareAuditTests(unittest.TestCase):
    def test_geometry_excludes_code_locations_but_keeps_sizes(self):
        a = dict(size=0x2e0, kind=0x1a0, width=0x208, constructor=0x56fc8,
                 open_video=0x360, ipc_command=0x4487)
        b = a | dict(constructor=0x58168)
        self.assertEqual(audit.geometry(a), audit.geometry(b))
        self.assertNotEqual(audit.geometry(a), audit.geometry(b | dict(size=0x2e8)))
        self.assertNotEqual(audit.geometry(a), audit.geometry(b | dict(kind=0x1a8)))

    def test_default_mask_span_normalized_without_hiding_legacy_tail(self):
        self.assertEqual(audit.geometry({}), audit.geometry(dict(mask_span=0x24)))
        self.assertNotEqual(audit.geometry({}), audit.geometry(dict(mask_span=0x14)))

    def test_observed_span_does_not_claim_missing_versions(self):
        def record(version, size):
            geometry = dict(size=size)
            return dict(version=version, geometry=geometry,
                        geometry_sha256=hashlib.sha256(json.dumps(geometry, sort_keys=True).encode()).hexdigest())
        groups = audit.families([record("9.20", 0x2b0), record("9.40", 0x2b0),
                                 record("10.01", 0x2b8), record("10.60", 0x2b8)])
        self.assertEqual(len(groups), 2)
        self.assertEqual(groups[0]["observed_span"], ["9.20", "9.40"])
        self.assertFalse(groups[0]["intermediate_releases_verified"])
        self.assertNotIn("9.00", groups[0]["observed_versions"])

    def test_equal_geometry_after_a_different_family_stays_separate(self):
        records = [dict(version=str(i), geometry={}, geometry_sha256=s)
                   for i, s in enumerate(("a", "b", "a"))]
        self.assertEqual(len(audit.families(records)), 3)

    @staticmethod
    def elf():
        data = bytearray(0x104)
        data[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<Q", data, 32, 64)
        struct.pack_into("<HH", data, 54, 56, 1)
        struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0x100, 0x340, 0, 4, 4, 0x1000)
        data[0x100:] = b"CODE"
        return data

    def test_plaintext_elf_segment_hash_and_mapping(self):
        memory, segments = audit.elf_image(self.elf())
        self.assertEqual(memory[0x340:0x344], b"CODE")
        self.assertEqual(segments, [dict(rva=0x340, size=4, sha256=audit.sha(b"CODE"))])

    def test_self_and_truncated_load_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "plaintext"):
            audit.elf_image(b"\x54\x14\xf5\xee" + bytes(256))
        data = self.elf()
        struct.pack_into("<Q", data, 64 + 32, 1000)
        with self.assertRaisesRegex(ValueError, "LOAD"):
            audit.elf_image(data)

    def test_real_dumps_when_provided(self):
        root = os.environ.get("PBS_PS5_DUMPS")
        if not root:
            self.skipTest("set PBS_PS5_DUMPS to include private firmware dumps")
        gathered, inventory = audit.collect(Path(root), [], None,
            os.environ.get("PBS_SEVEN_ZIP", r"C:\Program Files\7-Zip\7z.exe"))
        records = {v["version"]: v for v in audit.analyze(gathered)}
        self.assertGreaterEqual(len(inventory), 17)
        self.assertNotIn("9.00", records)
        self.assertEqual(records["11.40"]["geometry"], records["12.70"]["geometry"])
        self.assertNotEqual(records["12.70"]["geometry"], records["13.60"]["geometry"])


if __name__ == "__main__":
    unittest.main()
