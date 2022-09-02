"""Unit tests for the resmokelib.testing.executor module."""
import unittest

import mock

from buildscripts.resmokelib.testing import queue_element


class TestQueueElemFactory(unittest.TestCase):
    def test_without_time(self):
        suite_options = mock.Mock()
        suite_options.time_repeat_tests_secs = None
        queue_elem = queue_element.queue_elem_factory(None, None, suite_options)
        self.assertIsInstance(queue_elem, queue_element.QueueElem)

    def test_with_time(self):
        suite_options = mock.Mock()
        suite_options.time_repeat_tests_secs = 5
        queue_elem = queue_element.queue_elem_factory(None, None, suite_options)
        self.assertIsInstance(queue_elem, queue_element.QueueElemRepeatTime)


class TestQueueElem(unittest.TestCase):
    def test_norequeue(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests = 1
        queue_elem = queue_element.QueueElem(None, None, suite_options)
        queue_elem.job_completed(1)
        self.assertFalse(queue_elem.should_requeue())


class TestQueueElemRepeatTime(unittest.TestCase):
    def assert_elem_queues_n_times(self, queue_elem, run_time, n_expected):
        """
        Assert that the queue_elem should be requeued after a certain number of runs.

        :param queue_elem: Element to test.
        :param run_time: Runtime of each job execution.
        :param n_expected: Number of times the job should be run.
        """
        for _ in range(n_expected):
            self.assertTrue(queue_elem.should_requeue())
            queue_elem.job_completed(run_time)

        self.assertFalse(queue_elem.should_requeue())

    def test_requeue_time_only(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = None
        suite_options.num_repeat_tests_max = None
        suite_options.time_repeat_tests_secs = 7
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 3
        self.assert_elem_queues_n_times(queue_elem, job_time, 3)

    def test_should_requeue_time_exact_avg(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = None
        suite_options.num_repeat_tests_max = None
        suite_options.time_repeat_tests_secs = 9
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 3
        self.assert_elem_queues_n_times(queue_elem, job_time, 3)

    def test_should_requeue_time_with_min(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = 3
        suite_options.num_repeat_tests_max = None
        suite_options.time_repeat_tests_secs = 5
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 3
        self.assert_elem_queues_n_times(queue_elem, job_time, 3)

    def test_should_requeue_time_with_max(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = None
        suite_options.num_repeat_tests_max = 2
        suite_options.time_repeat_tests_secs = 10
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 2
        self.assert_elem_queues_n_times(queue_elem, job_time, 2)

    def test_should_requeue_time_with_min_max(self):
        suite_options = mock.Mock()
        suite_options.num_repeat_tests_min = 1
        suite_options.num_repeat_tests_max = 2
        suite_options.time_repeat_tests_secs = 10
        queue_elem = queue_element.QueueElemRepeatTime(None, None, suite_options)
        job_time = 1
        self.assert_elem_queues_n_times(queue_elem, job_time, 2)
