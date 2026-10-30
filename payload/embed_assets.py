#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Embed the shared viewer identically for both console builds.

The owner's banner is branding, not code, so it is embedded only when the build
asks for it (``--logo`` or ``PBS_WEB_LOGO=1``). Without it the payload is about
650 KB smaller and ``/banner.png`` answers 404, which the viewer treats as "no
corner logo".
"""
import argparse
import os
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--logo', action='store_true',
                    default=os.environ.get('PBS_WEB_LOGO', '0') == '1',
                    help='embed web/banner.png (default: off)')
parser.add_argument('--out', type=Path, default=None,
                    help='output directory (default: payload/build/generated)')
args = parser.parse_args()

root = Path(__file__).resolve().parent
generated = args.out or root / 'build/generated'
generated.mkdir(parents=True, exist_ok=True)
viewer = (root / 'web/h264.html').read_text(encoding='utf-8').replace(
    '/*PBS_MUX*/', (root / 'web/h264-mux.js').read_text(encoding='utf-8')).replace(
    '/*PBS_VIDEOJS*/', (root / 'web/vendor/videojs-player.js').read_text(encoding='utf-8').replace('</script', '<\\/script'))
escaped = viewer.replace('\\', '\\\\').replace('"', '\\"').replace('\r', '').replace('\n', '\\n').replace('?', r'\?')
(generated / 'webui.h').write_text(
    '#pragma once\nstatic const char PBS_H264_HTML[] = "' + escaped + '";\n', encoding='utf-8')
if args.logo:
    data = (root / 'web/banner.png').read_bytes()
    banner = ('#pragma once\nstatic const unsigned char PBS_BANNER_BYTES[] = {' +
              ','.join(map(str, data)) + '};\n'
              'static const size_t PBS_BANNER_LEN = sizeof PBS_BANNER_BYTES;\n')
else:
    banner = ('#pragma once\nstatic const unsigned char PBS_BANNER_BYTES[] = {0};\n'
              'static const size_t PBS_BANNER_LEN = 0;\n')
(generated / 'banner.h').write_text(banner, encoding='utf-8')
