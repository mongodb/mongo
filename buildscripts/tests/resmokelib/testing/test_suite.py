"""Unit tests for the resmokelib.testing.suite module."""

import logging
import unittest

from mock import MagicMock

from buildscripts.resmokelib.logging import loggers
from buildscripts.resmokelib.testing import suite as under_test
from buildscripts.resmokelib.testing.suite import EqualRuntime
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
        loggers.ROOT_EXECUTOR_LOGGER = logging
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
        # mock selector
        mock_selector = MagicMock()
        mock_selector.filter_tests.return_value = (["test1", "test2"], ["excluded_test"])
        mock_selector.group_tests.return_value = ["test1", "test2"]

        # Mock _config values
        under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION = True
        under_test._config.EVERGREEN_PROJECT_NAME = "project_name"
        under_test._config.EVERGREEN_VARIANT_NAME = "variant_name"
        under_test._config.EVERGREEN_REQUESTER = "requester"
        under_test._config.EVERGREEN_TASK_ID = "task_id"
        under_test._config.EVERGREEN_TASK_NAME = "task_name"
        under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY = "strategy"

        # Mock Evergreen API
        mock_evg_api = MagicMock()
        mock_evg_api.select_tests.return_value = {"tests": ["test1", "test2"]}
        under_test.evergreen_conn = MagicMock()  # Mock evergreen_conn object
        under_test.evergreen_conn.get_evergreen_api.return_value = mock_evg_api

        # Replace _selector in under_test with the mock
        under_test._selector = mock_selector

        # Call `_get_tests_for_kind` and verify its behavior
        tests, excluded = self.suite._get_tests_for_kind("js_test")

        # Assert the expected results
        self.assertEqual(tests, ["test1", "test2"])
        self.assertEqual(excluded, ["excluded_test"])

        mock_selector.filter_tests.assert_called_once_with(
            "js_test", self.suite.get_selector_config()
        )

        if under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION:
            mock_evg_api.select_tests.assert_called_once_with(
                project_id="project_name",
                build_variant="variant_name",
                requester="requester",
                task_id="task_id",
                task_name="task_name",
                tests=["test1", "test2"],
                strategies="strategy",
            )

    def test_sharding(self):
        tests = ["1.js", "2.js", "3.js"]
        shard_count = 2
        shard1 = self.suite.filter_tests_for_shard(tests, shard_count, 0)
        shard2 = self.suite.filter_tests_for_shard(tests, shard_count, 1)
        self.assertEqual(set(shard1), set(["1.js", "3.js"]))
        self.assertEqual(shard2, ["2.js"])

        actual = shard1 + shard2
        actual.sort()
        self.assertEqual(actual, tests)

    def test_runtime_sharding(self):
        tests = ["1.js", "2.js", "3.js", "4.js"]
        runtimes = [
            {
                "test_name": "1.js",
                "avg_duration_pass": 5,
            },
            {
                "test_name": "2.js",
                "avg_duration_pass": 1,
            },
            {
                "test_name": "3.js",
                "avg_duration_pass": 2,
            },
            {
                "test_name": "4.js",
                "avg_duration_pass": 2,
            },
        ]
        shard_count = 2

        strategy = EqualRuntime(runtimes=runtimes)
        shard1 = strategy.get_tests_for_shard(tests, shard_count, 0)
        shard2 = strategy.get_tests_for_shard(tests, shard_count, 1)

        self.assertEqual(
            shard1,
            [
                "1.js",
            ],
        )
        self.assertEqual(set(shard2), set(["2.js", "3.js", "4.js"]))

        actual = shard1 + shard2
        actual.sort()
        self.assertEqual(actual, tests)

    def test_runtime_sharding_no_history(self):
        tests = ["1.js", "2.js", "3.js", "4.js"]
        runtimes = []
        shard_count = 2

        strategy = EqualRuntime(runtimes=runtimes)
        shard1 = strategy.get_tests_for_shard(tests, shard_count, 0)
        shard2 = strategy.get_tests_for_shard(tests, shard_count, 1)

        self.assertEqual(set(shard1), set(["1.js", "3.js"]))
        self.assertEqual(set(shard2), set(["2.js", "4.js"]))

    def test_runtime_sharding_partial_history(self):
        tests = ["1.js", "2.js", "3.js", "4.js"]
        runtimes = [
            {
                "test_name": "1.js",
                "avg_duration_pass": 1,
            },
        ]
        shard_count = 2

        strategy = EqualRuntime(runtimes=runtimes)
        shard1 = strategy.get_tests_for_shard(tests, shard_count, 0)
        shard2 = strategy.get_tests_for_shard(tests, shard_count, 1)

        self.assertEqual(set(shard1), set(["1.js", "3.js"]))
        self.assertEqual(set(shard2), set(["2.js", "4.js"]))
