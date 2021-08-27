"""Strategies for splitting tests into multiple sub-suites."""
from typing import List, Callable, Optional

import structlog

from buildscripts.util.teststats import TestRuntime

LOGGER = structlog.getLogger(__name__)

SplitStrategy = Callable[[List[TestRuntime], int, int, int], List[List[str]]]
FallbackStrategy = Callable[[List[str], int], List[List[str]]]


def divide_remaining_tests_among_suites(remaining_tests_runtimes: List[TestRuntime],
                                        suites: List[List[TestRuntime]]) -> None:
    """
    Divide the list of tests given among the suites given.

    :param remaining_tests_runtimes: Tests that still need to be added to a suite.
    :param suites: Lists of tests in their test suites.
    """
    suite_idx = 0
    for test_instance in remaining_tests_runtimes:
        current_suite = suites[suite_idx]
        current_suite.append(test_instance)
        suite_idx += 1
        if suite_idx >= len(suites):
            suite_idx = 0


def _new_suite_needed(current_suite: List[TestRuntime], test_runtime: float,
                      max_suite_runtime: float, max_tests_per_suite: Optional[int]) -> bool:
    """
    Check if a new suite should be created for the given suite.

    :param current_suite: Suite currently being added to.
    :param test_runtime: Runtime of test being added.
    :param max_suite_runtime: Max runtime of a single suite.
    :param max_tests_per_suite: Max number of tests in a suite.
    :return: True if a new test suite should be created.
    """
    current_runtime = sum(test.runtime for test in current_suite)
    if current_runtime + test_runtime > max_suite_runtime:
        # Will adding this test put us over the target runtime?
        return True

    if max_tests_per_suite and len(current_suite) + 1 > max_tests_per_suite:
        # Will adding this test put us over the max number of tests?
        return True

    return False


def greedy_division(tests_runtimes: List[TestRuntime], max_time_seconds: float,
                    max_suites: Optional[int] = None,
                    max_tests_per_suite: Optional[int] = None) -> List[List[str]]:
    """
    Divide the given tests into suites.

    Each suite should be able to execute in less than the max time specified. If a single
    test has a runtime greater than `max_time_seconds`, it will be run in a suite on its own.

    If max_suites is reached before assigning all tests to a suite, the remaining tests will be
    divided up among the created suites.

    Note: If `max_suites` is hit, suites may have more tests than `max_tests_per_suite` and may have
    runtimes longer than `max_time_seconds`.

    :param tests_runtimes: List of tuples containing test names and test runtimes.
    :param max_time_seconds: Maximum runtime to add to a single bucket.
    :param max_suites: Maximum number of suites to create.
    :param max_tests_per_suite: Maximum number of tests to add to a single suite.
    :return: List of Suite objects representing grouping of tests.
    """
    suites = []
    last_test_processed = len(tests_runtimes)
    LOGGER.debug("Determines suites for runtime", max_runtime_seconds=max_time_seconds,
                 max_suites=max_suites, max_tests_per_suite=max_tests_per_suite)
    current_test_list = []
    for idx, test_instance in enumerate(tests_runtimes):
        LOGGER.debug("Adding test", test=test_instance, suite_index=len(suites))
        if _new_suite_needed(current_test_list, test_instance.runtime, max_time_seconds,
                             max_tests_per_suite):
            LOGGER.debug("Finished suite", test_runtime=test_instance.runtime,
                         max_time=max_time_seconds, suite_index=len(suites))
            if current_test_list:
                suites.append(current_test_list)
                current_test_list = []
                if max_suites and len(suites) >= max_suites:
                    last_test_processed = idx
                    break

        current_test_list.append(test_instance)

    if current_test_list:
        suites.append(current_test_list)

    if max_suites and last_test_processed < len(tests_runtimes):
        # We must have hit the max suite limit, just randomly add the remaining tests to suites.
        divide_remaining_tests_among_suites(tests_runtimes[last_test_processed:], suites)

    return [[test.test_name for test in test_list] for test_list in suites]


def round_robin_fallback(test_list: List[str], max_suites: int) -> List[List[str]]:
    """
    Split the tests among a given number of suites picking them round robin.

    :param test_list: List of tests to divide.
    :param max_suites: Number of suites to create.
    :return: List of which tests should be in each suite.
    """
    num_suites = min(len(test_list), max_suites)
    test_lists = [[] for _ in range(num_suites)]
    for idx, test_file in enumerate(test_list):
        test_lists[idx % num_suites].append(test_file)

    return test_lists
