#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline checks for the release tooling: no network, no console, no SDK."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


store = load("store_release", "store-release.py")
version = load("release_version", "release-version.py")


def write_artifact(directory: Path, name: str, payload: bytes = b"\x7fELF payload") -> Path:
    path = directory / name
    path.write_bytes(payload)
    return path


class ArtifactTest(unittest.TestCase):
    def test_names_follow_the_version_and_target(self):
        self.assertEqual(store.artifact_name("0.2.0", "ps5"), "PBS-Payload-0.2.0-ps5.elf")
        self.assertEqual(store.artifact_name("0.2.0-beta.1", "ps4"), "PBS-Payload-0.2.0-beta.1-ps4.elf")

    def test_collect_reads_one_file_per_selected_platform(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            first = write_artifact(directory, "PBS-Payload-0.1.0-ps5.elf", b"ps5 bytes")
            second = write_artifact(directory, "PBS-Payload-0.1.0-ps4.elf", b"ps4 bytes here")
            write_artifact(directory, "PBS-Payload-0.1.0-ps5.elf.sha256", b"orphan sidecar")
            write_artifact(directory, "SHA256SUMS.txt", b"manifest")
            files = store.collect(str(directory), "0.1.0", ("ps4", "ps5"))
            self.assertEqual([item["filename"] for item in files],
                             ["PBS-Payload-0.1.0-ps4.elf", "PBS-Payload-0.1.0-ps5.elf"])
            self.assertEqual([item["target"] for item in files], ["ps4", "ps5"])
            self.assertEqual({item["filename"]: item["path"] for item in files},
                             {"PBS-Payload-0.1.0-ps4.elf": second, "PBS-Payload-0.1.0-ps5.elf": first})
            by_name = {item["filename"]: item for item in files}
            self.assertEqual(by_name["PBS-Payload-0.1.0-ps5.elf"]["sha256"],
                             hashlib.sha256(b"ps5 bytes").hexdigest())
            self.assertEqual(by_name["PBS-Payload-0.1.0-ps4.elf"]["size_bytes"], len(b"ps4 bytes here"))

    def test_collect_only_needs_the_selected_platforms(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            write_artifact(directory, "PBS-Payload-0.1.0-ps5.elf")
            files = store.collect(str(directory), "0.1.0", ("ps5",))
            self.assertEqual(len(files), 1)

    def test_collect_reports_a_missing_artifact_with_the_build_command(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            write_artifact(directory, "PBS-Payload-0.1.0-ps5.elf")
            with self.assertRaises(store.ReleaseError) as caught:
                store.collect(str(directory), "0.1.0", ("ps4", "ps5"))
            message = str(caught.exception)
            self.assertIn("PBS-Payload-0.1.0-ps4.elf", message)
            self.assertIn("payload/build.sh ps4 stream", message)

    def test_collect_refuses_artifacts_of_another_version(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            write_artifact(directory, "PBS-Payload-0.1.0-ps5.elf")
            write_artifact(directory, "PBS-Payload-0.2.0-ps5.elf")
            with self.assertRaises(store.ReleaseError) as caught:
                store.collect(str(directory), "0.2.0", ("ps5",))
            self.assertIn("PBS-Payload-0.1.0-ps5.elf", str(caught.exception))

    def test_collect_rejects_an_empty_artifact(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            write_artifact(directory, "PBS-Payload-0.1.0-ps5.elf", b"")
            with self.assertRaises(store.ReleaseError):
                store.collect(str(directory), "0.1.0", ("ps5",))

    def test_collect_requires_a_directory(self):
        with tempfile.TemporaryDirectory() as raw:
            with self.assertRaises(store.ReleaseError):
                store.collect(str(Path(raw) / "absent"), "0.1.0", ("ps5",))

    def test_filename_grammar_matches_the_store(self):
        self.assertTrue(store.FILENAME.fullmatch("PBS-Payload-0.1.0-ps5.elf"))
        self.assertTrue(store.FILENAME.fullmatch("PBS-Payload 0.1.0+ps5.elf"))
        self.assertFalse(store.FILENAME.fullmatch("PBS-Payload-0.1.0-ps5(1).elf"))
        self.assertFalse(store.FILENAME.fullmatch("-PBS-Payload-0.1.0-ps5.elf"))
        self.assertFalse(store.FILENAME.fullmatch("PBS-Payload-0.1.0-ps5.elf " + "x" * 200))


class ReleaseRulesTest(unittest.TestCase):
    def test_checked_version_and_channel(self):
        good = [{"filename": "PBS-Payload-0.2.0-ps5.elf"}]
        store.check_release("0.2.0", "stable", good)
        store.check_release("0.2.0-beta.1", "beta", good)
        store.check_release("0.2.0-nightly.7", "nightly", good)

    def test_prerelease_cannot_use_the_stable_channel(self):
        with self.assertRaises(store.ReleaseError):
            store.check_release("0.2.0-beta.1", "stable", [])

    def test_bad_versions_and_channels(self):
        for candidate in ("0.2", "02.0.0", "v0.2.0", "0.2.0-", "latest"):
            with self.assertRaises(store.ReleaseError, msg=candidate):
                store.check_release(candidate, "beta", [])
        with self.assertRaises(store.ReleaseError):
            store.check_release("0.2.0", "release", [])

    def test_targets_are_parsed_leniently(self):
        self.assertEqual(store.parse_targets("ps5"), ("ps5",))
        self.assertEqual(store.parse_targets("PS4, ps5"), ("ps4", "ps5"))
        self.assertEqual(store.parse_targets("ps5,ps4,ps5"), ("ps5", "ps4"))
        with self.assertRaises(store.ReleaseError):
            store.parse_targets("ps3")
        with self.assertRaises(store.ReleaseError):
            store.parse_targets(" , ")

    def test_console_platform_fields(self):
        self.assertEqual(store.platform_of("ps5"),
                         {"platform": "console", "operating_system": "console", "architecture": "x64"})
        self.assertEqual(store.platform_of("ps4"), store.platform_of("ps5"))
        with self.assertRaises(store.ReleaseError):
            store.platform_of("ps3")


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.files = [
            {"filename": "PBS-Payload-0.2.0-ps4.elf", "target": "ps4", "size_bytes": 11,
             "sha256": "b" * 64, "path": Path("/nonexistent")},
            {"filename": "PBS-Payload-0.2.0-ps5.elf", "target": "ps5", "size_bytes": 22,
             "sha256": "a" * 64, "path": Path("/nonexistent")},
        ]

    def test_presign_asks_only_for_filename_and_size(self):
        body = store.presign_body(self.files, "seregonwar/PBS-Payload", "0.2.0")
        self.assertEqual(body["action"], "presign")
        self.assertEqual(body["version"], "0.2.0")
        self.assertEqual(body["files"][0], {"filename": "PBS-Payload-0.2.0-ps4.elf", "size_bytes": 11})

    def test_register_manifest_matches_the_store_schema(self):
        paths = store.storage_paths_for(self.files, "pbs-payload", "0.2.0")
        body = store.register_body(self.files, "seregonwar/PBS-Payload", "0.2.0", "beta", paths,
                                   "notes", publish=False, run_id="12345")
        self.assertEqual(body["action"], "register")
        self.assertEqual(body["channel"], "beta")
        self.assertIs(body["is_published"], False)
        self.assertEqual(body["artifact_name"], "PBS-Payload-0.2.0")
        self.assertEqual(body["workflow_run_id"], "12345")
        self.assertEqual(body["release_notes"], "notes")
        entry = body["files"][0]
        self.assertEqual(entry["storage_path"], "pbs-payload/0.2.0/PBS-Payload-0.2.0-ps4.elf")
        self.assertEqual(entry["storage_provider"], "r2")
        self.assertEqual(entry["platform"], "console")
        self.assertEqual(entry["operating_system"], "console")
        self.assertEqual(entry["architecture"], "x64")
        self.assertEqual(entry["sha256"], "b" * 64)
        self.assertEqual(entry["size_bytes"], 11)

    def test_register_without_notes_or_run(self):
        paths = store.storage_paths_for(self.files, "pbs-payload", "0.2.0")
        body = store.register_body(self.files, "seregonwar/PBS-Payload", "0.2.0", "stable", paths)
        self.assertNotIn("release_notes", body)
        self.assertNotIn("workflow_run_id", body)
        self.assertIs(body["is_published"], False)

    def test_notes_are_capped_like_the_store(self):
        paths = store.storage_paths_for(self.files, "pbs-payload", "0.2.0")
        body = store.register_body(self.files, "repo", "0.2.0", "beta", paths, "x" * 50000)
        self.assertEqual(len(body["release_notes"]), store.MAX_NOTES)

    def test_publish_flips_is_published(self):
        paths = store.storage_paths_for(self.files, "pbs-payload", "0.2.0")
        body = store.register_body(self.files, "repo", "0.2.0", "beta", paths, publish=True)
        self.assertIs(body["is_published"], True)


class CommandLineTest(unittest.TestCase):
    def run_main(self, argv):
        output, error = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
            code = store.main(argv)
        return code, output.getvalue(), error.getvalue()

    def test_dry_run_prints_both_requests_without_a_token(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            write_artifact(directory, "PBS-Payload-0.3.0-ps5.elf", b"x" * 128)
            code, output, error = self.run_main(["--dir", str(directory), "--version", "0.3.0",
                                                "--channel", "stable", "--platforms", "ps5", "--dry-run"])
            self.assertEqual(code, 0, error)
            self.assertIn('"action": "presign"', output)
            self.assertIn('"action": "register"', output)
            self.assertIn('"storage_path": "pbs-payload/0.3.0/PBS-Payload-0.3.0-ps5.elf"', output)
            self.assertIn('"is_published": false', output)
            self.assertEqual(error, "")

    def test_a_missing_artifact_fails_before_any_request(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            write_artifact(directory, "PBS-Payload-0.3.0-ps5.elf", b"x")
            code, output, error = self.run_main(["--dir", str(directory), "--version", "0.3.0",
                                                "--channel", "stable", "--platforms", "ps4,ps5", "--dry-run"])
            self.assertEqual(code, 1)
            self.assertIn("PBS-Payload-0.3.0-ps4.elf", error)
            self.assertEqual(output, "")

    def test_a_missing_token_is_reported(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            write_artifact(directory, "PBS-Payload-0.3.0-ps5.elf", b"x")
            token = store.os.environ.pop(store.TOKEN_ENV, None)
            try:
                code, _output, error = self.run_main(["--dir", str(directory), "--version", "0.3.0",
                                                     "--channel", "stable", "--platforms", "ps5"])
            finally:
                if token is not None:
                    store.os.environ[store.TOKEN_ENV] = token
            self.assertEqual(code, 1)
            self.assertIn(store.TOKEN_ENV, error)


class ReleaseVersionTest(unittest.TestCase):
    def test_the_repository_version_is_numeric(self):
        self.assertRegex(version.project_version(), r"^\d+\.\d+\.\d+$")

    def test_resolve_accepts_the_repository_version(self):
        number = version.project_version()
        info = version.resolve(f"v{number}")
        self.assertEqual(info["version"], number)
        self.assertEqual(info["tag"], f"v{number}")
        self.assertEqual(info["numeric"], number)
        self.assertEqual(info["prerelease"], "false")
        self.assertEqual(info["channel"], "stable")
        self.assertEqual(info["release_name"], f"PBS Payload v{number}")

    def test_resolve_keeps_prereleases_off_the_numeric_version(self):
        number = version.project_version()
        info = version.resolve(f"{number}-beta.2")
        self.assertEqual(info["numeric"], number)
        self.assertEqual(info["prerelease"], "true")
        self.assertEqual(info["channel"], "beta")
        self.assertEqual(version.resolve(f"{number}-nightly.9")["channel"], "nightly")

    def test_resolve_rejects_another_numeric_version(self):
        with self.assertRaises(ValueError) as caught:
            version.resolve("v99.88.77")
        self.assertIn("VERSION", str(caught.exception))

    def test_set_writes_a_numeric_version_only(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "VERSION"
            path.write_text("1.0.0\n", encoding="utf-8")
            version.set_project_version("1.1.0", path)
            self.assertEqual(path.read_text(encoding="utf-8"), "1.1.0\n")
            self.assertEqual(version.project_version(path), "1.1.0")
            with self.assertRaises(ValueError):
                version.set_project_version("1.1.0-beta.1", path)

    def test_project_version_rejects_a_dirty_file(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "VERSION"
            path.write_text("v1.0.0\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                version.project_version(path)
            path.write_text("1.0.0-beta.1\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                version.project_version(path)


if __name__ == "__main__":
    unittest.main()
