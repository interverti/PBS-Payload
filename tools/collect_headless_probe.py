#!/usr/bin/env python3
"""Read-only HTTP capture for issue #5: run once with display on, once off.

No capture restarts, profile changes, process attachment or console writes.
Only a bounded sample of the existing H.264 feed is read. FFmpeg, if present,
decodes one local PNG to distinguish black frames from missing packets.
"""
import argparse
import json
import shutil
import struct
import subprocess
import time
import urllib.request
from pathlib import Path
from urllib.parse import urlsplit


def read_exact(stream, count):
    parts = bytearray()
    while len(parts) < count:
        data = stream.read(count - len(parts))
        if not data:
            raise EOFError("Video connection closed")
        parts.extend(data)
    return bytes(parts)


def collect(base, phase, out, ffmpeg=None):
    out.mkdir(parents=True, exist_ok=True)
    report = {"phase": phase, "frames": 0, "bytes": 0, "errors": []}

    def get_json(path):
        try:
            with urllib.request.urlopen(base + path, timeout=8) as response:
                return json.loads(response.read(131072))
        except Exception as error:
            report["errors"].append(path + ": " + str(error))
            return {}

    report["configuration"] = get_json("/config.json")
    report["before"] = get_json("/status.json")
    config = report["configuration"]
    began = time.monotonic()
    try:
        with (out / "sample.h264").open("wb") as target:
            for key in ("sps", "pps"):
                nal = bytes.fromhex(config[key])
                if not nal or len(nal) > 128:
                    raise ValueError("Invalid capture headers")
                target.write(b"\0\0\0\1" + nal)
            with urllib.request.urlopen(base + "/frames", timeout=5) as stream:
                while time.monotonic() - began < 6 and report["frames"] < 120:
                    size, pts = struct.unpack("!II", read_exact(stream, 8))
                    if not 0 < size <= 0x200000:
                        raise ValueError("Invalid frame length")
                    if report["bytes"] + size > 8 * 1024 * 1024:
                        break
                    data = read_exact(stream, size)
                    target.write(data)
                    report["frames"] += 1
                    report["bytes"] += size
                    report["last_pts"] = pts
    except Exception as error:
        report["errors"].append("/frames: " + str(error))
    report["elapsed_seconds"] = round(time.monotonic() - began, 3)
    report["after"] = get_json("/status.json")
    if ffmpeg and report["frames"]:
        try:
            result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                                     "-i", str(out / "sample.h264"), "-frames:v", "1",
                                     str(out / "snapshot.png")], capture_output=True,
                                    text=True, timeout=30, check=False)
            if result.returncode:
                report["errors"].append("decode: " + result.stderr[-2000:])
        except Exception as error:
            report["errors"].append("decode: " + str(error))
    (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="Console URL, for example http://192.168.1.20:9999")
    parser.add_argument("--phase", choices=("tv-on", "tv-off"), required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    args = parser.parse_args()
    parsed = urlsplit(args.url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.query or parsed.fragment:
        parser.error("Use the console HTTP base URL without query parameters")
    out = args.out or Path(__file__).resolve().parents[1] / "build/diagnostics/headless" / args.phase
    report = collect(args.url.rstrip("/"), args.phase, out.resolve(), args.ffmpeg)
    print(f"Saved {report['frames']} frames ({report['bytes']} bytes) in {out.resolve()}")
    if report["errors"]:
        print("Collection notes are in report.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
