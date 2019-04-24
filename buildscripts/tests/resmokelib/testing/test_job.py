"""Unit tests for the resmokelib.testing.executor module."""

import logging
import unittest

import mock

from buildscripts.resmokelib.testing import job
from buildscripts.resmokelib.testing import queue_element
from buildscripts.resmokelib.utils import queue as _queue

# pylint: disable=missing-docstring,protected-access


class TestJob(unittest.TestCase):

    TESTS = ["jstests/core/and.js", "jstests/core/or.js"]

    @staticmethod
    def mock_testcase(test_name):
        testcase = mock.Mock()
        testcase.test_name = test_name
        testcase.REGISTERED_NAME = "js_test"
        testcase.logger = logging.getLogger("job_unittest")
        return testcase

    @staticmethod
    def mock_interrupt_flag():
        interrupt_flag = mock.Mock()
        interrupt_flag.is_set = lambda: False
        return interrupt_flag

    @staticmethod
    def get_suite_options(num_repeat_tests=None, time_repeat_tests_secs=None,
                          num_repeat_tests_min=None, num_repeat_tests_max=None):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests = num_repeat_tests
        suite_options.time_repeat_tests_secs = time_repeat_tests_secs
        suite_options.num_repeat_tests_min = num_repeat_tests_min
        suite_options.num_repeat_tests_max = num_repeat_tests_max
        return suite_options

    @staticmethod
    def queue_tests(tests, queue, queue_elem_type, suite_options):
        for test in tests:
            queue_elem = queue_elem_type(TestJob.mock_testcase(test), {}, suite_options)
            queue.put(queue_elem)

    @staticmethod
    def expected_run_num(time_repeat_tests_secs, test_time_secs):
        """Return the number of times a test is expected to run."""
        return time_repeat_tests_secs / test_time_secs

    def test__run_num_repeat(self):
        num_repeat_tests = 1
        queue = _queue.Queue()
        suite_options = self.get_suite_options(num_repeat_tests=num_repeat_tests)
        mock_time = MockTime(1)
        job_object = UnitJob(suite_options)
        self.queue_tests(self.TESTS, queue, queue_element.QueueElem, suite_options)
        job_object._get_time = mock_time.time
        job_object._run(queue, self.mock_interrupt_flag())
        self.assertEqual(job_object.total_test_num, num_repeat_tests * len(self.TESTS))
        for test in self.TESTS:
            self.assertEqual(job_object.tests[test], num_repeat_tests)

    def test__run_time_repeat_time_no_min_max(self):
        increment = 1
        time_repeat_tests_secs = 10
        expected_tests_run = self.expected_run_num(time_repeat_tests_secs, increment)
        queue = _queue.Queue()
        suite_options = self.get_suite_options(time_repeat_tests_secs=time_repeat_tests_secs)
        mock_time = MockTime(increment)
        job_object = UnitJob(suite_options)
        self.queue_tests(self.TESTS, queue, queue_element.QueueElemRepeatTime, suite_options)
        job_object._get_time = mock_time.time
        job_object._run(queue, self.mock_interrupt_flag())
        self.assertEqual(job_object.total_test_num, expected_tests_run * len(self.TESTS))
        for test in self.TESTS:
            self.assertEqual(job_object.tests[test], expected_tests_run)

    def test__run_time_repeat_time_no_min(self):
        increment = 1
        time_repeat_tests_secs = 10
        num_repeat_tests_max = 100
        expected_tests_run = self.expected_run_num(time_repeat_tests_secs, increment)
        queue = _queue.Queue()
        suite_options = self.get_suite_options(time_repeat_tests_secs=time_repeat_tests_secs,
                                               num_repeat_tests_max=num_repeat_tests_max)
        mock_time = MockTime(increment)
        job_object = UnitJob(suite_options)
        self.queue_tests(self.TESTS, queue, queue_element.QueueElemRepeatTime, suite_options)
        job_object._get_time = mock_time.time
        job_object._run(queue, self.mock_interrupt_flag())
        self.assertLess(job_object.total_test_num, num_repeat_tests_max * len(self.TESTS))
        for test in self.TESTS:
            self.assertEqual(job_object.tests[test], expected_tests_run)

    def test__run_time_repeat_time_no_max(self):
        increment = 1
        time_repeat_tests_secs = 10
        num_repeat_tests_min = 1
        expected_tests_run = self.expected_run_num(time_repeat_tests_secs, increment)
        queue = _queue.Queue()
        suite_options = self.get_suite_options(time_repeat_tests_secs=time_repeat_tests_secs,
                                               num_repeat_tests_min=num_repeat_tests_min)
        mock_time = MockTime(increment)
        job_object = UnitJob(suite_options)
        self.queue_tests(self.TESTS, queue, queue_element.QueueElemRepeatTime, suite_options)
        job_object._get_time = mock_time.time
        job_object._run(queue, self.mock_interrupt_flag())
        self.assertGreater(job_object.total_test_num, num_repeat_tests_min * len(self.TESTS))
        for test in self.TESTS:
            self.assertEqual(job_object.tests[test], expected_tests_run)

    def test__run_time_repeat_time(self):
        increment = 1
        time_repeat_tests_secs = 10
        num_repeat_tests_min = 1
        num_repeat_tests_max = 100
        expected_tests_run = self.expected_run_num(time_repeat_tests_secs, increment)
        queue = _queue.Queue()
        suite_options = self.get_suite_options(time_repeat_tests_secs=time_repeat_tests_secs,
                                               num_repeat_tests_min=num_repeat_tests_min,
                                               num_repeat_tests_max=num_repeat_tests_max)
        mock_time = MockTime(increment)
        job_object = UnitJob(suite_options)
        self.queue_tests(self.TESTS, queue, queue_element.QueueElemRepeatTime, suite_options)
        job_object._get_time = mock_time.time
        job_object._run(queue, self.mock_interrupt_flag())
        self.assertGreater(job_object.total_test_num, num_repeat_tests_min * len(self.TESTS))
        self.assertLess(job_object.total_test_num, num_repeat_tests_max * len(self.TESTS))
        for test in self.TESTS:
            self.assertEqual(job_object.tests[test], expected_tests_run)

    def test__run_time_repeat_min(self):
        increment = 1
        time_repeat_tests_secs = 2
        num_repeat_tests_min = 3
        num_repeat_tests_max = 100
        queue = _queue.Queue()
        suite_options = self.get_suite_options(time_repeat_tests_secs=time_repeat_tests_secs,
                                               num_repeat_tests_min=num_repeat_tests_min,
                                               num_repeat_tests_max=num_repeat_tests_max)
        mock_time = MockTime(increment)
        job_object = UnitJob(suite_options)
        self.queue_tests(self.TESTS, queue, queue_element.QueueElemRepeatTime, suite_options)
        job_object._get_time = mock_time.time
        job_object._run(queue, self.mock_interrupt_flag())
        self.assertEqual(job_object.total_test_num, num_repeat_tests_min * len(self.TESTS))
        for test in self.TESTS:
            self.assertEqual(job_object.tests[test], num_repeat_tests_min)

    def test__run_time_repeat_max(self):
        increment = 1
        time_repeat_tests_secs = 30
        num_repeat_tests_min = 1
        num_repeat_tests_max = 10
        expected_time_repeat_tests = self.expected_run_num(time_repeat_tests_secs, increment)
        queue = _queue.Queue()
        suite_options = self.get_suite_options(time_repeat_tests_secs=time_repeat_tests_secs,
                                               num_repeat_tests_min=num_repeat_tests_min,
                                               num_repeat_tests_max=num_repeat_tests_max)
        mock_time = MockTime(increment)
        job_object = UnitJob(suite_options)
        self.queue_tests(self.TESTS, queue, queue_element.QueueElemRepeatTime, suite_options)
        job_object._get_time = mock_time.time
        job_object._run(queue, self.mock_interrupt_flag())
        self.assertEqual(job_object.total_test_num, num_repeat_tests_max * len(self.TESTS))
        for test in self.TESTS:
            self.assertEqual(job_object.tests[test], num_repeat_tests_max)
            self.assertLess(job_object.tests[test], expected_time_repeat_tests)


class MockTime(object):
    """Class to mock time.time."""

    def __init__(self, increment):
        """Initialize with an increment which simulates a time increment."""
        self._time = 0
        self._increment = increment

    def time(self):
        """Simulate time.time by incrementing for every invocation."""
        cur_time = self._time
        self._time += self._increment
        return cur_time


class UnitJob(job.Job):  # pylint: disable=too-many-instance-attributes
    def __init__(self, suite_options):  #pylint: disable=super-init-not-called
        self.job_num = 0
        self.logger = logging.getLogger("job_unittest")
        self.fixture = None
        self.hooks = []
        self.report = None
        self.archival = None
        self.suite_options = suite_options
        self.test_queue_logger = logging.getLogger("job_unittest")
        self.total_test_num = 0
        self.tests = {}

    def _execute_test(self, test):
        self.total_test_num += 1
        if test.test_name not in self.tests:
            self.tests[test.test_name] = 0
        self.tests[test.test_name] += 1
