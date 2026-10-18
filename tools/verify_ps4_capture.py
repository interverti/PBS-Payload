#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify that an H.264 capture actually decodes to visible video.

Requires ffmpeg and ffprobe on PATH (or --ffmpeg / --ffprobe). All decoding is
offline. The optional --console path retrieves only the PS4 smoke recording via
read-only FTP; it never accesses the ELF loader or invokes console payloads.
"""

import argparse
import ftplib
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
from fractions import Fraction
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LOCAL_RESEARCH = ROOT / ".local-research"
REMOTE_SMOKE = "/data/pbs/ps4/smoke.h264"
SAMPLE_WIDTH = 64
SAMPLE_HEIGHT = 36
FRAME_BYTES = SAMPLE_WIDTH * SAMPLE_HEIGHT * 3
MAX_FTP_BYTES = 64 * 1024 * 1024  # Smoke writer caps video at 32 MiB.
BLACK_THRESHOLDS = {
    "max_rgb_near_black": 24,
    "max_rgb_bright": 50,
    "minimum_near_black_pixel_fraction": 0.98,
    "maximum_bright_pixel_fraction": 0.01,
    "maximum_mean_luma": 20.0,
    "maximum_spatial_luma_stddev": 12.0,
    "minimum_video_black_frame_fraction": 0.95,
}


class VerificationError(Exception):
    """An expected input, media, or tool failure with a useful explanation."""


class BadCaptureError(VerificationError):
    """The media itself is invalid or cannot be decoded."""


def output_path(value):
    """Keep all user-requested artifacts inside the repository's ignored area."""
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        candidate = ROOT / candidate
    candidate = candidate.resolve()
    allowed = LOCAL_RESEARCH.resolve()
    if not candidate.is_relative_to(allowed) or candidate == allowed:
        raise VerificationError(f"Output must be inside {allowed}")
    return candidate


