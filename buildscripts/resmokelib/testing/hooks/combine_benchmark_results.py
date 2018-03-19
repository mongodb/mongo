"""Module for generating the test results file fed into the perf plugin."""

from __future__ import absolute_import
from __future__ import division

import collections
import datetime
import json

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.testing.hooks import interface


class CombineBenchmarkResults(interface.Hook):
    """
    The CombineBenchmarkResults hook combines test results from
    individual benchmark files to a single file. This is useful for
    generating the json file to feed into the Evergreen performance
    visualization plugin.
    """

    DESCRIPTION = "Combine JSON results from individual benchmarks"

    def __init__(self, hook_logger, fixture):
        interface.Hook.__init__(self, hook_logger, fixture, CombineBenchmarkResults.DESCRIPTION)
        self.report_file = _config.PERF_REPORT_FILE

        # Reports grouped by name without thread.
        self.benchmark_reports = {}

        self.create_time = None
        self.end_time = None

    @staticmethod
    def _strftime(time):
        return time.strftime("%Y-%m-%dT%H:%M:%SZ")

    def after_test(self, test_case, test_report):
        if self.report_file is None:
            return

        bm_report_path = test_case.report_name()

        with open(bm_report_path, "r") as report_file:
            report_dict = json.load(report_file)
            self._parse_report(report_dict)

    def before_suite(self, test_report):
        self.create_time = datetime.datetime.now()

    def after_suite(self, test_report):
        if self.report_file is None:
            return

        self.end_time = datetime.datetime.now()
        report = self._generate_perf_plugin_report()
        with open(self.report_file, "w") as f:
            json.dump(report, f)

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
                "name": name,
                "results": report.generate_perf_plugin_dict(),
                "context": report.context._asdict()
            }

            perf_report["results"].append(test_report)

        return perf_report

    def _parse_report(self, report_dict):
        context = report_dict["context"]

        for benchmark_res in report_dict["benchmarks"]:
            # The group name is the benchmark name minus the thread count and any statistics.
            # Each group will be shown on a single perf graph.
            group_name = benchmark_res["name"].rsplit("/", 1)[0]

            if group_name == benchmark_res["name"] and group_name.count("_") == 2:
                # When running with only one thread, the thread count is not in the name;
                # just remove the mean/median/stddev suffix in this case.
                # With one thread, the group_name looks like: BM_MyTestNameInCamelCase_statistic.
                group_name = group_name.rsplit("_", 1)[0]

            if group_name not in self.benchmark_reports:
                self.benchmark_reports[group_name] = _BenchmarkThreadsReport(context)
            self.benchmark_reports[group_name].add_report(benchmark_res)


class _BenchmarkThreadsReport(object):
    """
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
        "date",
        "cpu_scaling_enabled",
        "num_cpus",
        "mhz_per_cpu",
        "library_build_type"
    ]
    Context = collections.namedtuple("Context", CONTEXT_FIELDS)

    def __init__(self, context_dict):
        self.context = self.Context(**context_dict)

        # list of benchmark runs for each thread.
        self.thread_benchmark_map = collections.defaultdict(list)

    def add_report(self, report):
        thread_count = self._thread_from_name(report["name"])
        self.thread_benchmark_map[thread_count].append(report)

    def generate_perf_plugin_dict(self):
        """
        Generate perf plugin data points of the following format:

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
            if (thread_count.endswith("median") or thread_count.endswith("mean") or
                    thread_count.endswith("stddev")):
                # We don't use Benchmark's included statistics for now because they clutter up the
                # graph.
                continue

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
    def _thread_from_name(name):
        # Get the thread from a string:
        # "BM_SetInsert/arg name:1024/threads:10_mean" -> "10_mean"
        # "BM_SetInsert" -> "1"
        # "BM_SetInsert_mean" -> "1_mean"
        thread_section = name.rsplit("/", 1)[-1]
        if thread_section.startswith("threads:"):
            return thread_section.split(":")[-1]
        else:
            if name.count("_") == 2:
                suffix = name.split("_")[-1]
                return "1_" + suffix
            return "1"
