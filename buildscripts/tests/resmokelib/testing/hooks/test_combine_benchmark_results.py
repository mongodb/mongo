#!/usr/bin/env python3
"""Unit tests for the resmokelib.testing.hooks.combine_benchmark_results module."""

import datetime
import unittest

import mock

import buildscripts.resmokelib.testing.hooks.combine_benchmark_results as cbr

# pylint: disable=protected-access
from buildscripts.resmokelib.errors import CedarReportError

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
    "name": "BM_Name1/arg1/arg with space", "run_type": "iteration", "repetition_index": 0,
    "threads": 1, "iterations": 1000, "real_time": 1204, "cpu_time": 1305, "bytes_per_second": 1406,
    "items_per_second": 1507, "custom_counter_1": 1608, "instructions_per_iteration": 101
}

_BM_REPORT_WITH_INSTRUCTIONS_2 = {
    "name": "BM_Name1/arg1/arg with space", "run_type": "iteration", "repetition_index": 0,
    "threads": 2, "iterations": 1000, "real_time": 1202, "cpu_time": 1303, "bytes_per_second": 1404,
    "items_per_second": 1505, "custom_counter_1": 1606, "instructions_per_iteration": 100
}

_BM_REPORT_WITH_INSTRUCTIONS_MEAN = {
    "name": "BM_Name1/arg1/arg with space_mean", "run_type": "aggregate", "repetition_index": 0,
    "threads": 2, "iterations": 1000, "real_time": 1202, "cpu_time": 1303, "bytes_per_second": 1404,
    "items_per_second": 1505, "custom_counter_1": 1606, "instructions_per_iteration": 100,
    "aggregate_name": "mean"
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
    "context":
        _BM_CONTEXT, "benchmarks": [
            _BM_REPORT_1,
            _BM_REPORT_2,
            _BM_MEAN_REPORT,
            _BM_MULTITHREAD_REPORT,
            _BM_MULTITHREAD_MEDIAN_REPORT,
        ]
}

# 12/31/2999 @ 11:59pm (UTC)
_START_TIME = 32503679999

# 01/01/3000 @ 12:00am (UTC)
_END_TIME = 32503680000


class CombineBenchmarkResultsFixture(unittest.TestCase):

    # Mock the hook's parent class because we're testing only functionality of this hook and
    # not anything related to or inherit from the parent class.
    @mock.patch("buildscripts.resmokelib.testing.hooks.interface.Hook", autospec=True)
    def setUp(self, MockHook):  # pylint: disable=arguments-differ,unused-argument
        self.bm_threads_report = cbr._BenchmarkThreadsReport(_BM_CONTEXT)

        self.cbr_hook = cbr.CombineBenchmarkResults(None, None)

        self.cbr_hook.create_time = datetime.datetime.utcfromtimestamp(_START_TIME)
        self.cbr_hook.end_time = datetime.datetime.utcfromtimestamp(_END_TIME)
        self.cbr_hook._parse_report(_BM_FULL_REPORT)


class TestCombineBenchmarkResults(CombineBenchmarkResultsFixture):
    def test_generate_cedar_report(self):
        report = self.cbr_hook._generate_cedar_report()

        self.assertEqual(len(report), 2)
        self.assertEqual(report[0]["info"]["args"]["thread_level"], 1)
        self.assertEqual(len(report[0]["metrics"]), 3)
        self.assertEqual(report[1]["info"]["args"]["thread_level"], 10)
        self.assertEqual(len(report[1]["metrics"]), 2)

    def test_generate_cedar_report_with_dup_metric_names(self):
        # After we parse the same report twice we have duplicated metrics
        self.cbr_hook._parse_report(_BM_FULL_REPORT)

        # self.assertRaises(Exception, self.cbr_hook._generate_cedar_report)
        with self.assertRaisesRegex(CedarReportError, _BM_REPORT_1["name"]):
            self.cbr_hook._generate_cedar_report()


class TestBenchmarkThreadsReport(CombineBenchmarkResultsFixture):
    def test_thread_from_name(self):
        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name/arg name:100/threads:10"})
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name(
            {"name": "BM_Name/arg name:100/threads:10_mean", "aggregate_name": "mean"})
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
            {"name": "BM_Name/1/eeee_mean", "aggregate_name": "mean"})
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
            {"name": "BM_baseline_match_simple/0_mean", "aggregate_name": "mean"})
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_baseline_match_simple/0")

    def test_generate_multithread_cedar_metrics(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MULTITHREAD_REPORT), _BM_MULTITHREAD_REPORT)
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MULTITHREAD_MEDIAN_REPORT),
            _BM_MULTITHREAD_MEDIAN_REPORT)
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(10, cedar_metrics)
        self.assertEqual(len(cedar_metrics[10]), 2)
        collected_types = (m.type for m in cedar_metrics[10])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEDIAN", collected_types)

    def test_generate_single_thread_cedar_metrics(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_1), _BM_REPORT_1)
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_2), _BM_REPORT_2)
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MEAN_REPORT), _BM_MEAN_REPORT)
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
            _BM_REPORT_WITH_INSTRUCTIONS_1)
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_INSTRUCTIONS_2),
            _BM_REPORT_WITH_INSTRUCTIONS_2)
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_INSTRUCTIONS_MEAN),
            _BM_REPORT_WITH_INSTRUCTIONS_MEAN)
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(1, cedar_metrics)
        self.assertEqual(len(cedar_metrics[1]), 2)
        collected_types = (m.type for m in cedar_metrics[2])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEAN", collected_types)
