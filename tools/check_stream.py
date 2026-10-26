#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Collect framed PBS video and write Annex B for a real decoder check."""
import argparse
import json
import struct
import time
import urllib.request
from pathlib import Path


def read_exact(stream, size):
    data = bytearray()
    while len(data) < size:
        part = stream.read(size - len(data))
        if not part:
            raise EOFError('Stream ended before a complete packet')
        data.extend(part)
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', default='192.168.1.25')
    ap.add_argument('--port', type=int, default=9999)
    ap.add_argument('--seconds', type=float, default=4)
    ap.add_argument('--output', type=Path, default=Path('build/capture.h264'))
    args = ap.parse_args()
    base = f'http://{args.host}:{args.port}'
    with urllib.request.urlopen(base + '/config.json', timeout=3) as response:
        config = json.load(response)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    count = total = 0
    first_pts = last_pts = None
    with args.output.open('wb') as out:
        for name in ('sps', 'pps'):
            out.write(b'\0\0\0\1' + bytes.fromhex(config[name]))
        with urllib.request.urlopen(base + '/frames', timeout=3) as response:
            until = time.monotonic() + args.seconds
            while time.monotonic() < until:
                size, pts = struct.unpack('!II', read_exact(response, 8))
                if not 0 < size <= 0x200000:
                    raise ValueError(f'Invalid packet size: {size}')
                out.write(read_exact(response, size))
                count += 1
                total += size
                if first_pts is None:
                    first_pts = pts
                if last_pts is not None:
                    delta = (pts - last_pts) & 0xffffffff
                    if delta == 0 or delta >= 0x80000000:
                        raise ValueError(f'Non-monotonic video timestamp: {last_pts} -> {pts}')
                last_pts = pts
    print(json.dumps(dict(config=config, frames=count, bytes=total,
                          first_pts=first_pts, last_pts=last_pts, output=str(args.output)), indent=2))
    if not count:
        raise SystemExit('No frames received')


if __name__ == '__main__':
    main()
