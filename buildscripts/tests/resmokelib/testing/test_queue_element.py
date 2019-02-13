"""Unit tests for the resmokelib.testing.executor module."""
import unittest

import mock

from buildscripts.resmokelib.testing import queue_element

# pylint: disable=missing-docstring,protected-access


class TestQueueElemRepeatNum(unittest.TestCase):
    def test_norequeue(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests = 1
        queue_elem = queue_element.QueueElemRepeatNum(None, None, suite_options)
        queue_elem.job_completed(1)
        self.assertFalse(queue_elem.should_requeue())

    def test_requeue(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests = 2
        queue_elem = queue_element.QueueElemRepeatNum(None, None, suite_options)
        queue_elem.job_completed(1)
        self.assertTrue(queue_elem.should_requeue())
        queue_elem.job_completed(1)
        self.assertFalse(queue_elem.should_requeue())


class TestQueueElemRepeatTime(unittest.TestCase):
    def test_requeue_time_only(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = None
        suite_options.num_repeat_tests_max = None
        suite_options.time_repeat_tests_secs = 7
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 3
        queue_elem.job_completed(job_time)
        self.assertTrue(queue_elem.should_requeue())
        queue_elem.job_completed(job_time)
        self.assertFalse(queue_elem.should_requeue())

    def test_should_requeue_time_exact_avg(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = None
        suite_options.num_repeat_tests_max = None
        suite_options.time_repeat_tests_secs = 9
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 3
        queue_elem.job_completed(job_time)
        self.assertTrue(queue_elem.should_requeue())
        queue_elem.job_completed(job_time)
        self.assertTrue(queue_elem.should_requeue())
        queue_elem.job_completed(job_time)
        self.assertFalse(queue_elem.should_requeue())

    def test_should_requeue_time_with_min(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = 3
        suite_options.num_repeat_tests_max = None
        suite_options.time_repeat_tests_secs = 5
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 3
        queue_elem.job_completed(job_time)
        self.assertTrue(queue_elem.should_requeue())
        queue_elem.job_completed(job_time)
        self.assertTrue(queue_elem.should_requeue())
        queue_elem.job_completed(job_time)
        self.assertFalse(queue_elem.should_requeue())

    def test_should_requeue_time_with_max(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = None
        suite_options.num_repeat_tests_max = 2
        suite_options.time_repeat_tests_secs = 10
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 2
        queue_elem.job_completed(job_time)
        self.assertTrue(queue_elem.should_requeue())
        queue_elem.job_completed(job_time)
        self.assertFalse(queue_elem.should_requeue())

    def test_should_requeue_time_with_min_max(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = 1
        suite_options.num_repeat_tests_max = 2
        suite_options.time_repeat_tests_secs = 10
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 1
        queue_elem.job_completed(job_time)
        self.assertTrue(queue_elem.should_requeue())
        queue_elem.job_completed(job_time)
        self.assertFalse(queue_elem.should_requeue())
