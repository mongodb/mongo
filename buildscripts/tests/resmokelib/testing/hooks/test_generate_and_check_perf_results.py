#!/usr/bin/env python3
"""Unit tests for the resmokelib.testing.hooks.combine_benchmark_results module."""

import datetime
import logging
import unittest
from typing import Dict, List

import mock

import buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results as cbr
from buildscripts.resmokelib.errors import CedarReportError, ServerFailure
from buildscripts.util.cedar_report import CedarMetric

_BM_CONTEXT = {
    "date": "2018/01/30-18:40:25",
    "executable": "./path/to/exe",
    "num_cpus": 40,
    "mhz_per_cpu": 4999,
    "cpu_scaling_enabled": False,
    "library_build_type": "debug",
    "caches": [],
}

_BM_REPORT_1 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 1,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
}

_BM_REPORT_2 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 1,
    "threads": 1,
    "iterations": 1000,
    "real_time": 1204,
    "cpu_time": 1305,
    "bytes_per_second": 1406,
    "items_per_second": 1507,
    "custom_counter_1": 1608,
}

_BM_REPORT_WITH_INSTRUCTIONS_1 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 1,
    "iterations": 1000,
    "real_time": 1204,
    "cpu_time": 1305,
    "bytes_per_second": 1406,
    "items_per_second": 1507,
    "custom_counter_1": 1608,
    "instructions_per_iteration": 101,
}

_BM_REPORT_WITH_INSTRUCTIONS_2 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 2,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
    "instructions_per_iteration": 100,
}

_BM_REPORT_WITH_INSTRUCTIONS_MEAN = {
    "name": "BM_Name1/arg1/arg with space_mean",
    "run_type": "aggregate",
    "repetition_index": 0,
    "threads": 2,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
    "instructions_per_iteration": 100,
    "aggregate_name": "mean",
}

_BM_REPORT_WITH_CYCLES_1 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 1,
    "iterations": 1000,
    "real_time": 1204,
    "cpu_time": 1305,
    "bytes_per_second": 1406,
    "items_per_second": 1507,
    "custom_counter_1": 1608,
    "cycles_per_iteration": 101,
}

_BM_REPORT_WITH_CYCLES_2 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 2,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
    "cycles_per_iteration": 100,
}

_BM_REPORT_WITH_CYCLES_MEAN = {
    "name": "BM_Name1/arg1/arg with space_mean",
    "run_type": "aggregate",
    "repetition_index": 0,
    "threads": 2,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
    "cycles_per_iteration": 100,
    "aggregate_name": "mean",
}

_BM_MEAN_REPORT = {
    "name": "BM_Name1/arg1/arg with space_mean",
    "run_type": "aggregate",
    "threads": 1,
    "aggregate_name": "mean",
    "iterations": 1000,
    "real_time": 1200,
    "cpu_time": 1300,
    "bytes_per_second": 1400,
    "items_per_second": 1500,
    "custom_counter_1": 1600,
}

_BM_MULTITHREAD_REPORT = {
    "name": "BM_Name2/threads:10",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 10,
    "iterations": 100,
    "real_time": 202,
    "cpu_time": 303,
    "bytes_per_second": 404,
    "items_per_second": 505,
    "custom_counter_1": 606,
}

_BM_MULTITHREAD_MEDIAN_REPORT = {
    "name": "BM_Name2/threads:10_median",
    "run_type": "aggregate",
    "threads": 10,
    "aggregate_name": "median",
    "iterations": 100,
    "real_time": 200,
    "cpu_time": 300,
    "bytes_per_second": 400,
    "items_per_second": 500,
    "custom_counter_1": 600,
}

_BM_FULL_REPORT = {
    "context": _BM_CONTEXT,
    "benchmarks": [
        _BM_REPORT_1,
        _BM_REPORT_2,
        _BM_MEAN_REPORT,
        _BM_MULTITHREAD_REPORT,
        _BM_MULTITHREAD_MEDIAN_REPORT,
    ],
}

_BM_FULL_REPORT_WITH_DUPS = {
    "context": _BM_CONTEXT,
    "benchmarks": [
        _BM_REPORT_1,
        _BM_REPORT_1,
        _BM_REPORT_2,
        _BM_MEAN_REPORT,
        _BM_MULTITHREAD_REPORT,
        _BM_MULTITHREAD_MEDIAN_REPORT,
    ],
}

# 12/31/2999 @ 11:59pm (UTC)
_START_TIME = 32503679999

# 01/01/3000 @ 12:00am (UTC)
_END_TIME = 32503680000


