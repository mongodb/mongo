"""Unit tests for the util.teststats module."""

import datetime
import unittest

from mock import Mock

import buildscripts.util.teststats as teststats_utils

# pylint: disable=missing-docstring

_DATE = datetime.datetime(2018, 7, 15)


class NormalizeTestNameTest(unittest.TestCase):
    def test_unix_names(self):
        self.assertEqual("/home/user/test.js",
                         teststats_utils.normalize_test_name("/home/user/test.js"))

    def test_windows_names(self):
        self.assertEqual("/home/user/test.js",
                         teststats_utils.normalize_test_name("\\home\\user\\test.js"))


class TestTestStats(unittest.TestCase):
    def test_no_hooks(self):
        evg_results = [
            self._make_evg_result("dir/test1.js", 1, 10),
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 25),
        ]
        test_stats = teststats_utils.TestStats(evg_results)
        expected_runtimes = [
            teststats_utils.TestRuntime(test_name="dir/test2.js", runtime=30),
            teststats_utils.TestRuntime(test_name="dir/test1.js", runtime=20),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    def test_hooks(self):
        evg_results = [
            self._make_evg_result("dir/test1.js", 1, 10),
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 25),
            self._make_evg_result("dir/test3.js", 5, 10),
            self._make_evg_result("test3:CleanEveryN", 10, 30),
            self._make_evg_result("test3:CheckReplDBHash", 10, 35),
        ]
        test_stats = teststats_utils.TestStats(evg_results)
        expected_runtimes = [
            teststats_utils.TestRuntime(test_name="dir/test3.js", runtime=75),
            teststats_utils.TestRuntime(test_name="dir/test2.js", runtime=30),
            teststats_utils.TestRuntime(test_name="dir/test1.js", runtime=20),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    def test_hook_first(self):
        evg_results = [
            self._make_evg_result("test3:CleanEveryN", 10, 35),
            self._make_evg_result("dir/test1.js", 1, 10),
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 25),
            self._make_evg_result("dir/test3.js", 5, 10),
            self._make_evg_result("test3:CheckReplDBHash", 10, 35),
        ]
        test_stats = teststats_utils.TestStats(evg_results)
        expected_runtimes = [
            teststats_utils.TestRuntime(test_name="dir/test3.js", runtime=80),
            teststats_utils.TestRuntime(test_name="dir/test2.js", runtime=30),
            teststats_utils.TestRuntime(test_name="dir/test1.js", runtime=20),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    def test_zero_runs(self):
        evg_results = [
            self._make_evg_result("dir/test1.js", 0, 0),
            self._make_evg_result("dir/test1.js", 0, 0),
        ]
        test_stats = teststats_utils.TestStats(evg_results)
        expected_runtimes = [
            teststats_utils.TestRuntime(test_name="dir/test1.js", runtime=0),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    @staticmethod
    def _make_evg_result(test_file="dir/test1.js", num_pass=0, duration=0):
        return Mock(
            test_file=test_file,
            task_name="task1",
            variant="variant1",
            distro="distro1",
            date=_DATE,
            num_pass=num_pass,
            num_fail=0,
            avg_duration_pass=duration,
        )
