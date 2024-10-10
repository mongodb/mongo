"""Utility to support parsing a TestStat."""

from collections import defaultdict
from dataclasses import dataclass
from itertools import chain
from json import JSONDecodeError
from typing import Callable, List, NamedTuple, Optional

import requests
from requests.adapters import HTTPAdapter, Retry

from buildscripts.util.testname import (
    get_short_name_from_test_file,
    is_resmoke_hook,
    split_test_hook_name,
)

TASK_LEVEL_HOOKS = {"CleanEveryN"}
TESTS_STATS_S3_LOCATION = "https://mongo-test-stats.s3.amazonaws.com"


class HistoricalTestInformation(NamedTuple):
    """
    Container for information about the historical runtime of a test.

    test_name: Name of test.
    avg_duration_pass: Average of runtime of test that passed.
    num_pass: Number of times the test has passed.
    num_fail: Number of times the test has failed.
    max_duration_pass: Maximum runtime of the test when it passed.
    """

    test_name: str
    num_pass: int
    num_fail: int
    avg_duration_pass: float
    max_duration_pass: Optional[float] = None


class TestRuntime(NamedTuple):
    """
    Container for the runtime of a test.

    test_name: Name of test.
    runtime: Average of runtime of test.
    """

    test_name: str
    runtime: float


@dataclass
class _RuntimeHistory:
    """
    History of runtime results.

    duration: Average duration of test runtime.
    num_runs: Number of test runs seen.
    """

    duration: float
    num_runs: int

    @classmethod
    def empty(cls) -> "_RuntimeHistory":
        """Create an empty runtime entry."""
        return cls(duration=0.0, num_runs=0)

    def add_runtimes(self, duration: float, num_runs: int) -> None:
        """
        Add the given duration number this history.

        :param duration: Average duration to include.
        :param num_runs: Number of runs to include.
        """
        self.duration = _average(self.duration, self.num_runs, duration, num_runs)
        self.num_runs += num_runs


def normalize_test_name(test_name: str) -> str:
    """Normalize test names that may have been run on windows or unix."""
    return test_name.replace("\\", "/")


def _average(value_a: float, num_a: int, value_b: float, num_b: int) -> float:
    """Compute a weighted average of 2 values with associated numbers."""
    divisor = num_a + num_b
    if divisor == 0:
        return 0
    else:
        return float(value_a * num_a + value_b * num_b) / divisor


class HistoricHookInfo(NamedTuple):
    """Historic information about a test hook."""

    hook_id: str
    num_pass: int
    avg_duration: float

    @classmethod
    def from_test_stats(cls, test_stats: HistoricalTestInformation) -> "HistoricHookInfo":
        """Create an instance from a test_stats object."""
        return cls(
            hook_id=test_stats.test_name,
            num_pass=test_stats.num_pass,
            avg_duration=test_stats.avg_duration_pass,
        )

    def test_name(self) -> str:
        """Get the name of the test associated with this hook."""
        return split_test_hook_name(self.hook_id)[0]

    def hook_name(self) -> str:
        """Get the name of this hook."""
        return split_test_hook_name(self.hook_id)[-1]

    def is_task_level_hook(self) -> bool:
        """Determine if this hook should be counted against the task not the test."""
        return self.hook_name() in TASK_LEVEL_HOOKS


