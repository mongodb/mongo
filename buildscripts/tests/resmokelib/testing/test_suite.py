"""Unit tests for the resmokelib.testing.suite module."""

import unittest

from mock import MagicMock

from buildscripts.resmokelib.testing import suite as under_test
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


class TestGetTestsForKind(unittest.TestCase):
    def setUp(self):
        self.suite = under_test.Suite("suite_name", {"test_kind": "js_test"})
        self.suite._tests = ["t/test1.js", "t/test2.js", "t/test3.js"]
        self.suite._suite_config = {
            "selector": {"roots": ["testroot/**"]},
            "include_files": ["testroot/test1.js, testroot/test2.js", "testroot/test3.js"],
        }

        self.default_evergreen_task_id = under_test._config.EVERGREEN_TASK_ID
        self.default_enable_evergreen_api_test_selection = (
            under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION
        )
        self.default_evergreen_test_selection_strategy = (
            under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY
        )
        self.default_evergreen_project_name = under_test._config.EVERGREEN_PROJECT_NAME
        self.default_evergreen_variant_name = under_test._config.EVERGREEN_VARIANT_NAME
        self.default_evergreen_requester = under_test._config.EVERGREEN_REQUESTER
        self.default_evergreen_task_id = under_test._config.EVERGREEN_TASK_ID
        self.default_evergreen_task_name = under_test._config.EVERGREEN_TASK_NAME

    def tearDown(self):
        under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION = (
            self.default_enable_evergreen_api_test_selection
        )
        under_test._config.EVERGREEN_PROJECT_NAME = self.default_evergreen_project_name
        under_test._config.EVERGREEN_VARIANT_NAME = self.default_evergreen_variant_name
        under_test._config.EVERGREEN_REQUESTER = self.default_evergreen_requester
        under_test._config.EVERGREEN_TASK_ID = self.default_evergreen_task_id
        under_test._config.EVERGREEN_TASK_NAME = self.default_evergreen_task_name
        under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY = (
            self.default_evergreen_test_selection_strategy
        )

    def test_simple(self):
        self.assertFalse(under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION)

        tests, excluded = self.suite._get_tests_for_kind("js_test")
        self.assertEqual(tests, ["testroot"])
        self.assertEqual(excluded, [])

    def test_with_test_selection_strategy(self):
        under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION = True
        under_test._config.EVERGREEN_PROJECT_NAME = "project_name"
        under_test._config.EVERGREEN_VARIANT_NAME = "variant_name"
        under_test._config.EVERGREEN_REQUESTER = "requester"
        under_test._config.EVERGREEN_TASK_ID = "task_id"
        under_test._config.EVERGREEN_TASK_NAME = "task_name"
        under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY = "strategy"

        # Currently raises an exception because the Evergreen endpoint is still under development
        with self.assertRaises(RuntimeError) as context:
            self.suite._get_tests_for_kind("js_test")
        self.assertEqual(
            str(context.exception),
            "Failure using the select tests evergreen endpoint with the following request:\n"
            + "{'project_id': 'project_name', 'build_variant': 'variant_name', 'requester': 'requester', 'task_id': 'task_id', 'task_name': 'task_name', 'tests': ['testroot'], 'strategies': 'strategy'}",
        )
        self.assertEqual(
            str(context.exception.__cause__),
            "400 Client Error: Bad Request for url: https://evergreen.mongodb.com/rest/v2/select/tests",
        )
