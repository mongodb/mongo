"""Module for generating the test results file fed into the perf plugin."""

from __future__ import absolute_import
from __future__ import division

import collections
import datetime
import json

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.testing.hooks import interface


class CombineBenchmarkResults(interface.CustomBehavior):
    """CombineBenchmarkResults class.

    The CombineBenchmarkResults hook combines test results from
    individual benchmark files to a single file. This is useful for
    generating the json file to feed into the Evergreen performance
    visualization plugin.
    """

    DESCRIPTION = "Combine JSON results from individual benchmarks"

    def __init__(self, hook_logger, fixture):
        """Initialize CombineBenchmarkResults."""
        interface.CustomBehavior.__init__(
            self, hook_logger, fixture,CombineBenchmarkResults.DESCRIPTION)
        self.report_file = _config.PERF_REPORT_FILE

        # Reports grouped by name without thread.
        self.benchmark_reports = {}

        self.create_time = None
        self.end_time = None

    @staticmethod
    def _strftime(time):
        return time.strftime("%Y-%m-%dT%H:%M:%SZ")

    def after_test(self, test, test_report):
        """Update test report."""
        if self.report_file is None:
            return

        bm_report_path = test.report_name()

        with open(bm_report_path, "r") as report_file:
            report_dict = json.load(report_file)
            self._parse_report(report_dict)

    def before_suite(self, test_report):
        """Set suite start time."""
        self.create_time = datetime.datetime.now()

    def after_suite(self, test_report):
        """Update test report."""
        if self.report_file is None:
            return

        self.end_time = datetime.datetime.now()
        report = self._generate_perf_plugin_report()
        with open(self.report_file, "w") as fh:
            json.dump(report, fh)

    def _generate_perf_plugin_report(self):
        """Format the data to look like a perf plugin report."""
        perf_report = {
            "start": self._strftime(self.create_time),
            "end": self._strftime(self.end_time),
            "errors": [],  # There are no errors if we have gotten this far.
            "results": []
        }

        for name, report in self.benchmark_reports.items():
            test_report = {
                "name": name, "context": report.context._asdict(),
                "results": report.generate_perf_plugin_dict()
            }

            perf_report["results"].append(test_report)

        return perf_report

    def _parse_report(self, report_dict):
        context = report_dict["context"]

        for benchmark_res in report_dict["benchmarks"]:
            bm_name_obj = _BenchmarkThreadsReport.parse_bm_name(benchmark_res["name"])

            # Don't show Benchmark's included statistics to prevent cluttering up the graph.
            if bm_name_obj.statistic_type is not None:
                continue

            if bm_name_obj.base_name not in self.benchmark_reports:
                self.benchmark_reports[bm_name_obj.base_name] = _BenchmarkThreadsReport(context)
            self.benchmark_reports[bm_name_obj.base_name].add_report(bm_name_obj, benchmark_res)


# Capture information from a Benchmark name in a logical format.
_BenchmarkName = collections.namedtuple("_BenchmarkName",
                                        ["base_name", "thread_count", "statistic_type"])


class _BenchmarkThreadsReport(object):
    """_BenchmarkThreadsReport class.

    Class representation of a report for all thread levels of a single
    benchmark test. Each report is designed to correspond to one graph
    in the Evergreen perf plugin.

    A raw Benchmark report looks like the following:
    {
      "context": {
        "date": "2015/03/17-18:40:25",
        "num_cpus": 40,
        "mhz_per_cpu": 2801,
        "cpu_scaling_enabled": false,
        "library_build_type": "debug"
      },
      "benchmarks": [
        {
          "name": "BM_SetInsert/arg name:1024/threads:10",
          "iterations": 21393,
          "real_time": 32724,
          "cpu_time": 33355,
          "bytes_per_second": 1199226,
          "items_per_second": 299807
        }
      ]
    }
    """

    CONTEXT_FIELDS = [
        "date", "cpu_scaling_enabled", "num_cpus", "mhz_per_cpu", "library_build_type"
    ]
    Context = collections.namedtuple("Context", CONTEXT_FIELDS)  # type: ignore

    def __init__(self, context_dict):
        self.context = self.Context(**context_dict)

        # list of benchmark runs for each thread.
        self.thread_benchmark_map = collections.defaultdict(list)

    def add_report(self, bm_name_obj, report):
        """Add to report."""
        self.thread_benchmark_map[bm_name_obj.thread_count].append(report)

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
            thread_report = {
                "error_values": [0 for _ in range(len(reports))],
                "ops_per_sec_values": []  # This is actually storing latency per op, not ops/s
            }

            # Take the negative of the latency numbers to preserve the higher is better semantics.
            for report in reports:
                thread_report["ops_per_sec_values"].append(-1 * report["cpu_time"])
            thread_report["ops_per_sec"] = sum(thread_report["ops_per_sec_values"]) / len(reports)

            res[thread_count] = thread_report

        return res

    @staticmethod
    def parse_bm_name(name_str):
        """
        Split the benchmark name into base_name, thread_count and statistic_type.

        The base name is the benchmark name minus the thread count and any statistics.
        Testcases of the same group will be shown on a single perf graph.

        name_str look like the following:
        "BM_SetInsert/arg name:1024/threads:10_mean"
        "BM_SetInsert/arg 1/arg 2"
        "BM_SetInsert_mean"
        """

        base_name = None
        thread_count = None
        statistic_type = None

        # Step 1: get the statistic type.
        if name_str.count("_") == 2:  # There is statistics.
            statistic_type = name_str.rsplit("_", 1)[-1]
            # Remove the statistic type suffix from the name.
            name_str = name_str[:-len(statistic_type) - 1]

        # Step 2: Get the thread count and name.
        thread_section = name_str.rsplit("/", 1)[-1]
        if thread_section.startswith("threads:"):
            base_name = name_str.rsplit("/", 1)[0]
            thread_count = thread_section.split(":")[-1]
        else:  # There is no explicit thread count, so the thread count is 1.
            thread_count = "1"
            base_name = name_str

        return _BenchmarkName(base_name, thread_count, statistic_type)
