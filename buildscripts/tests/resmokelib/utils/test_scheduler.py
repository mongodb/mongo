"""Unit tests for buildscripts/resmokelib/utils/scheduler.py."""

import sched
import unittest

from buildscripts.resmokelib.utils import scheduler as _scheduler


def noop():
    pass


class TestScheduler(unittest.TestCase):
    """Unit tests for the Scheduler class."""
    scheduler = _scheduler.Scheduler

    def setUp(self):
        self.__scheduler = self.scheduler()

    def test_cancel_with_identical_time_and_priority(self):
        event1 = self.__scheduler.enterabs(time=0, priority=0, action=noop)
        event2 = self.__scheduler.enterabs(time=0, priority=0, action=noop)

        self.__scheduler.cancel(event1)
        self.assertIs(self.__scheduler.queue[0], event2)

        # Attempting to cancel the same event should fail because it has already been removed.
        with self.assertRaises(ValueError):
            self.__scheduler.cancel(event1)

        self.__scheduler.cancel(event2)
        self.assertEqual(self.__scheduler.queue, [])


class TestBuiltinScheduler(TestScheduler):
    """Unit tests for the sched.scheduler class."""
    scheduler = sched.scheduler

    def test_cancel_with_identical_time_and_priority(self):
        with self.assertRaises(AssertionError):
            super().test_cancel_with_identical_time_and_priority()
