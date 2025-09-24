"""Simple_report test."""

import os
import random
import sys
import unittest
from unittest.mock import patch

from click.testing import CliRunner

import buildscripts.simple_report


def ns(name):
    return f"buildscripts.simple_report.{name}"


def fix_newlines(string: str) -> str:
    # These need to be CRLF newlines on Windows, so we split and rejoin
    # on os.linesep to fix that
    return os.linesep.join(string.splitlines())


class TestSimpleReport(unittest.TestCase):
    def __init__(self, *args, **kwargs):
        super(TestSimpleReport, self).__init__(*args, **kwargs)
        self.seed = random.randrange(sys.maxsize)
        self.rng = random.Random(self.seed)

    def test_trivial_report(self):
        """Run test 100x with randomly generated error codes."""
        print(f"TestSimpleReport.test_trivial_report seed: {self.seed}")
        for _ in range(0, 100):
            self._test_trivial_report()

    @patch(ns("try_combine_reports"))
    @patch(ns("_clean_log_file"))
    @patch(ns("put_report"))
    def _test_trivial_report(self, mock_put_report, mock_clean_log_file, _mock_try_combine_reports):
        exit_code = self.rng.randint(0, 254)
        print(f"Trying exit code: {exit_code}")
        mock_clean_log_file.return_value = "I'm a little test log, short and stdout."
        runner = CliRunner()
        result = runner.invoke(
            buildscripts.simple_report.main,
            ["--test-name", "potato", "--log-file", "test.log", "--exit-code", str(exit_code)],
        )
        report = mock_put_report.call_args[0][0]
        results = mock_put_report.call_args[0][0]["results"]
        if exit_code == 0:
            self.assertEqual(results[0]["status"], "pass")
            self.assertEqual(report["failures"], 0)
        else:
            self.assertEqual(results[0]["status"], "fail")
            self.assertEqual(report["failures"], 1)
        self.assertEqual(result.exit_code, 0)
