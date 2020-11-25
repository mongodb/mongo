"""Utility to support parsing a TestStat."""
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime
from typing import NamedTuple, List

from evergreen import EvergreenApi, TestStats

import buildscripts.util.testname as testname  # pylint: disable=wrong-import-position


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


class HistoricTaskData(object):
    """Represent the test statistics for the task that is being analyzed."""

    def __init__(self, evg_test_stats_results: List[TestStats]) -> None:
        """Initialize the TestStats with raw results from the Evergreen API."""
        self._runtime_by_test = defaultdict(_RuntimeHistory.empty)
        self._hook_runtime_by_test = defaultdict(lambda: defaultdict(_RuntimeHistory.empty))

        for doc in evg_test_stats_results:
            self._add_stats(doc)

    # pylint: disable=too-many-arguments
    @classmethod
    def from_evg(cls, evg_api: EvergreenApi, project: str, start_date: datetime, end_date: datetime,
                 task: str, variant: str) -> "HistoricTaskData":
        """
        Retrieve test stats from evergreen for a given task.

        :param evg_api: Evergreen API client.
        :param project: Project to query.
        :param start_date: Start date to query.
        :param end_date: End date to query.
        :param task: Task to query.
        :param variant: Build variant to query.
        :return: Test stats for the specified task.
        """
        days = (end_date - start_date).days
        return cls(
            evg_api.test_stats_by_project(project, after_date=start_date, before_date=end_date,
                                          tasks=[task], variants=[variant], group_by="test",
                                          group_num_days=days))

    def _add_stats(self, test_stats: TestStats) -> None:
        """Add the statistics found in a document returned by the Evergreen test_stats/ endpoint."""
        test_file = testname.normalize_test_file(test_stats.test_file)
        duration = test_stats.avg_duration_pass
        num_run = test_stats.num_pass
        is_hook = testname.is_resmoke_hook(test_file)
        if is_hook:
            self._add_test_hook_stats(test_file, duration, num_run)
        else:
            self._add_test_stats(test_file, duration, num_run)

    def _add_test_stats(self, test_file: str, duration: float, num_run: int) -> None:
        """Add the statistics for a test."""
        self._runtime_by_test[test_file].add_runtimes(duration, num_run)

    def _add_test_hook_stats(self, test_file: str, duration: float, num_run: int) -> None:
        """Add the statistics for a hook."""
        test_name, hook_name = testname.split_test_hook_name(test_file)
        self._hook_runtime_by_test[test_name][hook_name].add_runtimes(duration, num_run)

    def get_tests_runtimes(self) -> List[TestRuntime]:
        """Return the list of (test_file, runtime_in_secs) tuples ordered by decreasing runtime."""
        tests = []
        for test_file, runtime_info in list(self._runtime_by_test.items()):
            duration = runtime_info.duration
            test_name = testname.get_short_name_from_test_file(test_file)
            for _, hook_runtime_info in self._hook_runtime_by_test[test_name].items():
                duration += hook_runtime_info.duration
            test = TestRuntime(test_name=normalize_test_name(test_file), runtime=duration)
            tests.append(test)
        return sorted(tests, key=lambda x: x.runtime, reverse=True)
