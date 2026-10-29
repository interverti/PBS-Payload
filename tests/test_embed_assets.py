import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "payload" / "embed_assets.py"


class EmbedAssetsTest(unittest.TestCase):
    def embed(self, *args, logo_env=None):
        out = Path(tempfile.mkdtemp())
        env = {key: value for key, value in os.environ.items() if key != "PBS_WEB_LOGO"}
        if logo_env is not None:
            env["PBS_WEB_LOGO"] = logo_env
        subprocess.run([sys.executable, str(SCRIPT), "--out", str(out), *args],
                       check=True, cwd=str(ROOT), env=env)
        return out

    def test_default_build_carries_no_banner(self):
        out = self.embed()
        banner = (out / "banner.h").read_text()
        self.assertIn("PBS_BANNER_LEN = 0", banner)
        self.assertNotIn("sizeof PBS_BANNER_BYTES", banner)
        # An empty array, not the 650 KB PNG.
        self.assertLess(len(banner), 1024)
        self.assertIn("PBS_H264_HTML", (out / "webui.h").read_text())

    def test_logo_is_embedded_only_when_requested(self):
        out = self.embed("--logo")
        banner = (out / "banner.h").read_text()
        self.assertIn("PBS_BANNER_LEN = sizeof PBS_BANNER_BYTES", banner)
        self.assertGreater(len(banner), 100000)
        self.assertGreater((ROOT / "payload" / "web" / "banner.png").stat().st_size, 100000)

    def test_env_flag_enables_the_logo(self):
        out = self.embed(logo_env="1")
        self.assertIn("PBS_BANNER_LEN = sizeof PBS_BANNER_BYTES", (out / "banner.h").read_text())

    def test_env_flag_zero_keeps_it_off(self):
        out = self.embed(logo_env="0")
        self.assertIn("PBS_BANNER_LEN = 0", (out / "banner.h").read_text())


if __name__ == "__main__":
    unittest.main()
