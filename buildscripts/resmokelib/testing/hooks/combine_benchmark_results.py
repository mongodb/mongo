"""Module for generating the test results file fed into the perf plugin."""

import collections
import datetime
import json
from typing import List, Dict, Any
from dataclasses import dataclass

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.errors import CedarReportError
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.util.cedar_report import CedarMetric, CedarTestReport


class CombineBenchmarkResults(interface.Hook):
    """CombineBenchmarkResults class.

    The CombineBenchmarkResults hook combines test results from
    individual benchmark files to a single file. This is useful for
    generating the json file to feed into the Evergreen performance
    visualization plugin.
    """

    DESCRIPTION = "Combine JSON results from individual benchmarks"

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture):
        """Initialize CombineBenchmarkResults."""
        interface.Hook.__init__(self, hook_logger, fixture, CombineBenchmarkResults.DESCRIPTION)
        self.legacy_report_file = _config.PERF_REPORT_FILE
        self.cedar_report_file = _config.CEDAR_REPORT_FILE

        # Reports grouped by name without thread.
        self.benchmark_reports = {}

        self.create_time = None
        self.end_time = None

    @staticmethod
    def _strftime(time):
        return time.strftime("%Y-%m-%dT%H:%M:%SZ")

    def after_test(self, test, test_report):
        """Update test report."""
        if self.legacy_report_file is None:
            return

        bm_report_path = test.report_name()

        with open(bm_report_path, "r") as bm_report_file:
            bm_report_dict = json.load(bm_report_file)
            self._parse_report(bm_report_dict)

    def before_suite(self, test_report):
        """Set suite start time."""
        self.create_time = datetime.datetime.now()

    def after_suite(self, test_report, teardown_flag=None):
        """Update test report."""
        if self.legacy_report_file is None:
            return

        self.end_time = datetime.datetime.now()
        legacy_report = self._generate_perf_plugin_report()
        with open(self.legacy_report_file, "w") as fh:
            json.dump(legacy_report, fh)

        try:
            cedar_report = self._generate_cedar_report()
        except CedarReportError:
            teardown_flag.set()
            raise
        else:
            with open(self.cedar_report_file, "w") as fh:
                json.dump(cedar_report, fh)

    def _generate_perf_plugin_report(self):
        """Format the data to look like a perf plugin report."""
        perf_report = {
            "start": self._strftime(self.create_time),
            "end": self._strftime(self.end_time),
            "errors": [],  # There are no errors if we have gotten this far.
            "results": []
        }

        for name, report in list(self.benchmark_reports.items()):
            test_report = {
                "name": name, "context": report.context._asdict(),
                "results": report.generate_perf_plugin_dict()
            }

            perf_report["results"].append(test_report)

        return perf_report

    def _generate_cedar_report(self) -> List[dict]:
        """Format the data to look like a cedar report."""
        cedar_report = []

        for name, report in self.benchmark_reports.items():
            cedar_metrics = report.generate_cedar_metrics()
            for _, thread_metrics in cedar_metrics.items():
                if report.check_dup_metric_names(thread_metrics):
                    msg = f"The test '{name}' has duplicated metric names."
                    raise CedarReportError(msg)

            for threads_count, thread_metrics in cedar_metrics.items():
                test_report = CedarTestReport(test_name=name, thread_level=threads_count,
                                              metrics=thread_metrics)
                cedar_report.append(test_report.as_dict())

        return cedar_report

    def _parse_report(self, report_dict):
        context = report_dict["context"]

        for benchmark_res in report_dict["benchmarks"]:
            bm_name_obj = _BenchmarkThreadsReport.parse_bm_name(benchmark_res)

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
        "executable": "./build/opt/mongo/db/concurrency/lock_manager_bm"
        "num_cpus": 40,
        "mhz_per_cpu": 2801,
        "cpu_scaling_enabled": false,
        "caches": [
        ],
        "library_build_type": "debug"
      },
      "benchmarks": [
        {
          "name": "ServiceEntryPointCommonBenchmarkFixture/BM_SEP_PING/threads:1",
          "run_name": "ServiceEntryPointCommonBenchmarkFixture/BM_SEP_PING/threads:1",
          "run_type": "iteration",
          "repetitions": 0,
          "repetition_index": 0,
          "threads": 1,
          "iterations": 384039,
          "real_time": 1.8257322104271429e+04,
          "cpu_time": 1.8254808467369203e+04,
          "time_unit": "ns",
          "cycles": 8.5453635200000000e+08,
          "cycles_per_iteration": 2.2251290936597584e+03,
          "instructions": 1.4558972662000000e+10,
          "instructions_per_iteration": 3.7910141058590401e+04,
          "items_per_second": 5.4780087218527544e+04
        }
      ]
    }
    """

    @dataclass
    class BenchmarkMetricInfo:
        """Information about a particular benchmark metric."""

        local_name: str
        cedar_name: str
        cedar_metric_type: str

    BENCHMARK_METRICS_TO_GATHER = {
        "latency":
            BenchmarkMetricInfo(local_name="cpu_time", cedar_name="latency_per_op",
                                cedar_metric_type="LATENCY"), "instructions_per_iteration":
                                    BenchmarkMetricInfo(local_name="instructions_per_iteration",
                                                        cedar_name="instructions_per_iteration",
                                                        cedar_metric_type="LATENCY")
    }

    # Map benchmark metric type to the type in Cedar
    # https://github.com/evergreen-ci/cedar/blob/87e22df45845440cf299d4ee1f406e8c00ff05ae/perf.proto#L101-L115
    AGGREGATE_TYPE_TO_CEDAR_METRIC_TYPE_MAP = {
        "mean": "MEAN",
        "median": "MEDIAN",
        "stddev": "STANDARD_DEVIATION",
    }

    CONTEXT_FIELDS = [
        "date",
        "num_cpus",
        "mhz_per_cpu",
        "library_build_type",
        "executable",
        "caches",
        "cpu_scaling_enabled",
    ]

    Context = collections.namedtuple(
        typename="Context",
        field_names=CONTEXT_FIELDS,
        # We need a default for cpu_scaling_enabled, since newer
        # google benchmark doesn't report a value if it can't make a
        # determination.
        defaults=["unknown"],
    )  # type: ignore

    def __init__(self, context_dict):
        # `context_dict` was parsed from a json file and might have additional fields.
        relevant = dict(filter(lambda e: e[0] in self.Context._fields, context_dict.items()))
        self.context = self.Context(**relevant)

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
        for thread_count, reports in list(self.thread_benchmark_map.items()):
            thread_report = {
                "error_values": [],
                "ops_per_sec_values": [],  # This is actually storing latency per op, not ops/s
            }

            for report in reports:
                # Don't show Benchmark's included statistics to prevent cluttering up the graph.
                if report.get("run_type") == "aggregate":
                    continue
                thread_report["error_values"].append(0)
                # Take the negative of the latency numbers to preserve the higher is better semantics.
                thread_report["ops_per_sec_values"].append(-1 * report["cpu_time"])
            thread_report["ops_per_sec"] = sum(thread_report["ops_per_sec_values"]) / len(
                thread_report["ops_per_sec_values"])

            res[thread_count] = thread_report

        return res

    def generate_cedar_metrics(self) -> Dict[int, List[CedarMetric]]:
        """
        Generate metrics for Cedar.

        A single item in a Google Benchmark report looks something like

        {
          "name": "ServiceEntryPointCommonBenchmarkFixture/BM_SEP_PING/threads:32_mean",
          "run_name": "ServiceEntryPointCommonBenchmarkFixture/BM_SEP_PING/threads:32",
          "run_type": "aggregate",
          "repetitions": 0,
          "threads": 32,
          "aggregate_name": "mean",
          "iterations": 3,
          "real_time": 1.8245845942382809e+03,
          "cpu_time": 2.9134014577311347e+04,
          "time_unit": "ns",
          "cycles": 1.7830294660000000e+09,
          "cycles_per_iteration": 7.3209395365260807e+03,
          "instructions": 9.2459413546666660e+09,
          "instructions_per_iteration": 3.7962904655542414e+04,
          "items_per_second": 3.4326400041023953e+04
        }
        The regular output of a benchmark is a list of these types of reports. They differ by what iteration and thread
        level it is reporting on.

        aggregate_name is optional and only appears in some of the reports. When it appears in the report, it is telling
        us that all the metrics in that report are an aggregate of that type. The example above is telling us
        that all the metrics in that report (instructions_per_iteration, items_per_second, etc.) are an average. If
        aggregate_name is not in the report, that is a report showing only the results for that single iteration run.

        :return: A dictionary containing a list of CedarMetrics split up by what thread each belongs to.

        It looks something like this:
        [
            {
                "info": {
                    "test_name": "ServiceEntryPointCommonBenchmarkFixture/BM_SEP_PING",
                    "args": {
                        "thread_level": 1
                    }
                },
                "metrics": [
                    {
                        "name": "latency_per_op_0",
                        "type": "LATENCY",
                        "value": 18254.808467369203,
                        "user_submitted": false
                    },
                    {
                        "name": "latency_per_op_1",
                        "type": "LATENCY",
                        "value": 18257.808467369203,
                        "user_submitted": false
                    },
                    {
                        "name": "latency_per_op_stddev",
                        "type": "STANDARD_DEVIATION",
                        "value": 33.544253827885704,
                        "user_submitted": false
                    }
                ]
            },
            {
                "info": {
                    "test_name": "ServiceEntryPointCommonBenchmarkFixture/BM_SEP_PING",
                    "args": {
                        "thread_level": 2
                    }
                },
                "metrics": [
                    {
                        "name": "latency_per_op_0",
                        "type": "LATENCY",
                        "value": 19537.578709627178,
                        "user_submitted": false
                    },
                    {
                        "name": "latency_per_op_1",
                        "type": "LATENCY",
                        "value": 19348.98278000912,
                        "user_submitted": false
                    },
                    {
                        "name": "latency_per_op_stddev",
                        "type": "STANDARD_DEVIATION",
                        "value": 112.31621981920948,
                        "user_submitted": false
                    }
                ]
            }
        ]
        """

        res = {}

        for _, reports in self.thread_benchmark_map.items():
            for report in reports:
                # Check to see if this report is a collection of aggregates or not - this field is missing if it's not.
                aggregate_name = report.get("aggregate_name", None)
                for bm_metric_info in self.BENCHMARK_METRICS_TO_GATHER.values():
                    metric_local_name = bm_metric_info.local_name
                    metric_cedar_name = bm_metric_info.cedar_name
                    metric_cedar_type = bm_metric_info.cedar_metric_type

                    # Some metrics may not be in every benchmark. If a metric we want isn't in this report, move on.
                    metric_value = report.get(metric_local_name, None)
                    if metric_value is None:
                        continue

                    if aggregate_name is not None:
                        # Call out that this particular metric is an aggregated result. For example, if the aggregate is
                        # a mean and we are looking at the `latency` metric, metric_name becomes `latency_mean` and the
                        # cedar_type becomes `MEAN`.

                        metric_name = f"{metric_cedar_name}_{aggregate_name}"
                        metric_cedar_type = self.AGGREGATE_TYPE_TO_CEDAR_METRIC_TYPE_MAP[
                            aggregate_name]
                    else:
                        # Call out what iteration this metric came from. For example, if we are looking at iteration 2
                        # and the `latency` metric, metric_name becomes `latency_2`.
                        idx = report.get("repetition_index", 0)
                        metric_name = f"{metric_cedar_name}_{idx}"

                    metric = CedarMetric(name=metric_name, type=metric_cedar_type,
                                         value=metric_value)
                    threads = report["threads"]
                    if threads in res:
                        res[threads].append(metric)
                    else:
                        res[threads] = [metric]

        return res

    @staticmethod
    def check_dup_metric_names(metrics: List[CedarMetric]) -> bool:
        """Check duplicated metric names for Cedar."""
        names = []
        for metric in metrics:
            if metric.name in names:
                return True
            names.append(metric.name)
        return False

    @staticmethod
    def parse_bm_name(benchmark_res: Dict[str, Any]):
        """
        Split the benchmark name into base_name, thread_count and statistic_type.

        The base name is the benchmark name minus the thread count and any statistics.
        Testcases of the same group will be shown on a single perf graph.

        benchmark_res["name"] look like the following:
        "BM_SetInsert/arg name:1024/threads:10_mean"
        "BM_SetInsert/arg 1/arg 2"
        "BM_SetInsert_mean"
        """

        name_str = benchmark_res["name"]
        base_name = None
        thread_count = None
        statistic_type = benchmark_res.get("aggregate_name", None)

        # Step 1: get the statistic type.
        statistic_type_candidate = name_str.rsplit("_", 1)[-1]
        # Remove the statistic type suffix from the name.
        if statistic_type_candidate == statistic_type:
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
