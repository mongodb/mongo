"""Unit tests for the util.teststats module."""

import datetime
import unittest
from json import JSONDecodeError
from unittest.mock import patch

from mock import Mock
from mock.mock import MagicMock
from requests import Session

import buildscripts.util.teststats as under_test

_DATE = datetime.datetime(2018, 7, 15)


class NormalizeTestNameTest(unittest.TestCase):
    def test_unix_names(self):
        self.assertEqual("/home/user/test.js", under_test.normalize_test_name("/home/user/test.js"))

    def test_windows_names(self):
        self.assertEqual(
            "/home/user/test.js", under_test.normalize_test_name("\\home\\user\\test.js")
        )


class TestHistoricTestInfo(unittest.TestCase):
    def test_total_test_runtime_not_passing_test_no_hooks(self):
        test_info = under_test.HistoricTestInfo(
            test_name="jstests/test.js",
            num_pass=0,
            avg_duration=0.0,
            hooks=[],
        )

        self.assertEqual(0.0, test_info.total_test_runtime())

    def test_total_test_runtime_not_passing_test_with_hooks(self):
        test_info = under_test.HistoricTestInfo(
            test_name="jstests/test.js",
            num_pass=0,
            avg_duration=0.0,
            hooks=[
                under_test.HistoricHookInfo(
                    hook_id="test:hook",
                    num_pass=10,
                    avg_duration=5.0,
                ),
            ],
        )

        self.assertEqual(0.0, test_info.total_test_runtime())

    def test_total_test_runtime_passing_test_no_hooks(self):
        test_info = under_test.HistoricTestInfo(
            test_name="jstests/test.js",
            num_pass=10,
            avg_duration=23.0,
            hooks=[],
        )

        self.assertEqual(23.0, test_info.total_test_runtime())

    def test_total_test_runtime_passing_test_with_hooks(self):
        test_info = under_test.HistoricTestInfo(
            test_name="jstests/test.js",
            num_pass=10,
            avg_duration=23.0,
            hooks=[
                under_test.HistoricHookInfo(
                    hook_id="test:hook",
                    num_pass=10,
                    avg_duration=5.0,
                ),
            ],
        )

        self.assertEqual(28.0, test_info.total_test_runtime())


class TestHistoricTaskData(unittest.TestCase):
    def test_no_hooks(self):
        evg_results = [
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 20),
        ]
        test_stats = under_test.HistoricTaskData.from_stats_list(evg_results)
        expected_runtimes = [
            under_test.TestRuntime(test_name="dir/test2.js", runtime=30),
            under_test.TestRuntime(test_name="dir/test1.js", runtime=20),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    def test_hooks(self):
        evg_results = [
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 30),
            self._make_evg_result("dir/test3.js", 5, 10),
            self._make_evg_result("test2:HelloDelays", 2, 30),
            self._make_evg_result("test3:Validate", 5, 30),
            self._make_evg_result("test3:CheckReplDBHash", 5, 35),
        ]
        test_stats = under_test.HistoricTaskData.from_stats_list(evg_results)
        expected_runtimes = [
            under_test.TestRuntime(test_name="dir/test2.js", runtime=90),
            under_test.TestRuntime(test_name="dir/test3.js", runtime=75),
            under_test.TestRuntime(test_name="dir/test1.js", runtime=30),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    def test_hook_first(self):
        evg_results = [
            self._make_evg_result("test3:Validate", 5, 35),
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 25),
            self._make_evg_result("dir/test3.js", 5, 10),
            self._make_evg_result("test3:CheckReplDBHash", 5, 35),
        ]
        test_stats = under_test.HistoricTaskData.from_stats_list(evg_results)
        expected_runtimes = [
            under_test.TestRuntime(test_name="dir/test3.js", runtime=80),
            under_test.TestRuntime(test_name="dir/test2.js", runtime=30),
            under_test.TestRuntime(test_name="dir/test1.js", runtime=25),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    def test_zero_runs(self):
        evg_results = [
            self._make_evg_result("dir/test1.js", 0, 0),
        ]
        test_stats = under_test.HistoricTaskData.from_stats_list(evg_results)
        expected_runtimes = [
            under_test.TestRuntime(test_name="dir/test1.js", runtime=0),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    @staticmethod
    def _make_evg_result(test_file="dir/test1.js", num_pass=0, duration=0):
        return Mock(
            test_name=test_file,
            task_name="task1",
            variant="variant1",
            distro="distro1",
            date=_DATE,
            num_pass=num_pass,
            num_fail=0,
            avg_duration_pass=duration,
        )

    @patch.object(Session, "get")
    def test_get_stats_from_s3_returns_data(self, mock_get):
        mock_response = MagicMock()
        mock_response.json.return_value = [
            {
                "test_name": "jstests/noPassthroughWithMongod/query/geo/geo_near_random1.js",
                "num_pass": 74,
                "num_fail": 0,
                "avg_duration_pass": 23.16216216216216,
                "max_duration_pass": 27.123,
            },
            {
                "test_name": "shell_advance_cluster_time:ValidateCollections",
                "num_pass": 74,
                "num_fail": 0,
                "avg_duration_pass": 1.662162162162162,
                "max_duration_pass": 100.0987,
            },
        ]
        mock_get.return_value = mock_response

        result = under_test.HistoricTaskData.get_stats_from_s3("project", "task", "variant")

        self.assertEqual(
            result,
            [
                under_test.HistoricalTestInformation(
                    test_name="jstests/noPassthroughWithMongod/query/geo/geo_near_random1.js",
                    num_pass=74,
                    num_fail=0,
                    avg_duration_pass=23.16216216216216,
                    max_duration_pass=27.123,
                ),
                under_test.HistoricalTestInformation(
                    test_name="shell_advance_cluster_time:ValidateCollections",
                    num_pass=74,
                    num_fail=0,
                    avg_duration_pass=1.662162162162162,
                    max_duration_pass=100.0987,
                ),
            ],
        )

    @patch.object(Session, "get")
    def test_get_stats_from_s3_json_decode_error(self, mock_get):
        mock_response = MagicMock()
        mock_response.json.side_effect = JSONDecodeError("msg", "doc", 0)
        mock_get.return_value = mock_response

        result = under_test.HistoricTaskData.get_stats_from_s3("project", "task", "variant")

        self.assertEqual(result, [])
