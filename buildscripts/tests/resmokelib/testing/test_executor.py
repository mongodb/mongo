"""Unit tests for the resmokelib.testing.executor module."""
import logging
import unittest

import mock

from buildscripts.resmokelib.testing import executor
from buildscripts.resmokelib.testing.suite import Suite

# pylint: disable=protected-access

NS = "buildscripts.resmokelib.testing.executor"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def mock_suite(n_tests):
    suite = mock.MagicMock()
    suite.test_kind = "js_test"
    suite.tests = ["jstests/core/and{}.js".format(i) for i in range(n_tests)]
    suite.get_num_times_to_repeat_tests.return_value = 1
    suite.make_test_case_names_list = lambda: Suite.make_test_case_names_list(suite)
    return suite


class UnitTestExecutor(executor.TestSuiteExecutor):
    def __init__(self, suite, config):  # pylint: disable=super-init-not-called
        self._suite = suite
        self.test_queue_logger = logging.getLogger("executor_unittest")
        self.test_config = config
        self.logger = mock.MagicMock()


class TestCreateJobs(unittest.TestCase):
    def setUp(self):
        self.suite = mock_suite(1)
        self.ut_executor = UnitTestExecutor(self.suite, None)
        self.ut_executor._make_job = mock.MagicMock()

    def test_create_one_job(self):
        self.ut_executor._create_jobs(1)
        self.ut_executor._make_job.assert_called_once_with(0)

    def test_create_multiple_jobs(self):
        num_jobs = 8
        self.ut_executor._create_jobs(num_jobs)
        self.assertEqual(num_jobs, self.ut_executor._make_job.call_count)


class TestCreateQueueElemForTestName(unittest.TestCase):
    @mock.patch(ns("testcases.make_test_case"))
    @mock.patch(ns("queue_elem_factory"))
    def test_queue_elem_created_for_test_name(self, queue_elem_mock, make_test_case_mock):
        num_tests = 1
        test_config = {}
        suite = mock_suite(num_tests)
        ut_executor = UnitTestExecutor(suite, test_config)
        queue_elem = ut_executor._create_queue_elem_for_test_name('test_name')
        self.assertEqual(queue_elem_mock.return_value, queue_elem)
        make_test_case_mock.assert_called_with(suite.test_kind, ut_executor.test_queue_logger,
                                               'test_name', **test_config)
        queue_elem_mock.assert_called_with(make_test_case_mock.return_value, test_config,
                                           suite.options)


class TestMakeTestQueue(unittest.TestCase):
    def setUp(self):
        self.suite = mock_suite(3)
        self.ut_executor = UnitTestExecutor(self.suite, None)
        self.ut_executor._create_queue_elem_for_test_name = lambda x: x

    def test_repeat_once(self):
        test_queue = self.ut_executor._make_test_queue()
        self.assertEqual(len(self.suite.tests), test_queue.qsize())
        while not test_queue.empty():
            element = test_queue.get()
            self.assertIn(element, self.suite.tests)

    def test_repeat_three_times(self):
        num_repeats = 3
        self.suite.get_num_times_to_repeat_tests.return_value = num_repeats
        test_queue = self.ut_executor._make_test_queue()
        self.assertEqual(num_repeats * len(self.suite.tests), test_queue.qsize())
        while not test_queue.empty():
            element = test_queue.get()
            self.assertIn(element, self.suite.tests)


class TestTestQueueAddTestCases(unittest.TestCase):
    def setUp(self):
        self.default_max_test_queue_size = executor._config.MAX_TEST_QUEUE_SIZE
        self.num_test_cases = 3
        self.test_cases = [mock.MagicMock() for _ in range(self.num_test_cases)]

    def tearDown(self):
        executor._config.MAX_TEST_QUEUE_SIZE = self.default_max_test_queue_size

    def test_do_not_set_max_test_queue_size(self):
        test_queue = executor.TestQueue()
        test_queue.add_test_cases(self.test_cases)
        self.assertEqual(test_queue.num_tests, self.num_test_cases)
        while not test_queue.empty():
            element = test_queue.get()
            self.assertIn(element, self.test_cases)

    def test_max_test_queue_size_not_reached(self):
        max_test_queue_size = 10
        self.assertTrue(max_test_queue_size > self.num_test_cases)
        executor._config.MAX_TEST_QUEUE_SIZE = max_test_queue_size
        test_queue = executor.TestQueue()
        test_queue.add_test_cases(self.test_cases)
        self.assertEqual(test_queue.num_tests, self.num_test_cases)
        while not test_queue.empty():
            element = test_queue.get()
            self.assertIn(element, self.test_cases)

    def test_max_test_queue_size_exceeded(self):
        max_test_queue_size = 2
        self.assertTrue(max_test_queue_size < self.num_test_cases)
        executor._config.MAX_TEST_QUEUE_SIZE = max_test_queue_size
        test_queue = executor.TestQueue()
        test_queue.add_test_cases(self.test_cases)
        self.assertEqual(test_queue.num_tests, max_test_queue_size)
        while not test_queue.empty():
            element = test_queue.get()
            self.assertIn(element, self.test_cases)
