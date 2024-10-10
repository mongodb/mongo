"""Module for generating the test results file fed into Cedar and checking them against set thresholds."""

import collections
import datetime
import json
from dataclasses import dataclass
from enum import Enum
from typing import Any, Dict, List

import yaml

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.errors import CedarReportError, ServerFailure
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.util.cedar_report import CedarMetric, CedarTestReport

THRESHOLD_LOCATION = "etc/performance_thresholds.yml"


class BoundDirection(str, Enum):
    """Enum describing the two different values available to be set for thresholds."""

    UPPER = "upper"
    LOWER = "lower"


@dataclass
class IndividualMetricThreshold:
    """A single check for a reported performance result."""

    metric_name: str
    thread_level: int
    test_name: str
    value: int
    bound_direction: BoundDirection


@dataclass(frozen=True, eq=True)
class ReportedMetric:
    """The unique values of a single performance report."""

    test_name: str
    metric_name: str
    thread_level: int


class GenerateAndCheckPerfResults(interface.Hook):
    """GenerateAndCheckPerfResults class.

    The GenerateAndCheckPerfResults hook combines test results from
    individual benchmark files to a single file. This is useful for
    generating the json file to feed into the Evergreen performance
    visualization plugin.

    It also will check the performance results against any thresholds that are set for each benchmark.
    If no thresholds are set for a test, this hook should always pass.
    """

    DESCRIPTION = "Combine JSON results from individual benchmarks and check their reported values against any thresholds set for them."

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, check_result=False):
        """Initialize GenerateAndCheckPerfResults."""
        interface.Hook.__init__(self, hook_logger, fixture, GenerateAndCheckPerfResults.DESCRIPTION)
        self.cedar_report_file = _config.CEDAR_REPORT_FILE
        self.variant = _config.EVERGREEN_VARIANT_NAME
        self.cedar_reports: List[CedarTestReport] = []
        self.performance_thresholds: Dict[str, Any] = {}

        self.create_time = None
        self.end_time = None
        self.check_result = check_result
        # Flag to see if we have checked any results against thresholds. Initialize to false here.
        self.has_checked_results = False

    @staticmethod
    def _strftime(time):
        return time.strftime("%Y-%m-%dT%H:%M:%SZ")

    def after_test(self, test, test_report):
        """Update test report."""
        bm_report_path = test.report_name()

        with open(bm_report_path, "r") as bm_report_file:
            bm_report_dict = json.load(bm_report_file)

        # Reports grouped by name without thread.
        benchmark_reports = self._parse_report(bm_report_dict)
        # Flat list where results are separated by name and thread_level.
        cedar_formatted_results = self._generate_cedar_report(benchmark_reports)

        self.cedar_reports.extend(cedar_formatted_results)

        self._check_pass_fail(benchmark_reports, cedar_formatted_results, test, test_report)

    def _check_pass_fail(
        self,
        benchmark_reports: Dict[str, "_BenchmarkThreadsReport"],
        cedar_formatted_results: List[CedarTestReport],
        test,
        test_report,
    ):
        """Check to see if any of the reported results violate any of the thresholds set."""
        if self.variant is None:
            self.logger.info(
                "No variant information was given to resmoke. Please set the --variantName flag to let resmoke know what thresholds to use when checking."
            )
            return

        for test_name in benchmark_reports.keys():
            variant_thresholds = self.performance_thresholds.get(test_name, None)
            if variant_thresholds is None:
                self.logger.info(
                    f"No thresholds were set for {test_name}, skipping threshold check"
                )
                continue
            test_thresholds = variant_thresholds.get(self.variant, None)
            if test_thresholds is None:
                self.logger.info(
                    f"No thresholds were set for {test_name} on {self.variant}, skipping threshold check"
                )
                continue
            # Transform the thresholds set into something we can more easily use.
            metrics_to_check: List[IndividualMetricThreshold] = []
            for item in test_thresholds:
                thread_level = item["thread_level"]
                for metric in item["metrics"]:
                    metrics_to_check.append(
                        IndividualMetricThreshold(
                            test_name=test_name,
                            thread_level=thread_level,
                            metric_name=metric["name"],
                            value=metric["value"],
                            bound_direction=metric["bound_direction"],
                        )
                    )
            # Transform the reported performance results into something we can more easily use.
            transformed_metrics: Dict[ReportedMetric, CedarMetric] = {}
            for cedar_result in cedar_formatted_results:
                for individual_metric in cedar_result.metrics:
                    reported_metric = ReportedMetric(
                        test_name=cedar_result.test_name,
                        thread_level=cedar_result.thread_level,
                        metric_name=individual_metric.name,
                    )
                    if transformed_metrics.get(reported_metric, None) is not None:
                        raise CedarReportError(
                            f"Multiple values reported for the same metric: {reported_metric}"
                        )
                    else:
                        transformed_metrics[reported_metric] = individual_metric
            if len(metrics_to_check) > 0:
                self.has_checked_results = True
            # Add a dynamic resmoke test to make sure that the pass/fail results are reported correctly.
            hook_test_case = CheckPerfResultTestCase.create_after_test(
                self.logger, test, self, metrics_to_check, transformed_metrics
            )
            hook_test_case.configure(self.fixture)
            hook_test_case.run_dynamic_test(test_report)

    def before_suite(self, test_report):
        """Set suite start time."""
        self.create_time = datetime.datetime.now()

        try:
            with open(THRESHOLD_LOCATION) as fh:
                self.performance_thresholds = yaml.safe_load(fh)["tests"]
        except Exception:
            self.logger.exception(
                f"Could not load in the threshold file needed to check performance results. "
                f"Trying to retrieve them from {THRESHOLD_LOCATION}."
            )
            raise ServerFailure(
                "Could not load the needed threshold information. Please make sure you are in the root of the mongo repo."
            )

    def after_suite(self, test_report, teardown_flag=None):
        """Update test report."""

        self.end_time = datetime.datetime.now()

        if self.cedar_report_file is not None:
            dict_formatted_results = [result.as_dict() for result in self.cedar_reports]
            with open(self.cedar_report_file, "w") as fh:
                json.dump(dict_formatted_results, fh)
        if self.has_checked_results and not self.check_result:
            raise ServerFailure(
                "Running generate_and_check_perf_results."
                " Results were checked against thresholds, but configuration"
                " indicated there shouldn't be."
            )
        if not self.has_checked_results and self.check_result:
            raise ServerFailure(
                "Running generate_and_check_perf_results."
                " No results checked against thresholds, but configuration"
                " indicated there should be."
            )

    def _generate_cedar_report(
        self, benchmark_reports: Dict[str, "_BenchmarkThreadsReport"]
    ) -> List[CedarTestReport]:
        """Format the data to look like a cedar report."""
        cedar_report = []

        for name, report in benchmark_reports.items():
            cedar_metrics = report.generate_cedar_metrics()
            for _, thread_metrics in cedar_metrics.items():
                if report.check_dup_metric_names(thread_metrics):
                    msg = f"The test '{name}' has duplicated metric names."
                    raise CedarReportError(msg)

            for threads_count, thread_metrics in cedar_metrics.items():
                test_report = CedarTestReport(
                    test_name=name, thread_level=threads_count, metrics=thread_metrics
                )
                cedar_report.append(test_report)

        return cedar_report

    def _parse_report(self, report_dict) -> Dict[str, "_BenchmarkThreadsReport"]:
        context = report_dict["context"]
        # Reports grouped by name without thread.
        benchmark_reports: Dict[str, "_BenchmarkThreadsReport"] = {}

        for benchmark_res in report_dict["benchmarks"]:
            bm_name_obj = _BenchmarkThreadsReport.parse_bm_name(benchmark_res)

            if bm_name_obj.base_name not in benchmark_reports:
                benchmark_reports[bm_name_obj.base_name] = _BenchmarkThreadsReport(context)
            benchmark_reports[bm_name_obj.base_name].add_report(bm_name_obj, benchmark_res)
        return benchmark_reports


