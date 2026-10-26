# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regression for tools/diagnose_ps4_log.py. No console, no network."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('diagnose', ROOT / 'tools/diagnose_ps4_log.py')
diagnose = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnose)

CLEAN_RUN = """PBS PS4 startup argc=2 argv=0x1
PBS PS4 fw=09000005 1280x72060 port=8080 seconds=10 usage=0 paste=0
Capture ABI profile: FW11
Init -> 00000000
OpenVideoEx profile=19 -> 00000000 handle=0x1234
Pre-Start GetInfo SPS=28 PPS=8 elapsed_ms=42
Start -> 00000000
session packets=540 bytes=123456 min=32 max=9000 elapsed_ms=9980 fps=54.11 mbps=0.09900
transport_packets=540 video_pixels=UNVERIFIED_NEEDS_FFMPEG publish_failed=0
Stop -> 00000000
Close -> 00000000
Term -> 00000000
RemotePlay capture request 0 -> 00000000
total packets=540 bytes=123456 reopen=0
exit=0
"""

OPEN_FAILURE = """PBS PS4 startup argc=2 argv=0x1
Init -> 00000000
OpenVideoEx transient 80a70008 retries=125 elapsed_ms=2016
PROBLEM n=1 t=2100 sev=note stage=open code=80a70008 name=AvcapUnavailable errno=0 detail=transient 80a70008: 125 bounded retries in 2016ms
OpenVideoEx profile=19 -> 80a70008 handle=(nil)
Avcap encoder slots at final OpenVideoEx 80a70008: mask=00000001 slot0=1 (read-only)
PROBLEM n=2 t=2101 sev=fatal stage=open code=80a70008 name=AvcapUnavailable errno=0 detail=OpenVideoEx profile=19 failed and is quarantined
OpenVideoEx failure quarantined; not reopening an unknown capture slot
Avcap encoder slots after retry Close: mask=00000001 slot0=1 (read-only)
Stop -> 00000000
Close -> 00000000
Term -> 00000000
RemotePlay capture request 0 -> 00000000
total packets=0 bytes=0 reopen=0
SUMMARY problems=2 first=n=1 stage=open code=80a70008 sev=note worst=fatal exit=1
exit=1
"""

READ_FAILURE = """Init -> 00000000
OpenVideoEx profile=19 -> 00000000 handle=0x1234
Pre-Start GetInfo SPS=28 PPS=8 elapsed_ms=40
Start -> 00000000
Remote ReadVideo failed rc=80bf0011; ending capture before further target calls
PROBLEM n=1 t=4200 sev=fatal stage=read code=80bf0011 name=VencQueueState errno=0 detail=Remote ReadVideo failed after 116 packets
Avcap cleanup failed; refusing to reopen a potentially active resource
Stop -> 00000000
Close -> 00000000
Term -> 00000000
RemotePlay capture request 0 -> 00000000
total packets=116 bytes=112585 reopen=0
exit=1
"""

STALLED_RUN = """Init -> 00000000
OpenVideoEx profile=19 -> 00000000 handle=0x1234
Start -> 00000000
ReadVideo stalled rc=00000000 errno=0
Avcap reopen attempt 1/3
Avcap recovery incomplete after 3 attempts
Stop -> 00000000
Close -> 00000000
Term -> 00000000
exit=1
"""

CRASH_RUN = """Init -> 00000000
OpenVideoEx profile=19 -> 00000000 handle=0x1234
CRASH sig=0b rip=0000000000401234 address=0000000000000000
"""


class CodeDictionaryTest(unittest.TestCase):
    """Pin the fallback table to the curated names in problem_log.c."""

    def test_names_and_hints_match_payload_table(self):
        self.assertEqual(diagnose.CODE_NAMES[0x80A70008], "AvcapUnavailable")
        self.assertEqual(diagnose.CODE_NAMES[0x80A70004], "AvcapRingOverrun")
        self.assertEqual(diagnose.CODE_NAMES[0x80A7000A], "AvcapProfileRejected")
        self.assertEqual(diagnose.CODE_NAMES[0x80BF0001], "VencStartSequencePending")
        self.assertEqual(diagnose.CODE_NAMES[0x80BF0011], "VencQueueState")
        self.assertEqual(diagnose.CODE_NAMES[0x80020003], "KernelEsrch")
        for code in diagnose.CODE_NAMES:
            self.assertTrue(diagnose.CODE_HINTS[code], f"missing hint for {code:08x}")

    def test_unknown_code_decoding(self):
        self.assertEqual(diagnose.code_name(0x80AB1234), "-")
        self.assertEqual(diagnose.code_hint(0x80AB1234), "")
        self.assertEqual(diagnose.code_name(0x80020063), "KernelErrno")
        self.assertIn("errno", diagnose.code_hint(0x80020063))


