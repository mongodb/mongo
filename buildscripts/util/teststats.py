"""Utility to support parsing a TestStat."""

import sys
import os
import logging

from collections import defaultdict
from collections import namedtuple
import buildscripts.util.testname as testname  # pylint: disable=wrong-import-position

TestRuntime = namedtuple('TestRuntime', ['test_name', 'runtime'])


def normalize_test_name(test_name):
    """Normalize test names that may have been run on windows or unix."""
    return test_name.replace("\\", "/")


class TestStats(object):
    """Represent the test statistics for the task that is being analyzed."""

    def __init__(self, evg_test_stats_results):
        """Initialize the TestStats with raw results from the Evergreen API."""
        # Mapping from test_file to {"num_run": X, "duration": Y} for tests
        self._runtime_by_test = defaultdict(dict)
        # Mapping from test_name to {"num_run": X, "duration": Y} for hooks
        self._hook_runtime_by_test = defaultdict(dict)

        for doc in evg_test_stats_results:
            self._add_stats(doc)

    def _add_stats(self, test_stats):
        """Add the statistics found in a document returned by the Evergreen test_stats/ endpoint."""
        test_file = testname.normalize_test_file(test_stats.test_file)
        duration = test_stats.avg_duration_pass
        num_run = test_stats.num_pass
        is_hook = testname.is_resmoke_hook(test_file)
        if is_hook:
            self._add_test_hook_stats(test_file, duration, num_run)
        else:
            self._add_test_stats(test_file, duration, num_run)

    def _add_test_stats(self, test_file, duration, num_run):
        """Add the statistics for a test."""
        self._add_runtime_info(self._runtime_by_test, test_file, duration, num_run)

    def _add_test_hook_stats(self, test_file, duration, num_run):
        """Add the statistics for a hook."""
        test_name = testname.split_test_hook_name(test_file)[0]
        self._add_runtime_info(self._hook_runtime_by_test, test_name, duration, num_run)

    @staticmethod
    def _add_runtime_info(runtime_dict, test_name, duration, num_run):
        runtime_info = runtime_dict[test_name]
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
            hook_runtime_info = self._hook_runtime_by_test[test_name]
            if hook_runtime_info:
                duration += hook_runtime_info["duration"]
            test = TestRuntime(test_name=normalize_test_name(test_file), runtime=duration)
            tests.append(test)
        return sorted(tests, key=lambda x: x.runtime, reverse=True)
