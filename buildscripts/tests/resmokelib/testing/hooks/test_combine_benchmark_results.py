#!/usr/bin/env python

from __future__ import absolute_import

import datetime
import unittest

import mock

import buildscripts.resmokelib.config as _config
import buildscripts.resmokelib.testing.hooks.combine_benchmark_results as cbr

_BM_CONTEXT = {
    "date": "2018/01/30-18:40:25",
    "num_cpus": 40,
    "mhz_per_cpu": 4999,
    "cpu_scaling_enabled": False,
    "library_build_type": "debug"
}

_BM_REPORT = {
    "name": "BM_Name/threads:10",
    "iterations": 100,
    "real_time": 202,
    "cpu_time": 303,
    "bytes_per_second": 404,
    "items_per_second": 505,
    "custom_counter_1": 606
}

_BM_MEDIAN_REPORT = {
    "name": "BM_Name/threads:10_median",
    "iterations": 100,
    "real_time": 200,
    "cpu_time": 300,
    "bytes_per_second": 400,
    "items_per_second": 500,
    "custom_counter_1": 600
}

_BM_FULL_REPORT = {
    "context": _BM_CONTEXT,
    "benchmarks": [
        _BM_REPORT,
        _BM_MEDIAN_REPORT
    ]
}

# 12/31/2999 @ 11:59pm (UTC)
_START_TIME = 32503679999

# 01/01/3000 @ 12:00am (UTC)
_END_TIME = 32503680000


class CombineBenchmarkResultsFixture(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        _config.EVERGREEN_REVISION = "evergreen_revision"
        _config.EVERGREEN_PATCH_BUILD = False
        _config.EVERGREEN_PROJECT_NAME = "evergreen_project_name"
        _config.EVERGREEN_TASK_ID = "evergreen_task_id"
        _config.EVERGREEN_TASK_NAME = "evergreen_task_name"
        _config.EVERGREEN_VARIANT_NAME = "evergreen_variant_name"

    # Mock the hook's parent class because we're testing only functionality of this hook and
    # not anything related to or inherit from the parent class.
    @mock.patch("buildscripts.resmokelib.testing.hooks.interface.Hook", autospec=True)
    def setUp(self, MockHook):
        self.bm_threads_report = cbr._BenchmarkThreadsReport(_BM_CONTEXT)

        self.cbr_hook = cbr.CombineBenchmarkResults(None, None)

        self.cbr_hook.create_time = datetime.datetime.utcfromtimestamp(_START_TIME)
        self.cbr_hook.end_time = datetime.datetime.utcfromtimestamp(_END_TIME)
        self.cbr_hook._parse_report(_BM_FULL_REPORT)


class TestCombineBenchmarkResults(CombineBenchmarkResultsFixture):

    def test_generate_reports(self):
        report = self.cbr_hook._generate_perf_plugin_report()

        self.assertEqual(len(report.keys()), 12)
        self.assertEqual(len(report["data"]["results"]), 1)

        self.assertDictEqual(report["data"]["results"][0]["context"], _BM_CONTEXT)

        self.assertEqual(report["create_time"], "2999-12-31T23:59:59Z")
        self.assertEqual(report["data"]["end"], "3000-01-01T00:00:00Z")


class TestBenchmarkThreadsReport(CombineBenchmarkResultsFixture):

    def test_thread_from_name(self):
        thread = self.bm_threads_report._thread_from_name("BM_Name/arg name:100/threads:10")
        self.assertEqual(thread, "10")

        thread = self.bm_threads_report._thread_from_name("BM_Name/arg name:100/threads:10_mean")
        self.assertEqual(thread, "10_mean")

    def test_generate_perf_plugin_dict(self):
        # Also test add_report() in the process.
        self.bm_threads_report.add_report(_BM_REPORT)
        self.bm_threads_report.add_report(_BM_MEDIAN_REPORT)
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 2)

        report = self.bm_threads_report.generate_perf_plugin_dict()

        self.assertIn("10", report.keys())
        self.assertIn("10_median", report.keys())

        self.assertEqual(len(report["10"]["error_values"]), 1)
        self.assertEqual(len(report["10"]["ops_per_sec_values"]), 1)
        self.assertEqual(report["10"]["ops_per_sec"], -303.0)

        self.assertEqual(len(report["10_median"]["error_values"]), 1)
        self.assertEqual(len(report["10_median"]["ops_per_sec_values"]), 1)
        self.assertEqual(report["10_median"]["ops_per_sec"], -300.0)


if __name__ == "__main__":
    unittest.main()
