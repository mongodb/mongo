#!/usr/bin/env python3
"""Unit tests for the resmokelib.testing.hooks.combine_benchrun_embedded_results module."""

import datetime
import os
import unittest

import mock

import buildscripts.resmokelib.testing.hooks.combine_benchrun_embedded_results as cber

# pylint: disable=missing-docstring,protected-access,attribute-defined-outside-init

_BM_REPORT_INSERT_1 = {
    "note": "values per second", "errCount": {"$numberLong": "0"},
    "trapped": "error: not implemented", "insertLatencyAverageMicros": 389.4926654182272,
    "totalOps": {"$numberLong": "12816"}, "totalOps/s": 2563.095938304905, "findOne": 0,
    "insert": 2563.095938304905, "delete": 0, "update": 0, "query": 0, "command": 0, "findOnes": {
        "$numberLong": "0"
    }, "inserts": {"$numberLong": "12816"}, "deletes": {"$numberLong": "0"}, "updates": {
        "$numberLong": "0"
    }, "queries": {"$numberLong": "0"}, "commands": {"$numberLong": "0"}
}

_BM_REPORT_INSERT_2 = {
    "note": "values per second", "errCount": {"$numberLong": "0"}, "trapped":
        "error: not implemented", "insertLatencyAverageMicros": 2563.095938304905, "totalOps": {
            "$numberLong": "7404"
        }, "totalOps/s": 2409.05, "findOne": 0, "insert": 2409.05, "delete": 0, "update": 0,
    "query": 0, "command": 0, "findOnes": {"$numberLong": "0"}, "inserts": {"$numberLong": "7404"},
    "deletes": {"$numberLong": "0"}, "updates": {"$numberLong": "0"},
    "queries": {"$numberLong": "0"}, "commands": {"$numberLong": "0"}
}

_BM_REPORT_DELETE = {
    "note": "values per second", "errCount": {"$numberLong": "0"},
    "trapped": "error: not implemented", "insertLatencyAverageMicros": "1234.56", "totalOps": {
        "$numberLong": "2345"
    }, "totalOps/s": 1234.56, "findOne": 0, "insert": 0, "delete": 1234.56, "update": 0, "query":
        0, "command": 0, "findOnes": {"$numberLong": "0"}, "inserts": {"$numberLong": "0"},
    "deletes": {"$numberLong": "2345"}, "updates": {"$numberLong": "0"},
    "queries": {"$numberLong": "0"}, "commands": {"$numberLong": "0"}
}

_BM_REPORT_UPDATE = {
    "note": "values per second", "errCount": {"$numberLong": "0"},
    "trapped": "error: not implemented", "insertLatencyAverageMicros": 654.321, "totalOps": {
        "$numberLong": "4521"
    }, "totalOps/s": 4521.00, "findOne": 0, "insert": 0, "delete": 0, "update": 4521.00, "query":
        0, "command": 0, "findOnes": {"$numberLong": "0"}, "inserts": {"$numberLong": "0"},
    "deletes": {"$numberLong": "0"}, "updates": {"$numberLong": "4521"},
    "queries": {"$numberLong": "0"}, "commands": {"$numberLong": "0"}
}

_BM_REPORT_MULTI = {
    "note": "values per second", "errCount": {"$numberLong": "0"}, "trapped":
        "error: not implemented", "insertLatencyAverageMicros": 111.111,
    "totalOps": {"$numberLong": "11532"}, "totalOps/s": 5766.00, "findOne": 0, "insert": 2490.00,
    "delete": 0, "update": 9042.00, "query": 0, "command": 0, "findOnes": {"$numberLong": "0"},
    "inserts": {"$numberLong": "2490.00"}, "deletes": {"$numberLong": "0"}, "updates": {
        "$numberLong": "9042"
    }, "queries": {"$numberLong": "0"}, "commands": {"$numberLong": "0"}
}

_BM_ALL_REPORTS = [
    _BM_REPORT_INSERT_1, _BM_REPORT_INSERT_2, _BM_REPORT_DELETE, _BM_REPORT_UPDATE, _BM_REPORT_MULTI
]

# 12/31/2999 @ 11:59pm (UTC)
_START_TIME = 32503679999

# 01/01/3000 @ 12:00am (UTC)
_END_TIME = 32503680000


class CombineBenchrunEmbeddedResultsFixture(unittest.TestCase):

    # Mock the hook's parent class because we're testing only functionality of this hook and
    # not anything related to or inherit from the parent class.
    @mock.patch("buildscripts.resmokelib.testing.hooks.interface.Hook", autospec=True)
    def setUp(self, MockHook):  # pylint: disable=arguments-differ,unused-argument
        self.cber_hook = cber.CombineBenchrunEmbeddedResults(None, None)
        self.cber_hook.create_time = datetime.datetime.utcfromtimestamp(_START_TIME)
        self.cber_hook.end_time = datetime.datetime.utcfromtimestamp(_END_TIME)


