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
        self.end_time = datetime.datetime.now()

        if self.report_file is not None:
            report = self._generate_perf_plugin_report()
            with open(self.report_file, "w") as f:
                json.dump(report, f)

    def _generate_perf_plugin_report(self):
        """
        Format the data to look like a perf plugin report, which has
        the following format:
        
        "name": "perf",
        "task_name": "update",
        "project_id": "performance",
        "task_id": "TASK_ID",
        "build_id": "BUILD_ID",
        "variant": "linux-wt-standalone",
        "version_id": "performance_cfe9d2477fab36aa57ccd2f63206955a248cf266",
        "create_time": "2018-02-28T03:44:16Z",
        "is_patch": false,
        "order": 11249,
        "revision": "cfe9d2477fab36aa57ccd2f63206955a248cf266",
        "data": {
          "end": "2018-02-28T05:36:13.127Z",
          "errors": [],
          "results": [
            {
              "name": "Update.SetWithIndex.Random",
              "results": {
                ...
        """
        perf_report = {
            "build_id": None,
            "create_time": self._strftime(self.create_time),
            "is_patch": _config.EVERGREEN_PATCH_BUILD,
            "name": "perf",
            "order": None,
            "project_id": _config.EVERGREEN_PROJECT_NAME,
            "revision": _config.EVERGREEN_REVISION,
            "task_id": _config.EVERGREEN_TASK_ID,
            "task_name": _config.EVERGREEN_TASK_NAME,
            "variant": _config.EVERGREEN_VARIANT_NAME,
            "version_id": None,

            "data": {
                "end": self._strftime(self.end_time),
                "errors": [],  # There are no errors if we have gotten this far.
                "results": []
            }
        }

        for name, report in self.benchmark_reports.items():
            test_report = {
                "name": name,
                "results": report.generate_perf_plugin_dict(),
                "context": report.context._asdict()
            }

            perf_report["data"]["results"].append(test_report)

        return perf_report

    def _parse_report(self, report_dict):
        context = report_dict["context"]

        for benchmark_res in report_dict["benchmarks"]:
            name_no_thread = benchmark_res["name"].rsplit("/", 1)[0]

            if name_no_thread not in self.benchmark_reports:
                self.benchmark_reports[name_no_thread] = _BenchmarkThreadsReport(context)
            self.benchmark_reports[name_no_thread].add_report(benchmark_res)


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
            if (thread_count.endswith("median")
                    or thread_count.endswith("mean") or thread_count.endswith("stddev")):
                # We don't use Benchmark's included statistics for now, in case they're not
                # available. Just store them as-is for future reference.
                res[thread_count] = reports[0]

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
        # Get the thread from a string that looks like this:
        # "BM_SetInsert/arg name:1024/threads:10".
        return name.rsplit("/", 1)[-1].split(":")[-1]
