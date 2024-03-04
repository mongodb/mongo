"""Unit tests for scheduler."""

import sched
import unittest


def noop():
    pass


class TestBuiltinScheduler(unittest.TestCase):
    """Unit tests for the 'sched.scheduler' class that is used in buildscripts/resmokelib/logging/flush.py."""

    scheduler = sched.scheduler

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