class GenerateAndCheckPerfResultsFixture(unittest.TestCase):
    # Mock the hook's parent class because we're testing only functionality of this hook and
    # not anything related to or inherit from the parent class.
    @mock.patch("buildscripts.resmokelib.testing.hooks.interface.Hook", autospec=True)
    def setUp(self, MockHook):
        self.bm_threads_report = cbr._BenchmarkThreadsReport(_BM_CONTEXT)

        self.cbr_hook = cbr.GenerateAndCheckPerfResults(None, None)

        self.cbr_hook.create_time = datetime.datetime.utcfromtimestamp(_START_TIME)
        self.cbr_hook.end_time = datetime.datetime.utcfromtimestamp(_END_TIME)
        self.cbr_hook._parse_report(_BM_FULL_REPORT)


class TestGenerateAndCheckPerfResults(GenerateAndCheckPerfResultsFixture):
    def test_generate_cedar_report(self):
        report = self.cbr_hook._generate_cedar_report(self.cbr_hook._parse_report(_BM_FULL_REPORT))

        self.assertEqual(len(report), 2)
        self.assertEqual(report[0].thread_level, 1)
        self.assertEqual(len(report[0].metrics), 3)
        self.assertEqual(report[1].thread_level, 10)
        self.assertEqual(len(report[1].metrics), 2)

    def test_generate_cedar_report_with_dup_metric_names(self):
        # After we parse the same report twice we have duplicated metrics
        report = self.cbr_hook._parse_report(_BM_FULL_REPORT_WITH_DUPS)

        # self.assertRaises(Exception, self.cbr_hook._generate_cedar_report)
        with self.assertRaisesRegex(CedarReportError, _BM_REPORT_1["name"]):
            self.cbr_hook._generate_cedar_report(report)


class TestBenchmarkThreadsReport(GenerateAndCheckPerfResultsFixture):
    def test_thread_from_name(self):
        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name/arg name:100/threads:10"})
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name(
            {"name": "BM_Name/arg name:100/threads:10_mean", "aggregate_name": "mean"}
        )
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name/threads:abcd"})
        self.assertEqual(name_obj.thread_count, "abcd")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name"})
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name")

        name_obj = self.bm_threads_report.parse_bm_name(
            {"name": "BM_Name/1/eeee_mean", "aggregate_name": "mean"}
        )
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_Name/1/eeee")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name/arg name:100"})
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_baseline_match_simple/0"})
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_baseline_match_simple/0")

        name_obj = self.bm_threads_report.parse_bm_name(
            {"name": "BM_baseline_match_simple/0_mean", "aggregate_name": "mean"}
        )
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_baseline_match_simple/0")

    def test_generate_multithread_cedar_metrics(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MULTITHREAD_REPORT), _BM_MULTITHREAD_REPORT
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MULTITHREAD_MEDIAN_REPORT),
            _BM_MULTITHREAD_MEDIAN_REPORT,
        )
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(10, cedar_metrics)
        self.assertEqual(len(cedar_metrics[10]), 2)
        collected_types = (m.type for m in cedar_metrics[10])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEDIAN", collected_types)

    def test_generate_single_thread_cedar_metrics(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_1), _BM_REPORT_1
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_2), _BM_REPORT_2
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MEAN_REPORT), _BM_MEAN_REPORT
        )
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(1, cedar_metrics)
        self.assertEqual(len(cedar_metrics[1]), 3)
        collected_types = (m.type for m in cedar_metrics[1])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEAN", collected_types)

    def test_generate_cedar_report_with_instructions(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_INSTRUCTIONS_1),
            _BM_REPORT_WITH_INSTRUCTIONS_1,
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_INSTRUCTIONS_2),
            _BM_REPORT_WITH_INSTRUCTIONS_2,
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_INSTRUCTIONS_MEAN),
            _BM_REPORT_WITH_INSTRUCTIONS_MEAN,
        )
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(1, cedar_metrics)
        self.assertEqual(len(cedar_metrics[1]), 2)
        collected_types = (m.type for m in cedar_metrics[2])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEAN", collected_types)

    def test_generate_cedar_report_with_cycles(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_CYCLES_1), _BM_REPORT_WITH_CYCLES_1
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_CYCLES_2), _BM_REPORT_WITH_CYCLES_2
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_CYCLES_MEAN),
            _BM_REPORT_WITH_CYCLES_MEAN,
        )
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(1, cedar_metrics)
        self.assertEqual(len(cedar_metrics[1]), 2)
        collected_types = (m.type for m in cedar_metrics[2])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEAN", collected_types)