class CheckPerfResultTestCase(interface.DynamicTestCase):
    """CheckPerfResultTestCase class."""

    def __init__(
        self,
        logger,
        test_name,
        description,
        base_test_name,
        hook,
        thresholds_to_check: List["IndividualMetricThreshold"],
        reported_metrics: Dict[ReportedMetric, CedarMetric],
    ):
        super().__init__(logger, test_name, description, base_test_name, hook)
        self.thresholds_to_check: List["IndividualMetricThreshold"] = thresholds_to_check
        self.reported_metrics: Dict[ReportedMetric, CedarMetric] = reported_metrics

    def run_test(self):
        """
        Check the values reported by this performance run.

        If any metric fails validation, the entire benchmark fails.
        If any metric that has a threshold set is not found in the reported results, the entire benchmark will fail.
        """
        any_metric_has_failed = False

        for metric_to_check in self.thresholds_to_check:
            reported_metric = self.reported_metrics.get(
                ReportedMetric(
                    test_name=metric_to_check.test_name,
                    thread_level=metric_to_check.thread_level,
                    metric_name=metric_to_check.metric_name,
                ),
                None,
            )
            if reported_metric is None:
                self.logger.error(
                    f"One of the expected metrics was not able to be found in the performance results generated by this task. {metric_to_check.test_name} with thread_level of {metric_to_check.thread_level} did not report a metric called {metric_to_check.metric_name}."
                )
                any_metric_has_failed = True
                continue
            if (
                metric_to_check.bound_direction == BoundDirection.UPPER
                and metric_to_check.value < reported_metric.value
            ) or (
                metric_to_check.bound_direction == BoundDirection.LOWER
                and metric_to_check.value > reported_metric.value
            ):
                if metric_to_check.bound_direction == BoundDirection.LOWER:
                    self.logger.error(
                        f"Metric {metric_to_check.metric_name} in {metric_to_check.test_name} with thread_level of {metric_to_check.thread_level} has failed the threshold check. The reported value of {reported_metric.value} is lower than the set threshold of {metric_to_check.value}"
                    )
                    any_metric_has_failed = True
                else:
                    self.logger.error(
                        f"Metric {metric_to_check.metric_name} in {metric_to_check.test_name} with thread_level of {metric_to_check.thread_level} has failed the threshold check. The reported value of {reported_metric.value} is higher than the set threshold of {metric_to_check.value}"
                    )
                    any_metric_has_failed = True
        if any_metric_has_failed:
            raise ServerFailure(
                f"One or more of the metrics reported by this task have failed the threshold check. Please resolve all of the issues called out above. These thresholds can be found in {THRESHOLD_LOCATION}"
            )


