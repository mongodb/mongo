#!/usr/bin/env python
"""Unit tests for the resmokelib.testing.hooks.combine_benchmark_results module."""

from __future__ import absolute_import

import datetime
import unittest

import mock

import buildscripts.resmokelib.testing.hooks.combine_benchmark_results as cbr

# pylint: disable=missing-docstring,protected-access

_BM_CONTEXT = {
    "date": "2018/01/30-18:40:25", "num_cpus": 40, "mhz_per_cpu": 4999,
    "cpu_scaling_enabled": False, "library_build_type": "debug"
}

_BM_REPORT_1 = {
    "name": "BM_Name1/arg1/arg with space", "iterations": 1000, "real_time": 1202, "cpu_time": 1303,
    "bytes_per_second": 1404, "items_per_second": 1505, "custom_counter_1": 1606
}

_BM_REPORT_2 = {
    "name": "BM_Name1/arg1/arg with space", "iterations": 1000, "real_time": 1204, "cpu_time": 1305,
    "bytes_per_second": 1406, "items_per_second": 1507, "custom_counter_1": 1608
}

_BM_MEAN_REPORT = {
    "name": "BM_Name1/arg1/arg with space_mean", "iterations": 1000, "real_time": 1200,
    "cpu_time": 1300, "bytes_per_second": 1400, "items_per_second": 1500, "custom_counter_1": 1600
}

_BM_MULTITHREAD_REPORT = {
    "name": "BM_Name2/threads:10", "iterations": 100, "real_time": 202, "cpu_time": 303,
    "bytes_per_second": 404, "items_per_second": 505, "custom_counter_1": 606
}

_BM_MULTITHREAD_MEDIAN_REPORT = {
    "name": "BM_Name2/threads:10_median", "iterations": 100, "real_time": 200, "cpu_time": 300,
    "bytes_per_second": 400, "items_per_second": 500, "custom_counter_1": 600
}

_BM_FULL_REPORT = {
    "context":
        _BM_CONTEXT, "benchmarks": [
            _BM_REPORT_1, _BM_REPORT_2, _BM_MEAN_REPORT, _BM_MULTITHREAD_REPORT,
            _BM_MULTITHREAD_MEDIAN_REPORT
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
    def test_generate_reports(self):
        report = self.cbr_hook._generate_perf_plugin_report()

        self.assertEqual(len(report.keys()), 4)
        self.assertEqual(len(report["results"]), 2)

        self.assertDictEqual(report["results"][0]["context"], _BM_CONTEXT)
        self.assertEqual(report["results"][0]["results"]["1"]["ops_per_sec"], -1304.0)

        self.assertEqual(report["start"], "2999-12-31T23:59:59Z")
        self.assertEqual(report["end"], "3000-01-01T00:00:00Z")


class TestBenchmarkThreadsReport(CombineBenchmarkResultsFixture):
    def test_thread_from_name(self):
        name_obj = self.bm_threads_report.parse_bm_name("BM_Name/arg name:100/threads:10")
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name("BM_Name/arg name:100/threads:10_mean")
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name("BM_Name/threads:abcd")
        self.assertEqual(name_obj.thread_count, "abcd")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name")

        name_obj = self.bm_threads_report.parse_bm_name("BM_Name")
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name")

        name_obj = self.bm_threads_report.parse_bm_name("BM_Name/1/eeee_mean")
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_Name/1/eeee")

        name_obj = self.bm_threads_report.parse_bm_name("BM_Name/arg name:100")
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

    def test_generate_multithread_perf_plugin_dict(self):
        # Also test add_report() in the process.
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MULTITHREAD_REPORT["name"]),
            _BM_MULTITHREAD_REPORT)

        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        report = self.bm_threads_report.generate_perf_plugin_dict()

        self.assertEqual(len(report.keys()), 1)
        self.assertIn("10", report.keys())
        self.assertNotIn("10_median", report.keys())

        self.assertEqual(len(report["10"]["error_values"]), 1)
        self.assertEqual(len(report["10"]["ops_per_sec_values"]), 1)
        self.assertEqual(report["10"]["ops_per_sec"], -303.0)

    def test_generate_single_thread_perf_plugin_dict(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_1["name"]), _BM_REPORT_1)

        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_2["name"]), _BM_REPORT_2)

        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        report = self.bm_threads_report.generate_perf_plugin_dict()

        self.assertEqual(len(report.keys()), 1)
        self.assertIn("1", report.keys())
        self.assertNotIn("1_mean", report.keys())