class HistoricTestInfo(NamedTuple):
    """Historic information about a test."""

    test_name: str
    num_pass: int
    avg_duration: float
    hooks: List[HistoricHookInfo]

    @classmethod
    def from_test_stats(
        cls, test_stats: HistoricalTestInformation, hooks: List[HistoricHookInfo]
    ) -> "HistoricTestInfo":
        """Create an instance from a test_stats object."""
        return cls(
            test_name=test_stats.test_name,
            num_pass=test_stats.num_pass,
            avg_duration=test_stats.avg_duration_pass,
            hooks=hooks,
        )

    def normalized_test_name(self) -> str:
        """Get the normalized version of the test name."""
        return normalize_test_name(self.test_name)

    def total_hook_runtime(
        self, predicate: Optional[Callable[[HistoricHookInfo], bool]] = None
    ) -> float:
        """Get the average runtime of all the hooks associated with this test."""

        def default_predicate(_) -> bool:
            return True

        if not predicate:
            predicate = default_predicate
        return sum(
            [
                hook.avg_duration * (hook.num_pass // self.num_pass if self.num_pass else 1)
                for hook in self.hooks
                if predicate(hook)
            ]
        )

    def total_test_runtime(self) -> float:
        """Get the average runtime of this test and it's non-task level hooks."""
        if self.num_pass > 0:
            return self.avg_duration + self.total_hook_runtime(lambda h: not h.is_task_level_hook())
        return 0.0

    def get_hook_overhead(self) -> float:
        """Get the average runtime of this test and it's non-task level hooks."""
        return self.total_hook_runtime(lambda h: h.is_task_level_hook())


class HistoricTaskData(object):
    """Represent the test statistics for the task that is being analyzed."""

    def __init__(self, historic_test_results: List[HistoricTestInfo]) -> None:
        """Initialize the TestStats with raw results from the Evergreen API."""
        self.historic_test_results = historic_test_results

    @staticmethod
    def get_stats_from_s3(project: str, task: str, variant: str) -> List[HistoricalTestInformation]:
        """
        Retrieve test stats from s3 for a given task.

        :param project: Project to query.
        :param task: Task to query.
        :param variant: Build variant to query.
        :return: A list of the Test stats for the specified task.
        """
        session = requests.Session()
        retries = Retry(total=5, backoff_factor=1, status_forcelist=[502, 503, 504])
        session.mount("https://", HTTPAdapter(max_retries=retries))

        response = session.get(f"{TESTS_STATS_S3_LOCATION}/{project}/{variant}/{task}")

        try:
            data = response.json()
            return [HistoricalTestInformation(**item) for item in data]
        except JSONDecodeError:
            return []

    @classmethod
    def from_s3(cls, project: str, task: str, variant: str) -> "HistoricTaskData":
        """
        Retrieve test stats from s3 for a given task.

        :param project: Project to query.
        :param task: Task to query.
        :param variant: Build variant to query.
        :return: Test stats for the specified task.
        """
        historical_test_data = cls.get_stats_from_s3(project, task, variant)
        return cls.from_stats_list(historical_test_data)

    @classmethod
    def from_stats_list(
        cls, historical_test_data: List[HistoricalTestInformation]
    ) -> "HistoricTaskData":
        """
        Build historic task data from a list of historic stats.

        :param historical_test_data: A list of information about the runtime of a test.
        :return: Historic task data from the list of stats.
        """
        hooks = defaultdict(list)
        for hook in [stat for stat in historical_test_data if is_resmoke_hook(stat.test_name)]:
            historical_hook = HistoricHookInfo.from_test_stats(hook)
            hooks[historical_hook.test_name()].append(historical_hook)

        return cls(
            [
                HistoricTestInfo.from_test_stats(
                    stat, hooks[get_short_name_from_test_file(stat.test_name)]
                )
                for stat in historical_test_data
                if not is_resmoke_hook(stat.test_name)
            ]
        )

    def get_tests_runtimes(self) -> List[TestRuntime]:
        """Return the list of (test_file, runtime_in_secs) tuples ordered by decreasing runtime."""
        tests = [
            TestRuntime(
                test_name=test_stats.normalized_test_name(), runtime=test_stats.total_test_runtime()
            )
            for test_stats in self.historic_test_results
        ]
        return sorted(tests, key=lambda x: x.runtime, reverse=True)

    def get_avg_hook_runtime(self, hook_name: str) -> float:
        """Get the average runtime for the specified hook."""
        hook_instances = list(
            chain.from_iterable(
                [
                    [hook for hook in test.hooks if hook.hook_name() == hook_name]
                    for test in self.historic_test_results
                ]
            )
        )

        if not hook_instances:
            return 0
        return sum([hook.avg_duration for hook in hook_instances]) / len(hook_instances)

    def __len__(self) -> int:
        """Get the number of historical entries."""
        return len(self.historic_test_results)
