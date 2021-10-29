"""Unit tests for the resmokelib.testing.suite module."""
import unittest

from buildscripts.resmokelib.testing import suite as under_test

# pylint: disable=missing-docstring,protected-access


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