def ftp_download(host, port, destination, timeout, overwrite=False):
    if destination.exists() and not overwrite:
        raise VerificationError(f"Refusing to replace {destination}; pass --overwrite")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    transferred = 0
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", prefix=".pbs-smoke-", suffix=".part",
            dir=destination.parent, delete=False,
        ) as file:
            temporary = Path(file.name)
            def receive(chunk):
                nonlocal transferred
                transferred += len(chunk)
                if transferred > MAX_FTP_BYTES:
                    raise VerificationError("FTP smoke recording exceeds 64 MiB limit")
                file.write(chunk)

            with ftplib.FTP() as ftp:
                ftp.connect(host, port, timeout=timeout)
                ftp.login("anonymous", "anonymous")
                ftp.retrbinary("RETR " + REMOTE_SMOKE, receive, blocksize=65536)
            file.flush()
            os.fsync(file.fileno())
        if temporary.stat().st_size == 0:
            raise VerificationError("FTP returned an empty smoke.h264")
        if destination.exists() and not overwrite:
            raise VerificationError(f"Refusing to replace {destination}; pass --overwrite")
        os.replace(temporary, destination)
        temporary = None
    except (OSError, EOFError, ftplib.Error) as exc:
        raise VerificationError(f"FTP retrieval failed: {exc}") from exc
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def run_tool(argv, timeout):
    try:
        return subprocess.run(argv, capture_output=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise VerificationError(f"{Path(argv[0]).name} timed out after {timeout:g}s") from exc
    except OSError as exc:
        raise VerificationError(f"Cannot execute {argv[0]}: {exc}") from exc


def frame_rate(value):
    try:
        fps = float(Fraction(value))
        return round(fps, 3) if math.isfinite(fps) and 0 < fps <= 240 else None
    except (ValueError, TypeError, ZeroDivisionError):
        return None


def probe(path, executable, timeout):
    cmd = [executable, "-v", "error", "-select_streams", "v:0",
           "-show_entries", "stream=codec_name,width,height,avg_frame_rate,r_frame_rate,pix_fmt",
           "-of", "json", str(path)]
    result = run_tool(cmd, timeout)
    if result.returncode:
        raise BadCaptureError("ffprobe failed: " + result.stderr.decode(errors="replace").strip()[-1000:])
    try:
        streams = json.loads(result.stdout)["streams"]
    except (ValueError, KeyError, TypeError) as exc:
        raise BadCaptureError("ffprobe produced invalid JSON or no stream list") from exc
    if not streams:
        raise BadCaptureError("No decodable video stream found")
    stream = streams[0]
    try:
        width, height = int(stream["width"]), int(stream["height"])
    except (ValueError, TypeError, KeyError) as exc:
        raise BadCaptureError("ffprobe did not identify video dimensions") from exc
    if width <= 0 or height <= 0 or width * height > 7680 * 4320:
        raise BadCaptureError(f"Invalid or excessive video dimensions: {width}x{height}")
    if stream.get("codec_name") != "h264":
        raise BadCaptureError(f"Expected H.264, found {stream.get('codec_name', 'unknown')}")
    return {
        "codec": stream["codec_name"], "width": width, "height": height,
        "pixel_format": stream.get("pix_fmt"),
        "ffprobe_avg_fps": frame_rate(stream.get("avg_frame_rate")),
        "ffprobe_r_fps": frame_rate(stream.get("r_frame_rate")),
        "fps_note": "Elementary H.264 may contain no timestamps; ffprobe rates are not a verified capture FPS.",
    }


def classify_frames(raw):
    """Analyze every decoded frame after area downsampling, not compressed NALs."""
    if len(raw) % FRAME_BYTES:
        raise BadCaptureError("Decoder output ended in a partial RGB frame")
    count = len(raw) // FRAME_BYTES
    if not count:
        raise BadCaptureError("FFmpeg decoded zero video frames")

    pixels = SAMPLE_WIDTH * SAMPLE_HEIGHT
    total_rgb = [0, 0, 0]
    total_luma = total_luma_square = total_chroma = 0
    total_near_black = total_bright = 0
    black_frames = 0
    spatial_variance_total = temporal_delta_total = frame_mean_square_total = 0.0
    previous = None
    min_luma, max_luma = 255, 0

    for start in range(0, len(raw), FRAME_BYTES):
        values = raw[start:start + FRAME_BYTES]
        luminances = bytearray(pixels)
        frame_sum = frame_square = near_black = bright = 0
        for index, offset in enumerate(range(0, FRAME_BYTES, 3)):
            red, green, blue = values[offset], values[offset + 1], values[offset + 2]
            total_rgb[0] += red
            total_rgb[1] += green
            total_rgb[2] += blue
            peak = max(red, green, blue)
            total_chroma += peak - min(red, green, blue)
            near_black += peak <= BLACK_THRESHOLDS["max_rgb_near_black"]
            bright += peak > BLACK_THRESHOLDS["max_rgb_bright"]
            lum = (54 * red + 183 * green + 19 * blue + 128) >> 8
            luminances[index] = lum
            min_luma = min(min_luma, lum)
            max_luma = max(max_luma, lum)
            frame_sum += lum
            frame_square += lum * lum
        mean = frame_sum / pixels
        variance = max(0.0, frame_square / pixels - mean * mean)
        spatial_variance_total += variance
        frame_mean_square_total += mean * mean
        total_luma += frame_sum
        total_luma_square += frame_square
        total_near_black += near_black
        total_bright += bright
        if (near_black / pixels >= BLACK_THRESHOLDS["minimum_near_black_pixel_fraction"]
                and bright / pixels <= BLACK_THRESHOLDS["maximum_bright_pixel_fraction"]
                and mean <= BLACK_THRESHOLDS["maximum_mean_luma"]
                and math.sqrt(variance) <= BLACK_THRESHOLDS["maximum_spatial_luma_stddev"]):
            black_frames += 1
        if previous is not None:
            temporal_delta_total += sum(abs(a - b) for a, b in zip(previous, luminances)) / pixels
        previous = luminances

    pixel_count = count * pixels
    overall_mean = total_luma / pixel_count
    overall_variance = max(0.0, total_luma_square / pixel_count - overall_mean ** 2)
    frame_mean_variance = max(0.0, frame_mean_square_total / count - overall_mean ** 2)
    def rounded(value):
        return round(value, 4)

    return {
        "sampled_width": SAMPLE_WIDTH, "sampled_height": SAMPLE_HEIGHT,
        "decoded_frames": count, "black_frames": black_frames,
        "nonblack_frames": count - black_frames,
        "black_frame_fraction": rounded(black_frames / count),
        "nonblack_frame_fraction": rounded((count - black_frames) / count),
        "near_black_pixel_fraction": rounded(total_near_black / pixel_count),
        "bright_pixel_fraction": rounded(total_bright / pixel_count),
        "mean_rgb": [rounded(channel / pixel_count) for channel in total_rgb],
        "mean_color_channel_range": rounded(total_chroma / pixel_count),
        "mean_luma": rounded(overall_mean), "min_luma": min_luma, "max_luma": max_luma,
        "overall_luma_stddev": rounded(math.sqrt(overall_variance)),
        "mean_spatial_luma_stddev": rounded(math.sqrt(spatial_variance_total / count)),
        "frame_mean_luma_stddev": rounded(math.sqrt(frame_mean_variance)),
        "mean_temporal_luma_delta": rounded(temporal_delta_total / (count - 1)) if count > 1 else None,
        "predominantly_black": black_frames / count >= BLACK_THRESHOLDS["minimum_video_black_frame_fraction"],
        "thresholds": BLACK_THRESHOLDS,
    }


def verify(path, ffmpeg, ffprobe, timeout, min_frames, max_frames, assumed_fps):
    if not path.is_file():
        raise VerificationError(f"Capture file does not exist: {path}")
    length = path.stat().st_size
    if not length:
        raise BadCaptureError("Capture is empty")
    video = probe(path, ffprobe, timeout)
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-xerror", "-nostdin", "-i", str(path),
           "-map", "0:v:0", "-an", "-sn", "-dn", "-vf",
           f"scale={SAMPLE_WIDTH}:{SAMPLE_HEIGHT}:flags=area,format=rgb24",
           "-vsync", "0", "-frames:v", str(max_frames + 1),
           "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"]
    decoded = run_tool(cmd, timeout)
    if decoded.returncode:
        raise BadCaptureError("FFmpeg decode failed: " + decoded.stderr.decode(errors="replace").strip()[-1200:])
    analysis = classify_frames(decoded.stdout)
    frames = analysis["decoded_frames"]
    video["bytes"] = length
    video["bytes_per_decoded_frame"] = round(length / frames, 2)
    video["assumed_fps_for_bitrate"] = assumed_fps
    video["estimated_bitrate_kbps"] = round(length * 8 * assumed_fps / frames / 1000, 2)
    with path.open("rb") as source:
        video["sha256"] = hashlib.file_digest(source, "sha256").hexdigest()

    problems, warnings = [], []
    if frames > max_frames:
        problems.append(f"Capture exceeds analysis limit of {max_frames} frames; increase --max-frames")
    if frames < min_frames:
        problems.append(f"Only {frames} frames decoded; expected at least {min_frames}")
    if analysis["predominantly_black"]:
        problems.append(f"{analysis['black_frame_fraction']:.1%} of decoded frames are nearly black")
    if video["estimated_bitrate_kbps"] < 100 and video["width"] >= 640:
        warnings.append("Very low estimated H.264 bitrate; often consistent with a blank capture")
    if frames >= 30 and analysis["mean_temporal_luma_delta"] is not None and analysis["mean_temporal_luma_delta"] < 0.2:
        warnings.append("Very little frame-to-frame change; inspect for a frozen image")
    return video, analysis, problems, warnings


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Decode every PS4 H.264 frame with FFmpeg, test for near-black imagery and emit JSON.",
        epilog=("Examples:\n"
                "  python tools/verify_ps4_capture.py .local-research/ps4/smoke.h264\n"
                "  python tools/verify_ps4_capture.py --console\n"
                "  python tools/verify_ps4_capture.py --console 192.168.1.25 --output .local-research/ps4/latest.h264 --overwrite\n"
                "  python tools/verify_ps4_capture.py sample.h264 --report .local-research/ps4/report.json\n"
                "Exit 0: decodable nonblack video; 1: invalid/black/too short; 2: tool, I/O or FTP error."),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", nargs="?", type=Path, help="Local H.264 Annex B file")
    parser.add_argument("--console", nargs="?", const="192.168.1.25", metavar="HOST",
                        help="Read /data/pbs/ps4/smoke.h264 from PS4 via FTP (default host 192.168.1.25)")
    parser.add_argument("--ftp-port", type=int, default=2121, help="GoldHEN FTP port (default: 2121)")
    parser.add_argument("--ftp-timeout", type=float, default=15, help="FTP timeout in seconds")
    parser.add_argument("--output", type=Path, help="FTP download path within ignored .local-research/")
    parser.add_argument("--report", type=Path, help="Optional JSON report path within ignored .local-research/")
    parser.add_argument("--overwrite", action="store_true", help="Allow replacing an existing FTP download")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="Path/name of ffmpeg executable")
    parser.add_argument("--ffprobe", default="ffprobe", help="Path/name of ffprobe executable")
    parser.add_argument("--timeout", type=float, default=120, help="Maximum seconds per decoder/probe command")
    parser.add_argument("--min-frames", type=int, default=30, help="Minimum decoded frames required (default: 30)")
    parser.add_argument("--max-frames", type=int, default=3600, help="Bound decoded frames/memory (default: 3600)")
    parser.add_argument("--assumed-fps", type=float, default=60, help="FPS assumed only when estimating bitrate (default: 60)")
    args = parser.parse_args(argv)
    if (args.console is None) == (args.input is None):
        parser.error("Specify exactly one of INPUT or --console [HOST]")
    if args.output and args.console is None:
        parser.error("--output is for --console FTP downloads only")
    if args.ftp_port < 1 or args.ftp_port > 65535 or not math.isfinite(args.ftp_timeout) or args.ftp_timeout <= 0:
        parser.error("FTP port must be 1..65535 and FTP timeout must be positive")
    if (not math.isfinite(args.timeout) or args.timeout <= 0 or args.min_frames < 1
            or args.max_frames < args.min_frames or not math.isfinite(args.assumed_fps)
            or not 0 < args.assumed_fps <= 240):
        parser.error("Require timeout > 0, 1 <= min-frames <= max-frames and assumed-fps in (0,240]")

    report = {"schema_version": 1, "ok": False, "verdict": "error", "errors": [], "warnings": []}
    exit_code = 2
    report_file = None
    try:
        report_file = output_path(args.report) if args.report else None
        if args.console is not None:
            path = output_path(args.output or ".local-research/ps4/latest-smoke.h264")
            report["source"] = {"kind": "ftp", "host": args.console, "port": args.ftp_port,
                                "remote_path": REMOTE_SMOKE}
            ftp_download(args.console, args.ftp_port, path, args.ftp_timeout, args.overwrite)
        else:
            path = args.input.expanduser().resolve()
            report["source"] = {"kind": "local"}
        report["input"] = str(path)
        video, analysis, problems, warnings = verify(
            path, args.ffmpeg, args.ffprobe, args.timeout,
            args.min_frames, args.max_frames, args.assumed_fps,
        )
        report["video"] = video
        report["analysis"] = analysis
        report["errors"] = problems
        report["warnings"] = warnings
        report["ok"] = not problems
        report["verdict"] = "pass" if not problems else "fail"
        exit_code = 0 if not problems else 1
    except BadCaptureError as exc:
        report["verdict"] = "fail"
        report["errors"].append(str(exc))
        exit_code = 1
    except VerificationError as exc:
        report["errors"].append(str(exc))
    except (OSError, ftplib.Error) as exc:
        report["errors"].append(f"I/O failure: {exc}")

    payload = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    print(payload, end="")
    if report_file:
        try:
            report_file.parent.mkdir(parents=True, exist_ok=True)
            report_file.write_text(payload, encoding="utf-8")
        except (VerificationError, OSError) as exc:
            print(f"Could not write JSON report: {exc}", file=sys.stderr)
            return 2
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
