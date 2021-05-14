"""Unit tests for suite_split_strategies.py."""
import unittest

import buildscripts.task_generation.suite_split_strategies as under_test
from buildscripts.util.teststats import TestRuntime

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access,no-value-for-parameter


class TestDivideRemainingTestsAmongSuites(unittest.TestCase):
    @staticmethod
    def generate_tests_runtimes(n_tests):
        tests_runtimes = []
        # Iterating backwards so the list is sorted by descending runtimes
        for idx in range(n_tests - 1, -1, -1):
            name = "test_{0}".format(idx)
            tests_runtimes.append(TestRuntime(name, 2 * idx))

        return tests_runtimes

    def test_each_suite_gets_one_test(self):
        suites = [[] for _ in range(3)]
        tests_runtimes = self.generate_tests_runtimes(3)

        under_test.divide_remaining_tests_among_suites(tests_runtimes, suites)

        for suite in suites:
            self.assertEqual(len(suite), 1)

    def test_each_suite_gets_at_least_one_test(self):
        suites = [[] for _ in range(3)]
        tests_runtimes = self.generate_tests_runtimes(5)

        under_test.divide_remaining_tests_among_suites(tests_runtimes, suites)

        for suite in suites:
            self.assertGreaterEqual(len(suite), 1)

        total_tests = sum(len(suite) for suite in suites)
        self.assertEqual(total_tests, len(tests_runtimes))


class TestGreedyDivision(unittest.TestCase):
    def test_if_less_total_than_max_only_one_suite_created(self):
        max_time = 20
        tests_runtimes = [
            TestRuntime("test1", 5),
            TestRuntime("test2", 4),
            TestRuntime("test3", 3),
        ]

        suites = under_test.greedy_division(tests_runtimes, max_time)

        self.assertEqual(len(suites), 1)
        for test in tests_runtimes:
            self.assertIn(test.test_name, suites[0])

    def test_if_each_test_should_be_own_suite(self):
        max_time = 5
        tests_runtimes = [
            TestRuntime("test1", 5),
            TestRuntime("test2", 4),
            TestRuntime("test3", 3),
        ]

        suites = under_test.greedy_division(tests_runtimes, max_time)

        self.assertEqual(len(suites), 3)

    def test_if_test_is_greater_than_max_it_goes_alone(self):
        max_time = 7
        tests_runtimes = [
            TestRuntime("test1", 15),
            TestRuntime("test2", 4),
            TestRuntime("test3", 3),
        ]

        suites = under_test.greedy_division(tests_runtimes, max_time)

        self.assertEqual(len(suites), 2)
        self.assertEqual(len(suites[0]), 1)
        self.assertIn("test1", suites[0])

    def test_max_sub_suites_options(self):
        max_time = 5
        max_suites = 2
        tests_runtimes = [
            TestRuntime("test1", 5),
            TestRuntime("test2", 4),
            TestRuntime("test3", 3),
            TestRuntime("test4", 4),
            TestRuntime("test5", 3),
        ]

        suites = under_test.greedy_division(tests_runtimes, max_time, max_suites=max_suites)

        self.assertEqual(len(suites), max_suites)
        total_tests = sum(len(suite) for suite in suites)
        self.assertEqual(total_tests, len(tests_runtimes))

    def test_max_tests_per_suites_is_one(self):
        max_time = 5
        num_tests = 10
        tests_runtimes = [TestRuntime(f"tests_{i}", i) for i in range(num_tests)]

        suites = under_test.greedy_division(tests_runtimes, max_time, max_tests_per_suite=1)

        self.assertEqual(len(suites), num_tests)

    def test_max_tests_per_suites_is_less_than_number_of_tests(self):
        max_time = 100
        num_tests = 10
        tests_runtimes = [TestRuntime(f"tests_{i}", 1) for i in range(num_tests)]

        suites = under_test.greedy_division(tests_runtimes, max_time, max_tests_per_suite=2)

        self.assertEqual(len(suites), num_tests // 2)

    def test_max_suites_overrides_max_tests_per_suite(self):
        max_time = 100
        num_tests = 10
        max_suites = 2
        tests_runtimes = [TestRuntime(f"tests_{i}", 1) for i in range(num_tests)]

        suites = under_test.greedy_division(tests_runtimes, max_time, max_suites=max_suites,
                                            max_tests_per_suite=2)

        self.assertEqual(len(suites), max_suites)
