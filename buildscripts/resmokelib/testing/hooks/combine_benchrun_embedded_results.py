"""Module for generating the test results file fed into the perf plugin."""

from __future__ import absolute_import
from __future__ import division

import collections
import datetime
import glob
import json
import os

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.testing.hooks import combine_benchmark_results as cbr


class CombineBenchrunEmbeddedResults(cbr.CombineBenchmarkResults):
    """CombineBenchrunEmbeddedResults class.

    The CombineBenchrunEmbeddedResults hook combines test results from
    individual benchmark embedded files to a single file. This is useful for
    generating the json file to feed into the Evergreen performance
    visualization plugin.
    """

    DESCRIPTION = "Combine JSON results from embedded benchrun"

    def before_test(self, test, test_report):
        """Remove any existing mongoebench reports for this test."""
        for bm_report in self._test_result_files(test):
            os.remove(bm_report)

    def after_test(self, test, test_report):
        """Update test report."""
        for bm_report in self._test_result_files(test):
            test_name, thread_count = self._parse_report_name(bm_report)
            with open(bm_report, "r") as report_file:
                report_dict = json.load(report_file)
                if test_name not in self.benchmark_reports:
                    self.benchmark_reports[test_name] = _BenchrunEmbeddedThreadsReport()
                self.benchmark_reports[test_name].add_report(thread_count, report_dict)

    def before_suite(self, test_report):
        """Set suite start time."""
        self.create_time = datetime.datetime.now()
        # Remove any existing perf reports.
        if self.report_file and os.path.isfile(self.report_file):
            os.remove(self.report_file)

    def _generate_perf_plugin_report(self):
        """Format the data to look like a perf plugin report."""
        perf_report = {
            "start": self._strftime(self.create_time),
            "end": self._strftime(self.end_time),
            "errors": [],  # There are no errors if we have gotten this far.
            "results": []
        }

        for name, report in self.benchmark_reports.items():
            test_report = {"name": name, "results": report.generate_perf_plugin_dict()}

            perf_report["results"].append(test_report)

        return perf_report

    @staticmethod
    def _test_result_files(test):
        """Return a list of existing test result files based on the test.short_name()."""
        return glob.glob("mongoebench[.]{}[.]*[.]json".format(test.short_name()))

    @staticmethod
    def _parse_report_name(report_path):
        """Parse mongoebench report path and return test_name and thread_count.

        The format of the mongoebench report file name is defined in
        ../testing/testcases/benchrun_embedded_test.py
        as mongoebench.<test_name>.<num threads>.<iteration num>.json
        """
        report_base = os.path.basename(report_path)
        _, test_name, thread_count, _, _ = report_base.split(".")
        return test_name, thread_count


class _BenchrunEmbeddedThreadsReport(object):
    """_BenchrunEmbeddedThreadsReport class.

    Class representation of a report for all thread levels of a single
    benchmark test. Each report is designed to correspond to one graph
    in the Evergreen perf plugin.

    A raw mongoebench report looks like the following:
    {
        "note" : "values per second",
        "errCount" : { "$numberLong" : "0" },
        "trapped" : "error: not implemented",
        "insertLatencyAverageMicros" : 389.4926654182272,
        "totalOps" : { "$numberLong" : "12816" },
        "totalOps/s" : 2563.095938304905,
        "findOne" : 0,
        "insert" : 2563.095938304905,
        "delete" : 0,
        "update" : 0,
        "query" : 0,
        "command" : 0,
        "findOnes" : { "$numberLong" : "0" },
        "inserts" : { "$numberLong" : "12816" },
        "deletes" : { "$numberLong" : "0" },
        "updates" : { "$numberLong" : "0" },
        "queries" : { "$numberLong" : "0" },
        "commands" : { "$numberLong" : "0" }
    }
    """

    def __init__(self):
        # list of benchmark runs for each thread.
        self.thread_benchmark_map = collections.defaultdict(list)

    def add_report(self, thread_count, report):
        """Add to report."""
        self.thread_benchmark_map[str(thread_count)].append(report)

    def generate_perf_plugin_dict(self):
        """Generate perf plugin data points of the following format.

        "1": {
          "error_values": [
            0,
            0,
            0
          ],
          "ops_per_sec": 9552.108279243452,
          "ops_per_sec_values": [
            9574.812658450564,
            9522.642340821469,
            9536.252775275878
          ]
        },
        """

        res = {}
        for thread_count, reports in self.thread_benchmark_map.items():
            thread_report = {"error_values": [], "ops_per_sec_values": []}

            for report in reports:
                thread_report["error_values"].append(report["errCount"]["$numberLong"])
                thread_report["ops_per_sec_values"].append(report["totalOps/s"])
            thread_report["ops_per_sec"] = sum(thread_report["ops_per_sec_values"]) / len(reports)

            res[thread_count] = thread_report

        return res
