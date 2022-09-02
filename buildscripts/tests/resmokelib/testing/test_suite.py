"""Unit tests for the resmokelib.testing.suite module."""
import unittest

from mock import MagicMock

from buildscripts.resmokelib.testing import suite as under_test

# pylint: disable=protected-access
from buildscripts.resmokelib.testing.testcases.interface import TestCase


class TestNumTimesToRepeatTests(unittest.TestCase):
    def setUp(self):
        self.default_repeat_tests = under_test._config.REPEAT_TESTS
        self.suite = under_test.Suite("suite_name", {"test_kind": "js_test"})

    def tearDown(self):
        under_test._config.REPEAT_TESTS = self.default_repeat_tests

    def test_without_num_repeat_tests(self):
        expected_num_repeat_tests = 1
        num_repeat_tests = self.suite.get_num_times_to_repeat_tests()
        self.assertEqual(num_repeat_tests, expected_num_repeat_tests)

    def test_with_num_repeat_tests(self):
        expected_num_repeat_tests = 5
        under_test._config.REPEAT_TESTS = expected_num_repeat_tests
        num_repeat_tests = self.suite.get_num_times_to_repeat_tests()
        self.assertEqual(num_repeat_tests, expected_num_repeat_tests)


class TestNumJobsToStart(unittest.TestCase):
    def setUp(self):
        self.default_repeat_tests = under_test._config.REPEAT_TESTS
        self.default_num_jobs = under_test._config.JOBS

        self.suite = under_test.Suite("suite_name", {"test_kind": "js_test"})
        self.suite._tests = []
        self.num_tests = 5
        for _ in range(self.num_tests):
            self.suite._tests.append(MagicMock(TestCase))

    def tearDown(self):
        under_test._config.REPEAT_TESTS = self.default_repeat_tests
        under_test._config.JOBS = self.default_num_jobs

    def test_num_tests_gte_num_jobs(self):
        num_repeat = 2
        under_test._config.JOBS = 100
        under_test._config.REPEAT_TESTS = num_repeat
        self.assertEqual(self.num_tests * num_repeat, self.suite.get_num_jobs_to_start())
