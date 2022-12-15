"""Utility to support parsing a TestStat."""

from collections import defaultdict
from collections import namedtuple
from json import JSONDecodeError

from typing import NamedTuple, List
import requests
from requests.adapters import HTTPAdapter, Retry

import buildscripts.util.testname as testname  # pylint: disable=wrong-import-position

TESTS_STATS_S3_LOCATION = "https://mongo-test-stats.s3.amazonaws.com"


class HistoricalTestInformation(NamedTuple):
    """
    Container for information about the historical runtime of a test.

    test_name: Name of test.
    avg_duration_pass: Average of runtime of test that passed.
    num_pass: Number of times the test has passed.
    num_fail: Number of times the test has failed.
    """

    test_name: str
    num_pass: int
    num_fail: int
    avg_duration_pass: float


TestRuntime = namedtuple('TestRuntime', ['test_name', 'runtime'])


def normalize_test_name(test_name):
    """Normalize test names that may have been run on windows or unix."""
    return test_name.replace("\\", "/")


class TestStats(object):
    """Represent the test statistics for the task that is being analyzed."""

    def __init__(self, evg_test_stats_results: List[HistoricalTestInformation]) -> None:
        """Initialize the TestStats with raw results from the Evergreen API."""
        # Mapping from test_file to {"num_run": X, "duration": Y} for tests
        self._runtime_by_test = defaultdict(dict)
        # Mapping from 'test_name:hook_name' to
        #       {'test_name': {hook_name': {"num_run": X, "duration": Y}}}
        self._hook_runtime_by_test = defaultdict(lambda: defaultdict(dict))

        for doc in evg_test_stats_results:
            self._add_stats(doc)

    def _add_stats(self, test_stats: HistoricalTestInformation) -> None:
        """Add the statistics found in a document returned by the Evergreen test_stats/ endpoint."""
        test_file = testname.normalize_test_file(test_stats.test_name)
        duration = test_stats.avg_duration_pass
        num_run = test_stats.num_pass
        is_hook = testname.is_resmoke_hook(test_file)
        if is_hook:
            self._add_test_hook_stats(test_file, duration, num_run)
        else:
            self._add_test_stats(test_file, duration, num_run)

    def _add_test_stats(self, test_file, duration, num_run):
        """Add the statistics for a test."""
        runtime_info = self._runtime_by_test[test_file]
        self._add_runtime_info(runtime_info, duration, num_run)

    def _add_test_hook_stats(self, test_file, duration, num_run):
        """Add the statistics for a hook."""
        test_name, hook_name = testname.split_test_hook_name(test_file)
        runtime_info = self._hook_runtime_by_test[test_name][hook_name]
        self._add_runtime_info(runtime_info, duration, num_run)

    @staticmethod
    def _add_runtime_info(runtime_info, duration, num_run):
        if not runtime_info:
            runtime_info["duration"] = duration
            runtime_info["num_run"] = num_run
        else:
            runtime_info["duration"] = TestStats._average(
                runtime_info["duration"], runtime_info["num_run"], duration, num_run)
            runtime_info["num_run"] += num_run

    @staticmethod
    def _average(value_a, num_a, value_b, num_b):
        """Compute a weighted average of 2 values with associated numbers."""
        divisor = num_a + num_b
        if divisor == 0:
            return 0
        else:
            return float(value_a * num_a + value_b * num_b) / divisor

    def get_tests_runtimes(self):
        """Return the list of (test_file, runtime_in_secs) tuples ordered by decreasing runtime."""
        tests = []
        for test_file, runtime_info in list(self._runtime_by_test.items()):
            duration = runtime_info["duration"]
            test_name = testname.get_short_name_from_test_file(test_file)
            for _, hook_runtime_info in self._hook_runtime_by_test[test_name].items():
                duration += hook_runtime_info["duration"]
            test = TestRuntime(test_name=normalize_test_name(test_file), runtime=duration)
            tests.append(test)
        return sorted(tests, key=lambda x: x.runtime, reverse=True)


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
    session.mount('https://', HTTPAdapter(max_retries=retries))

    response = session.get(f"{TESTS_STATS_S3_LOCATION}/{project}/{variant}/{task}")

    try:
        data = response.json()
        return [HistoricalTestInformation(**item) for item in data]
    except JSONDecodeError:
        return []