class DiagnosisTest(unittest.TestCase):
    def test_clean_run_passes(self):
        result = diagnose.diagnose(CLEAN_RUN)
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["exit"], 0)
        self.assertIsNone(result["primary"])
        self.assertTrue(result["cleanup_ok"])
        self.assertEqual(result["counters"]["packets"], 540)

    def test_open_failure_primary_is_root_cause_with_slot_hint(self):
        result = diagnose.diagnose(OPEN_FAILURE)
        self.assertEqual(result["verdict"], "quarantine")
        self.assertEqual(result["exit"], 1)
        self.assertEqual(result["primary"]["id"], "open")
        self.assertEqual(result["primary"]["code"], 0x80A70008)
        self.assertIn("slot0 was occupied", result["primary"]["hint"])
        self.assertIn("unknown owner", result["primary"]["hint"])
        # The transient-retry note must not outrank the fatal Open failure.
        self.assertEqual(result["counts"]["note"], 1)
        self.assertEqual(result["counts"]["fatal"], 2)
        self.assertEqual(result["summary"]["first"], "n=1")

    def test_read_failure_decodes_code(self):
        result = diagnose.diagnose(READ_FAILURE)
        self.assertEqual(result["verdict"], "quarantine")
        self.assertEqual(result["primary"]["id"], "read")
        self.assertEqual(result["primary"]["name"], "VencQueueState")

    def test_stall_is_primary_over_cascade(self):
        result = diagnose.diagnose(STALLED_RUN)
        self.assertEqual(result["verdict"], "fail")
        self.assertEqual(result["primary"]["id"], "stall")
        recovery = [item for item in result["findings"] if item["id"] == "recovery"]
        self.assertTrue(recovery and recovery[0]["cascade"])

    def test_crash_reports_verdict_and_preserves_raw_line(self):
        result = diagnose.diagnose(CRASH_RUN)
        self.assertEqual(result["verdict"], "crash")
        self.assertEqual(result["primary"]["id"], "crash")
        self.assertIn("rip=0000000000401234", result["primary"]["detail"])

    def test_missing_exit_marker_is_incomplete(self):
        result = diagnose.diagnose("Init -> 00000000\nOpenVideoEx profile=19 -> 00000000 handle=0x1\n")
        self.assertEqual(result["verdict"], "incomplete")
        self.assertIsNone(result["exit"])

    def test_problems_index_replaces_missing_report_lines(self):
        report = "Init -> 00000000\nexit=1\n"
        index = ("PBS PROBLEMS v1 t=0 report=capture.txt fw=09000005\n"
                 "PROBLEM n=1 t=2101 sev=fatal stage=open code=80a70008 name=AvcapUnavailable "
                 "errno=0 detail=OpenVideoEx profile=19 failed and is quarantined\n"
                 "SUMMARY problems=1 first=n=1 stage=open code=80a70008 sev=fatal worst=fatal exit=1\n")
        result = diagnose.diagnose(report, index)
        self.assertEqual(result["primary"]["id"], "open")
        self.assertEqual(result["primary"]["code"], 0x80A70008)
        self.assertEqual(result["summary"]["problems"], "1")

    def test_mirrored_problem_lines_are_not_double_counted(self):
        report = OPEN_FAILURE
        index = ("PROBLEM n=1 t=2100 sev=note stage=open code=80a70008 name=AvcapUnavailable "
                 "errno=0 detail=transient 80a70008: 125 bounded retries in 2016ms\n"
                 "PROBLEM n=2 t=2101 sev=fatal stage=open code=80a70008 name=AvcapUnavailable "
                 "errno=0 detail=OpenVideoEx profile=19 failed and is quarantined\n"
                 "SUMMARY problems=2 first=n=1 stage=open code=80a70008 sev=note worst=fatal exit=1\n")
        result = diagnose.diagnose(report, index)
        self.assertEqual(result["counts"]["fatal"], 2)
        self.assertEqual(result["counts"]["note"], 1)

    def test_root_cause_beats_earlier_quarantine_marker(self):
        # Mirrors the payload's real ordering: the quarantine phrase is logged
        # just before its PROBLEM record, but the failing operation is the cause.
        text = ("OpenVideoEx failure quarantined; not reopening an unknown capture slot\n"
                "PROBLEM n=1 t=101 sev=fatal stage=open code=80a70008 name=AvcapUnavailable "
                "errno=0 detail=OpenVideoEx profile=19 failed and is quarantined\n"
                "exit=1\n")
        result = diagnose.diagnose(text)
        self.assertEqual(result["verdict"], "quarantine")
        self.assertEqual(result["primary"]["id"], "open")

    def test_quarantine_only_run_still_names_the_marker(self):
        text = ("Init -> 00000000\n"
                "RemotePlay Term uncertain; remote Avcap reference requires inspection\n"
                "exit=1\n")
        result = diagnose.diagnose(text)
        self.assertEqual(result["verdict"], "quarantine")
        self.assertEqual(result["primary"]["id"], "quarantine")

    def test_repeated_failures_survive_and_recovery_is_not_primary(self):
        # Shape of a real mocked start-failure run: several identical Start
        # failures across reopen attempts, then the exhausted recovery budget.
        text = (
            "PROBLEM n=1 t=52 sev=fail stage=start code=ffffffff name=- errno=0 "
            "detail=Avcap Start failed after valid pre-Start headers\n"
            "PROBLEM n=2 t=216 sev=fail stage=start code=ffffffff name=- errno=0 "
            "detail=Avcap Start failed after valid pre-Start headers\n"
            "PROBLEM n=5 t=545 sev=fatal stage=recovery code=00000000 name=- errno=0 "
            "detail=Avcap recovery incomplete after 3 attempts\n"
            "Start -> ffffffff\n"
            "Avcap recovery incomplete after 3 attempts\n"
            "exit=1\n")
        result = diagnose.diagnose(text)
        self.assertEqual(result["primary"]["id"], "start")
        self.assertEqual(result["counts"]["fail"], 2)
        self.assertEqual(result["counts"]["fatal"], 1)
        self.assertTrue(all(item["cascade"] for item in result["findings"]
                            if item["stage"] == "recovery"))

    def test_cleanup_status_extraction(self):
        result = diagnose.diagnose(CLEAN_RUN)
        self.assertEqual(result["cleanup"],
                         {"stop": True, "close": True, "term": True, "request_release": True})
        broken = CLEAN_RUN.replace("Close -> 00000000", "Close -> 80a70001")
        result = diagnose.diagnose(broken)
        self.assertFalse(result["cleanup"]["close"])
        self.assertFalse(result["cleanup_ok"])

    def test_format_report_names_the_primary_first(self):
        text = diagnose.format_report(diagnose.diagnose(OPEN_FAILURE), "capture.txt")
        self.assertIn("PS4 capture diagnosis: QUARANTINE (exit=1)", text)
        self.assertIn("primary  [fatal] stage=open code=80a70008 name=AvcapUnavailable", text)
        self.assertIn("evidence: capture.txt:", text)


