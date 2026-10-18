#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Embed the shared viewer identically for both console builds."""
from pathlib import Path

root = Path(__file__).resolve().parent
viewer = (root / 'web/h264.html').read_text(encoding='utf-8').replace(
    '/*PBS_MUX*/', (root / 'web/h264-mux.js').read_text(encoding='utf-8'))
escaped = viewer.replace('\\', '\\\\').replace('"', '\\"').replace('\r', '').replace('\n', '\\n')
(root / 'src/h264_webui.h').write_text(
    '#pragma once\nstatic const char PBS_H264_HTML[] = "' + escaped + '";\n', encoding='utf-8')
(root / 'src/h264_banner.h').write_text(
    '#pragma once\nstatic const unsigned char PBS_BANNER_BYTES[] = {' +
    ','.join(map(str, (root / 'web/banner.png').read_bytes())) + '};\n', encoding='utf-8')