class TestCheckPerfResultTestCase(unittest.TestCase):
    def test_all_metrics_pass(self):
        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=1)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        # We want to make sure this can run without any exceptions.
        test_case.run_test()

    def test_a_metric_fails(self):
        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_comment_allows_failure_issue_comment(self, mock_get_expansion, mock_config):
        """Test that an override comment from an approved user allows a metric failure (issue comment)."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        # Mock GitHub API
        mock_github = mock.MagicMock()
        mock_pr = mock.MagicMock()
        mock_repo = mock.MagicMock()
        mock_comment = mock.MagicMock()
        mock_comment.body = "This is a perf threshold check override comment"
        mock_comment.user.login = "brad-devlugt"

        mock_pr.number = 12345
        mock_pr.get_issue_comments.return_value = [mock_comment]
        mock_pr.get_review_comments.return_value = []
        mock_pr.get_comments.return_value = []
        mock_repo.get_pull.return_value = mock_pr
        mock_github.get_repo.return_value = mock_repo

        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )
        test_case.github = mock_github

        # Should not raise an exception due to override comment
        test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_comment_allows_failure_review_comment(self, mock_get_expansion, mock_config):
        """Test that an override comment from an approved user allows a metric failure (review comment)."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        # Mock GitHub API
        mock_github = mock.MagicMock()
        mock_pr = mock.MagicMock()
        mock_repo = mock.MagicMock()
        mock_comment = mock.MagicMock()
        mock_comment.body = "PERF THRESHOLD CHECK OVERRIDE"  # Test case insensitive
        mock_comment.user.login = "alicedoherty"

        mock_pr.number = 12345
        mock_pr.get_issue_comments.return_value = []
        mock_pr.get_review_comments.return_value = [mock_comment]
        mock_pr.get_comments.return_value = []
        mock_repo.get_pull.return_value = mock_pr
        mock_github.get_repo.return_value = mock_repo

        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )
        test_case.github = mock_github

        # Should not raise an exception due to override comment
        test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_comment_allows_failure_commit_comment(self, mock_get_expansion, mock_config):
        """Test that an override comment from an approved user allows a metric failure (commit comment)."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        # Mock GitHub API
        mock_github = mock.MagicMock()
        mock_pr = mock.MagicMock()
        mock_repo = mock.MagicMock()
        mock_comment = mock.MagicMock()
        mock_comment.body = "perf threshold check override - approved"
        mock_comment.user.login = "samanca"

        mock_pr.number = 12345
        mock_pr.get_issue_comments.return_value = []
        mock_pr.get_review_comments.return_value = []
        mock_pr.get_comments.return_value = [mock_comment]
        mock_repo.get_pull.return_value = mock_pr
        mock_github.get_repo.return_value = mock_repo

        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )
        test_case.github = mock_github

        # Should not raise an exception due to override comment
        test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_comment_from_unauthorized_user_fails(self, mock_get_expansion, mock_config):
        """Test that an override comment from an unauthorized user does not prevent failure."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        # Mock GitHub API
        mock_github = mock.MagicMock()
        mock_pr = mock.MagicMock()
        mock_repo = mock.MagicMock()
        mock_comment = mock.MagicMock()
        mock_comment.body = "perf threshold check override"
        mock_comment.user.login = "unauthorized-user"

        mock_pr.number = 12345
        mock_pr.get_issue_comments.return_value = [mock_comment]
        mock_pr.get_review_comments.return_value = []
        mock_pr.get_comments.return_value = []
        mock_repo.get_pull.return_value = mock_pr
        mock_github.get_repo.return_value = mock_repo

        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )
        test_case.github = mock_github

        # Should raise an exception since user is not authorized
        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_no_override_comment_fails(self, mock_get_expansion, mock_config):
        """Test that a failure without override comment still fails."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        # Mock GitHub API
        mock_github = mock.MagicMock()
        mock_pr = mock.MagicMock()
        mock_repo = mock.MagicMock()
        mock_comment = mock.MagicMock()
        mock_comment.body = "This is a regular comment without override"
        mock_comment.user.login = "brad-devlugt"

        mock_pr.number = 12345
        mock_pr.get_issue_comments.return_value = [mock_comment]
        mock_pr.get_review_comments.return_value = []
        mock_pr.get_comments.return_value = []
        mock_repo.get_pull.return_value = mock_pr
        mock_github.get_repo.return_value = mock_repo

        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )
        test_case.github = mock_github

        # Should raise an exception since there's no override comment
        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_not_checked_when_not_merge_queue(self, mock_get_expansion, mock_config):
        """Test that override checking is skipped when not in merge queue."""
        mock_config.EVERGREEN_REQUESTER = "patch_request"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_token_mongo": "fake_token",
        }.get(key, default)

        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        # Should raise an exception since we're not in merge queue
        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_missing_pr_number_raises_error(self, mock_get_expansion, mock_config):
        """Test that missing PR number raises an error in merge queue."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_token_mongo": "fake_token",
        }.get(key, default)

        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        # Should raise an exception about missing PR number
        with self.assertRaisesRegex(ServerFailure, "github_pr_number"):
            test_case.run_test()

    def test_metric_doesnt_exist(self):
        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="instructions"
            ): CedarMetric(name="instructions", type="LATENCY", value=1)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    def test_thread_level_doesnt_exist(self):
        thresholds_to_check: List[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: Dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=12, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=1)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()
