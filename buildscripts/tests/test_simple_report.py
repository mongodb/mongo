"""Simple_report test."""
import unittest
import random
import textwrap
import sys
import os
from unittest.mock import patch, mock_open

from click.testing import CliRunner

import buildscripts.simple_report


def ns(name):  # pylint: disable=invalid-name
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
            self._test_trivial_report()  # pylint: disable=no-value-for-parameter

    @patch(ns("_try_combine_reports"))
    @patch(ns("_clean_log_file"))
    @patch(ns("_put_report"))
    def _test_trivial_report(self, mock_put_report, mock_clean_log_file, _mock_try_combine_reports):
        exit_code = self.rng.randint(0, 254)
        print(f"Trying exit code: {exit_code}")
        mock_clean_log_file.return_value = "I'm a little test log, short and stdout."
        runner = CliRunner()
        result = runner.invoke(
            buildscripts.simple_report.main,
            ["--test-name", "potato", "--log-file", "test.log", "--exit-code",
             str(exit_code)])
        report = mock_put_report.call_args[0][0]
        results = mock_put_report.call_args[0][0]["results"]
        if exit_code == 0:
            self.assertEqual(results[0]["status"], "pass")
            self.assertEqual(report["failures"], 0)
        else:
            self.assertEqual(results[0]["status"], "fail")
            self.assertEqual(report["failures"], 1)
        self.assertEqual(result.exit_code, 0)

    @patch(ns("_try_combine_reports"))
    @patch(ns("_put_report"))
    def test_truncate_scons(self, mock_put_report, _mock_try_combine_reports):
        exit_code = 0
        data = fix_newlines(
            textwrap.dedent("""\
TO BE TRUNCATED
TO BE TRUNCATED
TO BE TRUNCATED
TO BE TRUNCATED
scons: done reading SConscript files.
scons: Building targets ...
interesting part"""))

        with patch("builtins.open", mock_open(read_data=data)) as _mock_file:
            runner = CliRunner()
            result = runner.invoke(
                buildscripts.simple_report.main,
                ["--test-name", "potato", "--log-file", "test.log", "--exit-code",
                 str(exit_code)])
        report = mock_put_report.call_args[0][0]
        results = mock_put_report.call_args[0][0]["results"]
        self.assertEqual(results[0]["status"], "pass")
        self.assertEqual(results[0]["log_raw"], "interesting part")
        self.assertEqual(report["failures"], 0)
        self.assertEqual(result.exit_code, 0)

    @patch(ns("_try_combine_reports"))
    @patch(ns("_put_report"))
    def test_non_scons_log(self, mock_put_report, _mock_try_combine_reports):
        exit_code = 0
        data = fix_newlines(
            textwrap.dedent("""\
*NOT* TO BE TRUNCATED
*NOT* TO BE TRUNCATED
*NOT* TO BE TRUNCATED
*NOT* TO BE TRUNCATED
interesting part"""))

        with patch("builtins.open", mock_open(read_data=data)) as _mock_file:
            runner = CliRunner()
            result = runner.invoke(
                buildscripts.simple_report.main,
                ["--test-name", "potato", "--log-file", "test.log", "--exit-code",
                 str(exit_code)])
        report = mock_put_report.call_args[0][0]
        results = mock_put_report.call_args[0][0]["results"]
        self.assertEqual(results[0]["status"], "pass")
        self.assertEqual(results[0]["log_raw"], data)
        self.assertEqual(report["failures"], 0)
        self.assertEqual(result.exit_code, 0)