class CommandLineTest(unittest.TestCase):
    def run_cli(self, report_text, problems_text=None):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            report = tmp / "capture.txt"
            report.write_text(report_text, encoding="utf-8")
            command = [sys.executable, str(ROOT / "tools/diagnose_ps4_log.py"), str(report),
                       "--json", str(tmp / "diagnosis.json")]
            if problems_text is not None:
                problems = tmp / "problems.txt"
                problems.write_text(problems_text, encoding="utf-8")
                command += ["--problems", str(problems)]
            completed = subprocess.run(command, capture_output=True, text=True)
            payload = json.loads((tmp / "diagnosis.json").read_text(encoding="utf-8"))
            return completed, payload

    def test_exit_codes_and_json(self):
        completed, payload = self.run_cli(CLEAN_RUN)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(payload["verdict"], "pass")
        completed, payload = self.run_cli(READ_FAILURE)
        self.assertEqual(completed.returncode, 1)
        self.assertEqual(payload["verdict"], "quarantine")
        completed, payload = self.run_cli(CRASH_RUN)
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(payload["verdict"], "crash")

    def test_problems_index_is_used(self):
        index = ("PROBLEM n=1 t=5 sev=fatal stage=init code=80a70001 name=AvcapGeneric "
                 "errno=0 detail=Avcap Init failed before any Open\n"
                 "SUMMARY problems=1 first=n=1 stage=init code=80a70001 sev=fatal worst=fatal exit=1\n")
        completed, payload = self.run_cli("Init -> 80a70001\nexit=1\n", index)
        self.assertEqual(completed.returncode, 1)
        self.assertEqual(payload["primary"]["id"], "init")
        self.assertIn("primary  [fatal] stage=init code=80a70001 name=AvcapGeneric", completed.stdout)


if __name__ == "__main__":
    unittest.main()