class TestCombineBenchmarkResults(CombineBenchrunEmbeddedResultsFixture):
    def _setup_reports(self, reports, test_name, num_threads):
        self.total_ops_per_sec = 0
        self.num_tests = len(reports)
        self.cber_hook.benchmark_reports[test_name] = cber._BenchrunEmbeddedThreadsReport()
        for rep in reports:
            self.cber_hook.benchmark_reports[test_name].add_report(num_threads, rep)
            self.total_ops_per_sec += rep["totalOps/s"]
        self.ops_per_sec = self.total_ops_per_sec / self.num_tests
        self.report = self.cber_hook._generate_perf_plugin_report()

    def test_generate_one_report(self):
        test_name = "test_cber1"
        num_threads = "2"
        self._setup_reports([_BM_REPORT_MULTI], test_name, num_threads)
        report_0 = self.report["results"][0]
        self.assertEqual(report_0["name"], test_name)
        self.assertEqual(report_0["results"][str(num_threads)]["ops_per_sec"], self.ops_per_sec)

    def test_generate_all_reports(self):
        test_name = "test_cber2"
        thread_num = "1"
        self._setup_reports(_BM_ALL_REPORTS, test_name, thread_num)
        self.assertEqual(len(list(self.report.keys())), 4)
        report_0 = self.report["results"][0]
        self.assertEqual(report_0["name"], test_name)
        self.assertEqual(report_0["results"][thread_num]["ops_per_sec"], self.ops_per_sec)
        self.assertEqual(self.report["start"], "2999-12-31T23:59:59Z")
        self.assertEqual(self.report["end"], "3000-01-01T00:00:00Z")

    def test_parse_report_name(self):
        self.cber_hook.report_root = os.path.join("benchrun_embedded", "results")
        test_name = "test1"
        thread_num = 3
        file_name = os.path.join(self.cber_hook.report_root, test_name,
                                 "thread{}".format(thread_num), "mongoebench.0.json")
        report_threads = self.cber_hook._parse_report_name(file_name)
        self.assertEqual(thread_num, int(report_threads))


class TestBenchrunEmbeddedThreadsReport(CombineBenchrunEmbeddedResultsFixture):
    def test_generate_single_thread_perf_plugin_dict(self):
        thread_report = cber._BenchrunEmbeddedThreadsReport()
        thread_num = "1"
        thread_report.add_report(thread_num, _BM_REPORT_INSERT_1)
        perf_report = thread_report.generate_perf_plugin_dict()
        self.assertEqual(len(list(perf_report.keys())), 1)
        self.assertEqual(perf_report[thread_num]["ops_per_sec"], _BM_REPORT_INSERT_1["totalOps/s"])
        self.assertEqual(len(perf_report[thread_num]["ops_per_sec_values"]), 1)

        thread_report.add_report(thread_num, _BM_REPORT_INSERT_2)
        perf_report = thread_report.generate_perf_plugin_dict()
        self.assertEqual(len(list(perf_report.keys())), 1)
        ops_per_sec = (_BM_REPORT_INSERT_1["totalOps/s"] + _BM_REPORT_INSERT_2["totalOps/s"]) / 2
        self.assertEqual(perf_report[thread_num]["ops_per_sec"], ops_per_sec)
        self.assertEqual(len(perf_report[thread_num]["ops_per_sec_values"]), 2)

    def test_generate_multi_thread_perf_plugin_dict(self):
        thread_report = cber._BenchrunEmbeddedThreadsReport()
        thread_num = "1"
        thread_report.add_report(thread_num, _BM_REPORT_INSERT_1)
        perf_report = thread_report.generate_perf_plugin_dict()
        self.assertEqual(len(list(perf_report.keys())), 1)
        self.assertEqual(perf_report[thread_num]["ops_per_sec"], _BM_REPORT_INSERT_1["totalOps/s"])
        self.assertEqual(len(perf_report[thread_num]["ops_per_sec_values"]), 1)

        thread_num = "2"
        thread_report.add_report(thread_num, _BM_REPORT_INSERT_2)
        perf_report = thread_report.generate_perf_plugin_dict()
        self.assertEqual(len(list(perf_report.keys())), 2)
        self.assertEqual(perf_report["1"]["ops_per_sec"], _BM_REPORT_INSERT_1["totalOps/s"])
        self.assertEqual(len(perf_report["1"]["ops_per_sec_values"]), 1)
        self.assertEqual(perf_report[thread_num]["ops_per_sec"], _BM_REPORT_INSERT_2["totalOps/s"])
        self.assertEqual(len(perf_report[thread_num]["ops_per_sec_values"]), 1)
