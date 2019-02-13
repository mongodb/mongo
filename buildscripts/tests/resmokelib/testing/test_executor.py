"""Unit tests for the resmokelib.testing.executor module."""
import logging
import unittest

import mock

from buildscripts.resmokelib.testing import executor
from buildscripts.resmokelib.testing import queue_element

# pylint: disable=missing-docstring,protected-access


class TestTestSuiteExecutor(unittest.TestCase):
    @staticmethod
    def mock_suite():
        suite = mock.Mock()
        suite.test_kind = "js_test"
        suite.tests = ["jstests/core/and.js", "jstests/core/and2.js"]
        suite.options = mock.Mock()
        return suite

    def test__make_test_queue_time_repeat(self):
        suite = self.mock_suite()
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
        suite = self.mock_suite()
        suite.options.time_repeat_tests_secs = None
        executor_object = UnitTestExecutor(suite, {})
        test_queue = executor_object._make_test_queue()
        self.assertFalse(test_queue.empty())
        self.assertEqual(test_queue.qsize(), len(suite.tests))
        for suite_test in suite.tests:
            test_element = test_queue.get_nowait()
            self.assertIsInstance(test_element, queue_element.QueueElemRepeatNum)
            self.assertEqual(test_element.testcase.test_name, suite_test)
        self.assertTrue(test_queue.empty())


class UnitTestExecutor(executor.TestSuiteExecutor):
    def __init__(self, suite, config):  # pylint: disable=super-init-not-called
        self._suite = suite
        self.test_queue_logger = logging.getLogger("executor_unittest")
        self.test_config = config
