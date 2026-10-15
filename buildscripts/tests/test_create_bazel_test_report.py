"""Unit tests for buildscripts/create_bazel_test_report.py."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from buildscripts import create_bazel_test_report as under_test

TEST_XML = """<?xml version="1.0" encoding="utf-8"?>
<testsuites>
  <testsuite name="mongo_unittest" tests="1" failures="0" errors="0">
    <testcase name="mongo/db/index_builds/index_builds_test.exe" time="1.234"/>
  </testsuite>
</testsuites>
"""

# Non-ASCII output is common in test logs (e.g. unicode test data). Reading with
# the Windows default encoding (cp1252) used to raise UnicodeDecodeError and lose
# the whole report.
TEST_LOG_UTF8 = "running test… output: café ✓ ñ"

TEST_LOG_ASCII = "ok"


class TestCreateBazelTestReport(unittest.TestCase):
    def _write_testlogs(self, tmpdir: str, test_log: str) -> Path:
        testlog_dir = Path(
            tmpdir, "bazel-testlogs", "src", "mongo", "db", "index_builds", "index_builds_test"
        )
        testlog_dir.mkdir(parents=True)
        (testlog_dir / "test.xml").write_text(TEST_XML, encoding="utf-8")
        (testlog_dir / "test.log").write_text(test_log, encoding="utf-8")
        return testlog_dir

    def _run_main(self, tmpdir: str):
        original_cwd = os.getcwd()
        try:
            os.chdir(tmpdir)
            with (
                patch.object(under_test, "get_expansion", return_value="true"),
                patch.object(under_test.time, "time", return_value=1000.0),
            ):
                under_test.main("bazel-testlogs/")
        finally:
            os.chdir(original_cwd)

    def test_reads_utf8_logs_regardless_of_platform_default_encoding(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self._write_testlogs(tmpdir, TEST_LOG_UTF8)

            self._run_main(tmpdir)

            report = json.loads(Path(tmpdir, "report.json").read_text(encoding="utf-8"))
            self.assertEqual(1, len(report["results"]))
            self.assertEqual("pass", report["results"][0]["status"])
            self.assertIn("café ✓ ñ", report["results"][0]["log_raw"])

    def test_ascii_log(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self._write_testlogs(tmpdir, TEST_LOG_ASCII)

            self._run_main(tmpdir)

            report = json.loads(Path(tmpdir, "report.json").read_text(encoding="utf-8"))
            self.assertEqual(1, len(report["results"]))

    def test_preserves_fractional_duration(self):
        for duration in ("1.234", "0.125"):
            with self.subTest(duration=duration), tempfile.TemporaryDirectory() as tmpdir:
                testlog_dir = self._write_testlogs(tmpdir, TEST_LOG_ASCII)
                (testlog_dir / "test.xml").write_text(
                    TEST_XML.replace('time="1.234"', f'time="{duration}"'), encoding="utf-8"
                )

                self._run_main(tmpdir)

                report = json.loads(Path(tmpdir, "report.json").read_text(encoding="utf-8"))
                result = report["results"][0]
                self.assertAlmostEqual(float(duration), result["end"] - result["start"])

    def test_no_report_without_results(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            Path(tmpdir, "bazel-testlogs").mkdir()

            self._run_main(tmpdir)

            self.assertFalse(Path(tmpdir, "report.json").exists())

    def test_skips_when_expansion_unset(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self._write_testlogs(tmpdir, TEST_LOG_ASCII)

            original_cwd = os.getcwd()
            try:
                os.chdir(tmpdir)
                with patch.object(under_test, "get_expansion", return_value=None):
                    under_test.main("bazel-testlogs/")
            finally:
                os.chdir(original_cwd)

            self.assertFalse(Path(tmpdir, "report.json").exists())


if __name__ == "__main__":
    unittest.main()
