#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Instant diagnosis of a PBS PS4 capture report.

Reads the evidence a run leaves in /data/pbs/ps4 (capture.txt and the compact
problems.txt index) and prints the most likely root cause first, with the
decoded SCE error code and the operator hint, instead of hand-scanning the full
report. Exit codes: 0 clean, 1 problems found, 2 crash or inconclusive run.
"""
import argparse
import json
from pathlib import Path
import re
import sys

# Fallback for reports produced before the payload wrote PROBLEM lines. Keep in
# sync with payload/src/common/diagnostics/problem_log.c; that module's table is
# the source of truth for fresh logs because problems.txt already carries the
# decoded name and hint.
CODE_NAMES = {
    0x80020003: "KernelEsrch",
    0x8002003C: "KernelTimedOut",
    0x80A70001: "AvcapGeneric",
    0x80A70002: "AvcapEmptyRing",
    0x80A70003: "AvcapBudgetGate",
    0x80A70004: "AvcapRingOverrun",
    0x80A70008: "AvcapUnavailable",
    0x80A70009: "AvcapCapacity",
    0x80A7000A: "AvcapProfileRejected",
    0x80BF0001: "VencStartSequencePending",
    0x80BF000F: "VencNotStarted",
    0x80BF0011: "VencQueueState",
}
CODE_HINTS = {
    0x80020003: "ESRCH: target process or thread is gone (for example a replaced RemotePlay process)",
    0x8002003C: "ETIMEDOUT: a kernel wait budget expired",
    0x80A70001: "generic SceAvCapture failure; native code maps other kernel errors and 0x8002003c here",
    0x80A70002: "empty producer ring / timeout class; expected from flags=1 polling, not a failure by itself",
    0x80A70003: "budget gate refusal; the latch/budget gate rejected the request",
    0x80A70004: "producer overwrote unread frames; the reader resynchronized, request a fresh IDR",
    0x80A70008: ("temporarily unavailable: service not ready or encoder slot busy; native RemotePlay "
                 "retries 16ms for a 2016ms budget; read-only slot snapshot discriminates occupancy"),
    0x80A70009: "capacity/FPS admission condition, a distinct cause from 0x80a70008",
    0x80A7000A: "profile/capability rejection; the daemon maps capabilities & 0x3f0a60 to this code",
    0x80BF0001: "encoder core +0xb0a bit set: StartSequence did not complete (state 3)",
    0x80BF000F: "encoder not started; StartSequence never ran",
    0x80BF0011: "encoder dequeue after Close+Open; queue/generation state mismatch",
}

SEVERITY_RANK = {"note": 0, "warn": 1, "fail": 2, "fatal": 3}

# Phrases that mean ownership/teardown is uncertain even when a video
# recording exists. Mirrors tools/run_ps4_smoke.ps1's quarantine list.
QUARANTINE_PHRASES = (
    "PS4 debug gate restore pending",
    "RemotePlay Term uncertain",
    "Refusing new capture: previous RemotePlay Term",
    "Avcap release quarantined",
    "capture request ownership is uncertain",
    "RemotePlay Init timeout",
    "SetStaticInfo outcome uncertain",
    "local Avcap Term failed",
    "Avcap cleanup incomplete",
    "OpenVideoEx failure quarantined",
    "RemotePlay process replaced",
    "process birth unavailable",
    "Venc journal ownership uncertain",
    "capture teardown quarantined",
    "refusing to reopen a potentially active resource",
)


def code_name(code):
    if code in CODE_NAMES:
        return CODE_NAMES[code]
    if code and code & 0xFFFF0000 == 0x80020000:
        return "KernelErrno"
    return "-"


def code_hint(code):
    if code in CODE_HINTS:
        return CODE_HINTS[code]
    if code and code & 0xFFFF0000 == 0x80020000:
        return "SCE kernel errno class; decode the low 16 bits as an errno"
    return ""


def finding(finder_id, severity, stage, line_no, detail, code=0, hint="", error=None,
            source="log"):
    return {
        "id": finder_id,
        "source": source,
        "file": "capture.txt",
        "n": None,
        "severity": severity,
        "stage": stage,
        "line": line_no,
        "detail": detail,
        "code": code,
        "code_hex": f"{code:08x}" if code else "",
        "name": code_name(code) if code else "-",
        "hint": hint or (code_hint(code) if code else ""),
        "error": error,
        "cascade": False,
    }


def parse_problem_line(line, line_no):
    """Parse one PROBLEM record as written by problem_log.c (from either file)."""
    # detail= is always the last field and may contain spaces.
    record, detail = line, ""
    if " detail=" in line:
        record, detail = line.split(" detail=", 1)
    fields = dict(part.split("=", 1) for part in record.split()[1:] if "=" in part)
    code = int(fields.get("code", "0"), 16)
    stage = fields.get("stage", "-")
    error = fields.get("errno", "0")
    item = finding(
        stage, fields.get("sev", "note"), stage, line_no, detail, code, "",
        int(error) if error.lstrip("-").isdigit() else None, source="record")
    if fields.get("n", "").isdigit():
        item["n"] = int(fields["n"])
    # These stages only report the consequence of an earlier failure (an
    # exhausted reopen budget or the refusal to reopen after a bad teardown).
    if stage in ("recovery", "cleanup"):
        item["cascade"] = True
    return item


def parse_problems_index(text):
    """Parse the compact problems.txt index into findings plus its summary."""
    findings, summary = [], None
    for line_no, line in enumerate(text.splitlines(), 1):
        if line.startswith("PROBLEM "):
            item = parse_problem_line(line, line_no)
            item["file"] = "problems.txt"
            findings.append(item)
        elif line.startswith("SUMMARY "):
            summary = dict(part.split("=", 1) for part in line.split()[1:] if "=" in part)
    return findings, summary


def parse_report(text):
    """Extract findings, cleanup status and counters from a capture report."""
    findings = []
    summary = None
    lines = text.splitlines()
    for line_no, line in enumerate(lines, 1):
        if not line:
            continue
        if line.startswith("PROBLEM "):
            findings.append(parse_problem_line(line, line_no))
            continue
        if line.startswith("SUMMARY "):
            summary = dict(part.split("=", 1) for part in line.split()[1:] if "=" in part)
            continue
        if line.startswith("PBS PROBLEMS"):
            continue
        match = re.match(r"^CRASH sig=([0-9a-f]{2}) rip=?([0-9a-f]{16,17}) address=?([0-9a-f]{16,17})", line)
        if match:
            findings.append(finding(
                "crash", "fatal", "crash", line_no,
                f"payload crashed: sig={match.group(1)} rip={match.group(2)} address={match.group(3)}",
                hint="payload exception: preserve evidence and do not redeploy before reviewing the "
                     "faulting context and the last PROBLEM line"))
            continue
        for phrase in QUARANTINE_PHRASES:
            if phrase in line:
                item = finding(
                    "quarantine", "fatal", "teardown", line_no, line.strip(),
                    hint="cleanup/ownership uncertain: do not run a second payload until the console "
                         "state is verified")
                # A quarantine marker describes the consequence of an earlier
                # failure (or an uncertain teardown); never let it outrank the
                # operation that actually failed when naming the root cause.
                item["cascade"] = True
                findings.append(item)
                break
        match = re.match(r"^OpenVideoEx profile=(\d+) -> ([0-9a-f]{8}) handle=(\S+)", line)
        if match and match.group(2) != "00000000":
            code = int(match.group(2), 16)
            detail = line.strip()
            hint = code_hint(code)
            if code == 0x80A70008:
                slots = re.search(
                    r"Avcap encoder slots at (?:final OpenVideoEx 80a70008|first OpenVideoEx 80a70008)"
                    r": mask=([0-9a-f]{8}) slot0=([01])", text)
                if slots:
                    occupied = slots.group(2) == "1"
                    hint = ("encoder slot0 was occupied at snapshot time (mask=%s): an unknown owner "
                            "holds the resource; investigate concurrent capture owners and never clear "
                            "the bitmap or close foreign resources" % slots.group(1)) if occupied else (
                            "slot0 was free at snapshot time (mask=%s): a race is still possible; study "
                            "only read-only getters of the capacity branch" % slots.group(1))
            findings.append(finding("open", "fatal", "open", line_no, detail, code, hint))
            continue
        match = re.match(r"^Init -> ([0-9a-f]{8})$", line)
        if match and match.group(1) != "00000000":
            findings.append(finding("init", "fatal", "init", line_no, line.strip(),
                                    int(match.group(1), 16)))
            continue
        match = re.match(r"^Start -> ([0-9a-f]{8})$", line)
        if match and match.group(1) != "00000000":
            findings.append(finding("start", "fail", "start", line_no, line.strip(),
                                    int(match.group(1), 16)))
            continue
        match = re.match(r"^Pre-Start GetInfo SPS=(-?\d+) PPS=(-?\d+)", line)
        if match and (int(match.group(1)) <= 0 or int(match.group(2)) <= 0):
            findings.append(finding(
                "prestart", "fail", "prestart", line_no, line.strip(),
                hint="Avcap never produced SPS/PPS headers before Start; the encoder sequence did not "
                     "stabilize within the 2000ms barrier"))
            continue
        match = re.match(r"^Remote ReadVideo failed rc=([0-9a-f]{8})", line)
        if match:
            findings.append(finding("read", "fatal", "read", line_no, line.strip(),
                                    int(match.group(1), 16)))
            continue
        match = re.match(r"^ReadVideo stalled rc=([0-9a-f]{8}) errno=(-?\d+)", line)
        if match:
            findings.append(finding(
                "stall", "fail", "read", line_no, line.strip(), int(match.group(1), 16),
                "no encoded packet for more than 3s; producer stopped or the ring is empty", int(match.group(2))))
            continue
        match = re.match(r"^H264 publish failed after (\d+) frames", line)
        if match:
            findings.append(finding(
                "publish", "fatal", "publish", line_no, line.strip(),
                hint="the HTTP pipeline rejected a frame; streaming cannot claim success"))
            continue
        match = re.match(r"^Frame buffer allocation failed errno=(-?\d+)", line)
        if match:
            findings.append(finding("memory", "fatal", "memory", line_no, line.strip(),
                                    error=int(match.group(1))))
            continue
        match = re.match(r"^HTTP server failed on port (\d+) errno=(-?\d+)", line)
        if match:
            findings.append(finding(
                "http", "fatal", "http", line_no, line.strip(), error=int(match.group(2)),
                hint=f"port {match.group(1)} unavailable (a stale server or a bind error)"))
            continue
        if line.startswith("Avcap ring overrun"):
            findings.append(finding(
                "overrun", "warn", "read", line_no, line.strip(), 0x80A70004,
                hint="recovered automatically: frame references were invalidated and an IDR was requested"))
            continue
        match = re.match(r"^SUSPECTED EMPTY SCAN-IN: (\d+) packets (\d+) bytes", line)
        if match:
            findings.append(finding(
                "empty-scan-in", "warn", "source", line_no, line.strip(),
                hint="encoder emitted only near-empty frames: scan-in pixels are suspect, decode the "
                     "recorded sample before trusting this run"))
            continue
        if line.startswith("WARNING: unusually low H.264 output rate"):
            findings.append(finding(
                "low-rate", "warn", "stream", line_no, line.strip(),
                hint="output rate is abnormally low; inspect decoded frames, source/scan-in unverified"))
            continue
        match = re.match(r"^Avcap recovery incomplete after (\d+) attempts", line)
        if match:
            item = finding("recovery", "fatal", "recovery", line_no, line.strip(),
                           hint="the bounded reopen budget expired; this is usually a cascade of an "
                                "earlier failure")
            item["cascade"] = True
            findings.append(item)
            continue
        match = re.match(r"^Stop -> ([0-9a-f]{8})$|^Close -> ([0-9a-f]{8})$|^Term -> ([0-9a-f]{8})$", line)
        if match and "00000000" not in match.groups():
            code = int(next(g for g in match.groups() if g), 16)
            stage = line.split(" ")[0].lower()
            findings.append(finding("teardown", "fail", stage, line_no, line.strip(), code))
            continue
    cleanup = {
        "stop": _last_status(lines, r"^Stop -> ([0-9a-f]{8})$"),
        "close": _last_status(lines, r"^Close -> ([0-9a-f]{8})$"),
        "term": _last_status(lines, r"^Term -> ([0-9a-f]{8})$"),
        "request_release": _request_release(lines),
    }
    exit_match = re.search(r"(?m)^exit=(\d+)\s*$", text)
    counters = {
        "packets": _counter(text, r"(?m)^total packets=(\d+) bytes=(\d+) reopen=(\d+)"),
        "reopens": _reopen_counter(text),
    }
    return {
        "findings": findings,
        "cleanup": cleanup,
        "exit": int(exit_match.group(1)) if exit_match else None,
        "counters": counters,
        "summary": summary,
    }


def _last_status(lines, pattern):
    values = [m.group(1) for line in lines for m in [re.match(pattern, line)] if m]
    if not values:
        return None
    return values[-1] == "00000000"


def _request_release(lines):
    for line in lines:
        if line.startswith("RemotePlay capture request 0 -> "):
            return line.strip().endswith("00000000")
        if line.startswith("Generic capture: RemotePlay-specific request not acquired"):
            return True
    return None


def _counter(text, pattern):
    match = re.search(pattern, text)
    return int(match.group(1)) if match else None


def _reopen_counter(text):
    match = re.search(r"(?m)^total packets=\d+ bytes=\d+ reopen=(\d+)", text)
    return int(match.group(1)) if match else None


def merge_findings(findings):
    """Unify structured PROBLEM records and classic report lines.

    The same event usually appears twice: as a payload PROBLEM record and as
    its historical human line. Counting both inflates the diagnosis, so a
    classic line whose (stage, code, severity) matches a record is folded into
    that record, transferring any evidence-specific hint (for example the
    encoder-slot occupancy reading) and its cascade marker. The same record
    mirrored into both evidence files is identified by its sequence number n
    and kept once, while genuinely repeated events survive as separate items.
    """
    seen_numbers, unique = set(), []
    for item in findings:
        if item["source"] == "record" and item["n"] is not None:
            if item["n"] in seen_numbers:
                continue
            seen_numbers.add(item["n"])
        unique.append(item)
    records = [item for item in unique if item["source"] == "record"]
    merged = []
    for item in unique:
        if item["source"] == "record":
            merged.append(item)
            continue
        twin = next((record for record in records
                     if record["stage"] == item["stage"] and record["code"] == item["code"]
                     and record["severity"] == item["severity"]), None)
        if twin is not None:
            if item["hint"] and item["hint"] != code_hint(item["code"]):
                twin["hint"] = item["hint"]
            twin["cascade"] = twin["cascade"] or item["cascade"]
            continue
        merged.append(item)
    return merged


def diagnose(text, problems_text=None):
    """Combine the report and the compact index into one diagnosis dict."""
    report = parse_report(text)
    findings = list(report["findings"])
    summary = report["summary"]
    if problems_text:
        index_findings, index_summary = parse_problems_index(problems_text)
        findings.extend(index_findings)
        if index_summary is not None:
            summary = index_summary
    findings = sorted(merge_findings(findings), key=lambda item: item["line"])
    worst = max((SEVERITY_RANK[item["severity"]] for item in findings), default=0)
    recorded = [item for item in findings if not item["cascade"]]
    primary = None
    if recorded:
        primary = next((item for item in recorded
                        if SEVERITY_RANK[item["severity"]] == max(
                            SEVERITY_RANK[other["severity"]] for other in recorded)), None)
    elif findings:
        primary = findings[0]
    cleanup = report["cleanup"]
    cleanup_ok = all(value for value in cleanup.values() if value is not None)
    if any(item["id"] == "crash" for item in findings):
        verdict = "crash"
    elif any(item["id"] == "quarantine" for item in findings):
        verdict = "quarantine"
    elif worst >= SEVERITY_RANK["fail"]:
        verdict = "fail"
    elif report["exit"] != 0:
        verdict = "incomplete"
    else:
        verdict = "pass"
    return {
        "verdict": verdict,
        "exit": report["exit"],
        "primary": primary,
        "findings": findings,
        "cleanup": cleanup,
        "cleanup_ok": cleanup_ok,
        "counters": report["counters"],
        "summary": summary,
        "counts": {name: sum(1 for item in findings if item["severity"] == name)
                   for name in SEVERITY_RANK},
    }


def format_report(diagnosis, source="capture.txt"):
    out = []
    exit_text = "exit=" + (str(diagnosis["exit"]) if diagnosis["exit"] is not None else "missing")
    out.append(f"PS4 capture diagnosis: {diagnosis['verdict'].upper()} ({exit_text})")
    primary = diagnosis["primary"]
    if primary:
        out.append(f"  primary  [{primary['severity']}] stage={primary['stage']} "
                   f"code={primary['code_hex'] or '-'} name={primary['name']}")
        out.append(f"    detail: {primary['detail']}")
        if primary["hint"]:
            out.append(f"    hint:   {primary['hint']}")
        out.append(f"    evidence: {primary.get('file', source)}:{primary['line']}")
    else:
        out.append("  primary  none detected")
    counts = diagnosis["counts"]
    out.append("  findings %d (fatal=%d fail=%d warn=%d note=%d)" % (
        len(diagnosis["findings"]), counts["fatal"], counts["fail"], counts["warn"], counts["note"]))
    for item in diagnosis["findings"]:
        if item is primary:
            continue
        code = f" code={item['code_hex']}" if item["code_hex"] else ""
        out.append(f"    - [{item['severity']}] stage={item['stage']}{code} {item['detail']}"
                   f" ({item.get('file', source)}:{item['line']})")
    cleanup = diagnosis["cleanup"]
    state = lambda value: "ok" if value else ("MISSING" if value is None else "FAILED")
    out.append("  cleanup  Stop=%s Close=%s Term=%s request_release=%s" % (
        state(cleanup["stop"]), state(cleanup["close"]), state(cleanup["term"]),
        state(cleanup["request_release"])))
    counters = diagnosis["counters"]
    if counters["packets"] is not None:
        out.append(f"  totals   packets={counters['packets']} reopens={counters['reopens']}")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="capture.txt retrieved from /data/pbs/ps4")
    parser.add_argument("--problems", type=Path,
                        help="problems.txt compact index from the same run")
    parser.add_argument("--json", type=Path, help="also write the full diagnosis as JSON")
    args = parser.parse_args()
    text = args.report.read_text(encoding="utf-8", errors="replace")
    problems_text = None
    if args.problems and args.problems.is_file():
        problems_text = args.problems.read_text(encoding="utf-8", errors="replace")
    diagnosis = diagnose(text, problems_text)
    print(format_report(diagnosis, args.report.name))
    if args.json:
        args.json.write_text(json.dumps(diagnosis, indent=2), encoding="utf-8")
    if diagnosis["verdict"] == "pass":
        return 0
    return 2 if diagnosis["verdict"] in ("crash", "incomplete") else 1


if __name__ == "__main__":
    sys.exit(main())
