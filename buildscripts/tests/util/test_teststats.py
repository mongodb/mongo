"""Unit tests for the util.teststats module."""

import datetime
from json import JSONDecodeError
import unittest
from unittest.mock import patch

from mock.mock import MagicMock
from requests import Session

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
    def _make_evg_result(test_name="dir/test1.js", num_pass=0, duration=0):
        return teststats_utils.HistoricalTestInformation(
            test_name=test_name,
            avg_duration_pass=duration,
            num_pass=num_pass,
            num_fail=0,
        )


class TestGetStatsFromS3(unittest.TestCase):
    @patch.object(Session, 'get')
    def test_get_stats_from_s3_returns_data(self, mock_get):
        mock_response = MagicMock()
        mock_response.json.return_value = [
            {
                "test_name": "jstests/noPassthroughWithMongod/geo_near_random1.js",
                "num_pass": 74,
                "num_fail": 0,
                "avg_duration_pass": 23.16216216216216,
            },
            {
                "test_name": "shell_advance_cluster_time:ValidateCollections",
                "num_pass": 74,
                "num_fail": 0,
                "avg_duration_pass": 1.662162162162162,
            },
        ]
        mock_get.return_value = mock_response

        result = teststats_utils.get_stats_from_s3("project", "task", "variant")

        self.assertEqual(result, [
            teststats_utils.HistoricalTestInformation(
                test_name="jstests/noPassthroughWithMongod/geo_near_random1.js",
                num_pass=74,
                num_fail=0,
                avg_duration_pass=23.16216216216216,
            ),
            teststats_utils.HistoricalTestInformation(
                test_name="shell_advance_cluster_time:ValidateCollections",
                num_pass=74,
                num_fail=0,
                avg_duration_pass=1.662162162162162,
            ),
        ])

    @patch.object(Session, 'get')
    def test_get_stats_from_s3_json_decode_error(self, mock_get):
        mock_response = MagicMock()
        mock_response.json.side_effect = JSONDecodeError("msg", "doc", 0)
        mock_get.return_value = mock_response

        result = teststats_utils.get_stats_from_s3("project", "task", "variant")

        self.assertEqual(result, [])
