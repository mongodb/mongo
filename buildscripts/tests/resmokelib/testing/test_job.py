"""Unit tests for the resmokelib.testing.executor module."""

import logging
import threading
import unittest

import mock

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing import job
from buildscripts.resmokelib.testing import queue_element
from buildscripts.resmokelib.testing.fixtures import interface as _fixtures
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib
from buildscripts.resmokelib.utils import queue as _queue

# pylint: disable=protected-access


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


class UnitJob(job.Job):
    def __init__(self, suite_options):
        super(UnitJob, self).__init__(0, logging.getLogger("job_unittest"), None, [], None, None,
                                      suite_options, logging.getLogger("job_unittest"))
        self.total_test_num = 0
        self.tests = {}

    def _execute_test(self, test, hook_failure_flag=None):
        self.total_test_num += 1
        if test.test_name not in self.tests:
            self.tests[test.test_name] = 0
        self.tests[test.test_name] += 1


class TestFixtureSetupAndTeardown(unittest.TestCase):
    """Test cases for error handling around setup_fixture() and teardown_fixture()."""

    def setUp(self):
        logger = logging.getLogger("job_unittest")
        self.__job_object = job.Job(job_num=0, logger=logger, fixture=None, hooks=[], report=None,
                                    archival=None, suite_options=None, test_queue_logger=logger)

        # Initialize the Job instance such that its setup_fixture() and teardown_fixture() methods
        # always indicate success. The settings for these mocked method will be changed in the
        # individual test cases below.
        self.__job_object.manager.setup_fixture = mock.Mock(return_value=True)
        self.__job_object.manager.teardown_fixture = mock.Mock(return_value=True)

    def __assert_when_run_tests(self, setup_succeeded=True, teardown_succeeded=True):
        queue = _queue.Queue()
        interrupt_flag = threading.Event()
        setup_flag = threading.Event()
        teardown_flag = threading.Event()

        self.__job_object(queue, interrupt_flag, setup_flag, teardown_flag)

        self.assertEqual(setup_succeeded, not interrupt_flag.is_set())
        self.assertEqual(setup_succeeded, not setup_flag.is_set())
        self.assertEqual(teardown_succeeded, not teardown_flag.is_set())

        # teardown_fixture() should be called even if setup_fixture() raises an exception.
        self.__job_object.manager.setup_fixture.assert_called()
        self.__job_object.manager.teardown_fixture.assert_called()

    def test_setup_and_teardown_both_succeed(self):
        self.__assert_when_run_tests()

    def test_setup_returns_failure(self):
        self.__job_object.manager.setup_fixture.return_value = False
        self.__assert_when_run_tests(setup_succeeded=False)

    def test_setup_raises_logging_config_exception(self):
        self.__job_object.manager.setup_fixture.side_effect = errors.LoggerRuntimeConfigError(
            "Logging configuration error intentionally raised in unit test")
        self.__assert_when_run_tests(setup_succeeded=False)

    def test_setup_raises_unexpected_exception(self):
        self.__job_object.manager.setup_fixture.side_effect = Exception(
            "Generic error intentionally raised in unit test")
        self.__assert_when_run_tests(setup_succeeded=False)

    def test_teardown_returns_failure(self):
        self.__job_object.manager.teardown_fixture.return_value = False
        self.__assert_when_run_tests(teardown_succeeded=False)

    def test_teardown_raises_logging_config_exception(self):
        self.__job_object.manager.teardown_fixture.side_effect = errors.LoggerRuntimeConfigError(
            "Logging configuration error intentionally raised in unit test")
        self.__assert_when_run_tests(teardown_succeeded=False)

    def test_teardown_raises_unexpected_exception(self):
        self.__job_object.manager.teardown_fixture.side_effect = Exception(
            "Generic error intentionally raised in unit test")
        self.__assert_when_run_tests(teardown_succeeded=False)


class TestNoOpFixtureSetupAndTeardown(unittest.TestCase):
    """Test cases for NoOpFixture handling in setup_fixture() and teardown_fixture()."""

    def setUp(self):
        self.logger = logging.getLogger("job_unittest")
        fixturelib = FixtureLib()
        self.__noop_fixture = _fixtures.NoOpFixture(logger=self.logger, job_num=0,
                                                    fixturelib=fixturelib)
        self.__noop_fixture.setup = mock.Mock()
        self.__noop_fixture.teardown = mock.Mock()

        test_report = mock.Mock()
        test_report.find_test_info().status = "pass"

        self.__job_object = job.Job(job_num=0, logger=self.logger, fixture=self.__noop_fixture,
                                    hooks=[], report=test_report, archival=None, suite_options=None,
                                    test_queue_logger=self.logger)

    def test_setup_called_for_noop_fixture(self):
        self.assertTrue(self.__job_object.manager.setup_fixture(self.logger))
        self.__noop_fixture.setup.assert_called_once_with()

    def test_teardown_called_for_noop_fixture(self):
        self.assertTrue(self.__job_object.manager.teardown_fixture(self.logger))
        self.__noop_fixture.teardown.assert_called_once_with(finished=True)
