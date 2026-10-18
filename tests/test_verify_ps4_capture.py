#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Integration tests for real H.264 decoding and visibility classification."""

import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "verify_ps4_capture.py"
ARCHIVED_BLACK = ROOT / ".local-research" / "ps4" / "smoke.h264"


def load_verifier():
    spec = importlib.util.spec_from_file_location("verify_ps4_capture", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def invoke(path, *options):
    completed = subprocess.run([sys.executable, str(SCRIPT), str(path), *options],
                               capture_output=True, text=True, timeout=45)
    return completed.returncode, json.loads(completed.stdout), completed.stderr


class PathAndFtpTests(unittest.TestCase):
    def test_output_restricted_to_ignored_local_research(self):
        verifier = load_verifier()
        with self.assertRaises(verifier.VerificationError):
            verifier.output_path("tools/unwanted.h264")
        with self.assertRaises(verifier.VerificationError):
            verifier.output_path(".local-research/../tools/unwanted.h264")
        expected = ROOT / ".local-research" / "ps4" / "download.h264"
        self.assertEqual(verifier.output_path(".local-research/ps4/download.h264"), expected)

    def test_ftp_uses_read_only_retr_and_never_writes_partial_target(self):
        verifier = load_verifier()
        events = []
        content = b"\x00\x00\x00\x01\x67PS4"

        class FakeFTP:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return None

            def connect(self, host, port, timeout):
                events.append(("connect", host, port, timeout))

            def login(self, user, password):
                events.append(("login", user, password))

            def retrbinary(self, command, callback, blocksize):
                events.append(("retrieve", command))
                callback(content)

        with tempfile.TemporaryDirectory() as folder, patch.object(verifier.ftplib, "FTP", FakeFTP):
            target = Path(folder) / "smoke.h264"
            verifier.ftp_download("127.0.0.1", 2121, target, 5)
            self.assertEqual(target.read_bytes(), content)
            with self.assertRaises(verifier.VerificationError):
                verifier.ftp_download("127.0.0.1", 2121, target, 5)
        self.assertEqual([event[0] for event in events], ["connect", "login", "retrieve"])
        self.assertEqual(events[-1], ("retrieve", "RETR /data/pbs/ps4/smoke.h264"))

    def test_partial_ftp_failure_does_not_leave_download(self):
        verifier = load_verifier()

        class BrokenFTP:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return None

            def connect(self, *_args, **_kwargs):
                pass

            def login(self, *_args):
                pass

            def retrbinary(self, _command, callback, **_kwargs):
                callback(b"partial")
                raise EOFError("FTP interrupted")

        with tempfile.TemporaryDirectory() as folder, patch.object(verifier.ftplib, "FTP", BrokenFTP):
            target = Path(folder) / "smoke.h264"
            with self.assertRaises(verifier.VerificationError):
                verifier.ftp_download("127.0.0.1", 2121, target, 5)
            self.assertFalse(target.exists())
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_oversized_ftp_response_is_rejected_without_output(self):
        verifier = load_verifier()

        class OversizedFTP:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return None

            def connect(self, *_args, **_kwargs):
                pass

            def login(self, *_args):
                pass

            def retrbinary(self, _command, callback, **_kwargs):
                callback(b"12345")

        with (tempfile.TemporaryDirectory() as folder,
              patch.object(verifier.ftplib, "FTP", OversizedFTP),
              patch.object(verifier, "MAX_FTP_BYTES", 4)):
            target = Path(folder) / "smoke.h264"
            with self.assertRaisesRegex(verifier.VerificationError, "exceeds 64 MiB"):
                verifier.ftp_download("127.0.0.1", 2121, target, 5)
            self.assertFalse(target.exists())
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_mostly_black_video_fails_even_with_one_visible_frame(self):
        verifier = load_verifier()
        black = bytes(verifier.FRAME_BYTES)
        red = bytes((255, 0, 0)) * (verifier.FRAME_BYTES // 3)
        statistics = verifier.classify_frames(black * 39 + red)
        self.assertEqual(statistics["decoded_frames"], 40)
        self.assertEqual(statistics["nonblack_frames"], 1)
        self.assertTrue(statistics["predominantly_black"])


@unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"),
                     "Requires ffmpeg and ffprobe on PATH")
class DecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.root = Path(cls.directory.name)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def encode(self, name, source, frames=45):
        output = self.root / name
        command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                   "-f", "lavfi", "-i", source, "-frames:v", str(frames),
                   "-an", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                   "-f", "h264", str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if result.returncode:
            self.skipTest("FFmpeg does not support the test H.264 encoder: " + result.stderr[-500:])
        return output

    def test_black_h264_is_decodable_but_rejected(self):
        path = self.encode("black.h264", "color=c=black:s=160x96:r=30")
        code, result, _ = invoke(path)
        self.assertEqual(code, 1)
        self.assertFalse(result["ok"])
        self.assertEqual(result["verdict"], "fail")
        self.assertEqual(result["video"]["codec"], "h264")
        self.assertEqual(result["analysis"]["decoded_frames"], 45)
        self.assertEqual(result["analysis"]["black_frames"], 45)
        self.assertEqual(result["analysis"]["mean_color_channel_range"], 0)
        self.assertTrue(any("nearly black" in item for item in result["errors"]))

    def test_low_level_encoder_noise_is_still_black(self):
        path = self.encode("nearblack.h264", "color=c=0x0b0b0b:s=160x96:r=30")
        code, result, _ = invoke(path)
        self.assertEqual(code, 1)
        self.assertTrue(result["analysis"]["predominantly_black"])
        self.assertLess(result["analysis"]["mean_luma"], 20)

    def test_bright_static_video_is_visible_not_black(self):
        path = self.encode("red.h264", "color=c=red:s=160x96:r=30")
        code, result, _ = invoke(path)
        self.assertEqual(code, 0, result)
        self.assertEqual(result["analysis"]["black_frames"], 0)
        self.assertEqual(result["analysis"]["decoded_frames"], 45)
        self.assertGreater(result["analysis"]["mean_rgb"][0], 150)
        self.assertGreater(result["analysis"]["mean_color_channel_range"], 100)
        self.assertGreater(result["analysis"]["bright_pixel_fraction"], 0.9)
        self.assertLess(result["analysis"]["mean_temporal_luma_delta"], 1)

    def test_changing_scene_has_spatial_and_temporal_variation(self):
        path = self.encode("testsrc.h264", "testsrc2=size=160x96:rate=30")
        code, result, _ = invoke(path)
        self.assertEqual(code, 0, result)
        self.assertEqual(result["analysis"]["decoded_frames"], 45)
        self.assertGreater(result["analysis"]["mean_spatial_luma_stddev"], 10)
        self.assertGreater(result["analysis"]["mean_temporal_luma_delta"], 0.5)
        self.assertGreater(result["analysis"]["mean_color_channel_range"], 10)

    def test_short_visible_video_fails_minimum_frame_count(self):
        path = self.encode("short.h264", "color=c=red:s=160x96:r=30", frames=5)
        code, result, _ = invoke(path)
        self.assertEqual(code, 1)
        self.assertEqual(result["analysis"]["decoded_frames"], 5)
        self.assertFalse(result["analysis"]["predominantly_black"])
        self.assertTrue(any("Only 5 frames" in item for item in result["errors"]))

    def test_corrupt_h264_is_failed_media_not_success(self):
        path = self.root / "invalid.h264"
        path.write_bytes(b"not a valid H264 elementary stream")
        code, result, _ = invoke(path)
        self.assertEqual(code, 1)
        self.assertFalse(result["ok"])
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(result["errors"])

    def test_missing_decoder_is_distinct_tool_error(self):
        path = self.encode("missing-ffmpeg.h264", "color=c=red:s=160x96:r=30")
        code, result, _ = invoke(path, "--ffmpeg", "definitely_no_ffmpeg_executable_6472")
        self.assertEqual(code, 2)
        self.assertEqual(result["verdict"], "error")
        self.assertTrue(any("Cannot execute" in item for item in result["errors"]))

    @unittest.skipUnless(ARCHIVED_BLACK.is_file(), "Archived PS4 recording not present")
    def test_archived_ps4_capture_has_black_frames(self):
        code, result, _ = invoke(ARCHIVED_BLACK)
        self.assertEqual(code, 1)
        self.assertEqual(result["video"]["width"], 1280)
        self.assertEqual(result["video"]["height"], 720)
        self.assertGreaterEqual(result["analysis"]["decoded_frames"], 30)
        self.assertEqual(result["analysis"]["black_frame_fraction"], 1.0)
        self.assertTrue(result["analysis"]["predominantly_black"])


if __name__ == "__main__":
    unittest.main()
