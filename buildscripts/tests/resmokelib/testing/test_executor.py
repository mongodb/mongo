"""Unit tests for the resmokelib.testing.executor module."""
import logging
import unittest

import mock

from buildscripts.resmokelib.testing import executor
from buildscripts.resmokelib.testing import queue_element

# pylint: disable=missing-docstring,protected-access

NS = "buildscripts.resmokelib.testing.executor"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def mock_suite(n_tests):
    suite = mock.MagicMock()
    suite.test_kind = "js_test"
    suite.tests = ["jstests/core/and{}.js".format(i) for i in range(n_tests)]
    suite.options.num_repeat_tests = None
    return suite


class TestTestSuiteExecutor(unittest.TestCase):
    def test__make_test_queue_time_repeat(self):
        suite = mock_suite(2)
        suite.options.time_repeat_tests_secs = 30
        executor_object = UnitTestExecutor(suite, {})
        test_queue = executor_object._make_test_queue()
        self.assertFalse(test_queue.empty())
        self.assertEqual(test_queue.qsize(), len(suite.tests))
        for suite_test in suite.tests:
            test_element = test_queue.get_nowait()
            self.assertIsInstance(test_element, queue_element.QueueElemRepeatTime)
            self.assertEqual(test_element.testcase.test_name, suite_test)
        self.assertTrue(test_queue.empty())

    def test__make_test_queue_num_repeat(self):
        suite = mock_suite(2)
        suite.options.time_repeat_tests_secs = None
        executor_object = UnitTestExecutor(suite, {})
        test_queue = executor_object._make_test_queue()
        self.assertFalse(test_queue.empty())
        self.assertEqual(test_queue.qsize(), len(suite.tests))
        for suite_test in suite.tests:
            test_element = test_queue.get_nowait()
            self.assertIsInstance(test_element, queue_element.QueueElem)
            self.assertEqual(test_element.testcase.test_name, suite_test)
        self.assertTrue(test_queue.empty())


class TestNumJobsToStart(unittest.TestCase):
    def test_num_tests_gt_num_jobs(self):
        num_tests = 8
        num_jobs = 1
        suite = mock_suite(num_tests)
        suite.options.num_jobs = num_jobs
        ut_executor = UnitTestExecutor(suite, None)

        self.assertEqual(num_jobs, ut_executor._num_jobs_to_start(suite, num_tests))

    def test_num_tests_lt_num_jobs(self):
        num_tests = 2
        suite = mock_suite(num_tests)
        suite.options.num_jobs = 8
        ut_executor = UnitTestExecutor(suite, None)

        self.assertEqual(num_tests, ut_executor._num_jobs_to_start(suite, num_tests))


class TestCreateJobs(unittest.TestCase):
    def setUp(self):
        self.suite = mock_suite(1)
        self.ut_executor = UnitTestExecutor(self.suite, None)
        self.ut_executor._make_job = mock.MagicMock()

    def test_create_one_job(self):
        num_jobs = 1
        self.ut_executor._num_jobs_to_start = lambda x, y: num_jobs
        self.ut_executor._create_jobs(1)
        self.ut_executor._make_job.assert_called_once_with(0)

    def test_create_multiple_jobs(self):
        num_jobs = 8
        self.ut_executor._num_jobs_to_start = lambda x, y: num_jobs
        self.ut_executor._create_jobs(1)
        self.assertEqual(num_jobs, self.ut_executor._make_job.call_count)


class TestNumTimesToRepeatTests(unittest.TestCase):
    def test_default(self):
        num_tests = 1
        suite = mock_suite(num_tests)
        ut_executor = UnitTestExecutor(suite, None)
        self.assertEqual(1, ut_executor._num_times_to_repeat_tests())

    def test_with_num_repeat_tests(self):
        num_tests = 1
        suite = mock_suite(num_tests)
        suite.options.num_repeat_tests = 5
        ut_executor = UnitTestExecutor(suite, None)
        self.assertEqual(suite.options.num_repeat_tests, ut_executor._num_times_to_repeat_tests())


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
        self.suite.options.num_repeat_tests = num_repeats
        test_queue = self.ut_executor._make_test_queue()
        self.assertEqual(num_repeats * len(self.suite.tests), test_queue.qsize())
        while not test_queue.empty():
            element = test_queue.get()
            self.assertIn(element, self.suite.tests)


class UnitTestExecutor(executor.TestSuiteExecutor):
    def __init__(self, suite, config):  # pylint: disable=super-init-not-called
        self._suite = suite
        self.test_queue_logger = logging.getLogger("executor_unittest")
        self.test_config = config
        self.logger = mock.MagicMock()