# Capture information from a Benchmark name in a logical format.
_BenchmarkName = collections.namedtuple(
    "_BenchmarkName", ["base_name", "thread_count", "statistic_type"]
)


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
        "latency": BenchmarkMetricInfo(
            local_name="cpu_time", cedar_name="latency_per_op", cedar_metric_type="LATENCY"
        ),
        "instructions_per_iteration": BenchmarkMetricInfo(
            local_name="instructions_per_iteration",
            cedar_name="instructions_per_iteration",
            cedar_metric_type="LATENCY",
        ),
        "cycles_per_iteration": BenchmarkMetricInfo(
            local_name="cycles_per_iteration",
            cedar_name="cycles_per_iteration",
            cedar_metric_type="LATENCY",
        ),
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
                            aggregate_name
                        ]
                    else:
                        # Call out what iteration this metric came from. For example, if we are looking at iteration 2
                        # and the `latency` metric, metric_name becomes `latency_2`.
                        idx = report.get("repetition_index", 0)
                        metric_name = f"{metric_cedar_name}_{idx}"

                    metric = CedarMetric(
                        name=metric_name, type=metric_cedar_type, value=metric_value
                    )
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
            name_str = name_str[: -len(statistic_type) - 1]

        # Step 2: Get the thread count and name.
        thread_section = name_str.rsplit("/", 1)[-1]
        if thread_section.startswith("threads:"):
            base_name = name_str.rsplit("/", 1)[0]
            thread_count = thread_section.split(":")[-1]
        else:  # There is no explicit thread count, so the thread count is 1.
            thread_count = "1"
            base_name = name_str

        return _BenchmarkName(base_name, thread_count, statistic_type)
