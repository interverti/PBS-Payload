"""Offline SDK installation regression: fixture repositories, no downloads."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INSTALL = ROOT / ".github/actions/payload-sdk/install-sdk.sh"


@unittest.skipIf(os.name == "nt", "run with Linux/WSL bash, make and git")
class InstallSDKTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pbs sdk test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.job = self.root / "job"
        self.job.mkdir()
        self.repo = self.root / "sdk-source"
        self.repo.mkdir()
        self.bin = self.root / "tools"
        self.bin.mkdir()
        # Avoid package-manager calls; these fixtures don't compile C.
        for name in ("llvm-config", "clang", "ld.lld", "curl", "wget"):
            p = self.bin / name
            p.write_text("#!/bin/sh\nexit 0\n")
            p.chmod(0o755)

    def commit(self):
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        subprocess.run(["git", "-C", str(self.repo), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.repo), "-c", "user.name=Fixture",
                        "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"], check=True)

    def run_installer(self, platform, **extra):
        env = os.environ.copy()
        for name in ("PS4_PAYLOAD_SDK", "PS5_PAYLOAD_SDK", "SDK_URL", "SDK_REF"):
            env.pop(name, None)
        env.update(PLATFORM=platform, SDK_REPO=str(self.repo),
                   GITHUB_ENV=str(self.root / "env"), GITHUB_OUTPUT=str(self.root / "output"),
                   RUNNER_TEMP=str(self.root), PATH=str(self.bin) + os.pathsep + env["PATH"])
        env.update(extra)
        return subprocess.run(["bash", str(INSTALL)], cwd=self.job, env=env,
                              capture_output=True, text=True)

    def test_make_install_and_cached_prefix_for_both_platforms(self):
        self.repo.joinpath("Makefile").write_text(
            'install:\n\tinstall -d "$(DESTDIR)/bin"\n'
            '\tprintf "#!/bin/sh\\nexit 0\\n" > "$(DESTDIR)/bin/orbis-clang"\n'
            '\tcp "$(DESTDIR)/bin/orbis-clang" "$(DESTDIR)/bin/prospero-clang"\n'
            '\tchmod +x "$(DESTDIR)/bin/"*-clang\n')
        self.commit()
        for platform in ("ps4", "ps5"):
            with self.subTest(platform=platform):
                result = self.run_installer(platform)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                sdk = self.job / ".sdk" / platform / ".installed"
                self.assertIn(f"path={sdk}", (self.root / "output").read_text())
                result = self.run_installer(platform, SDK_REPO="/nonexistent/repository")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("Cloning", result.stdout)

    def test_preinstalled_sdk_wins(self):
        sdk = self.root / "preinstalled"
        (sdk / "bin").mkdir(parents=True)
        wrapper = sdk / "bin/prospero-clang"
        wrapper.write_text("#!/bin/sh\nexit 0\n")
        wrapper.chmod(0o755)
        result = self.run_installer("ps5", PS5_PAYLOAD_SDK=str(sdk))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Cloning", result.stdout)

    def test_legacy_build_script_still_works(self):
        script = self.repo / "build.sh"
        script.write_text('''#!/bin/bash
mkdir -p bin
printf '#!/bin/sh\\nexit 0\\n' > bin/prospero-clang
chmod +x bin/prospero-clang
''')
        script.chmod(0o755)
        self.commit()
        result = self.run_installer("ps5")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failed_make_does_not_export_source_wrapper(self):
        host = self.repo / "host/bin"
        host.mkdir(parents=True)
        wrapper = host / "prospero-clang"
        wrapper.write_text("#!/bin/sh\nexit 0\n")
        wrapper.chmod(0o755)
        self.repo.joinpath("Makefile").write_text("install:\n\tfalse\n")
        self.commit()
        result = self.run_installer("ps5")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "output").exists())


if __name__ == "__main__":
    unittest.main()
