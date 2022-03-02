"""Unit tests for the evergreen_task_timeout script."""
from datetime import timedelta
import unittest

import buildscripts.evergreen_task_timeout as under_test

# pylint: disable=missing-docstring,no-self-use


class DetermineTimeoutTest(unittest.TestCase):
    def test_timeout_used_if_specified(self):
        timeout = timedelta(seconds=42)
        self.assertEqual(
            under_test.determine_timeout("task_name", "variant", None, timeout), timeout)

    def test_default_is_returned_with_no_timeout(self):
        self.assertEqual(
            under_test.determine_timeout("task_name", "variant"),
            under_test.DEFAULT_NON_REQUIRED_BUILD_TIMEOUT)

    def test_default_is_returned_with_timeout_at_zero(self):
        self.assertEqual(
            under_test.determine_timeout("task_name", "variant", timedelta(seconds=0)),
            under_test.DEFAULT_NON_REQUIRED_BUILD_TIMEOUT)

    def test_default_required_returned_on_required_variants(self):
        self.assertEqual(
            under_test.determine_timeout("task_name", "variant-required"),
            under_test.DEFAULT_REQUIRED_BUILD_TIMEOUT)

    def test_task_specific_timeout(self):
        self.assertEqual(
            under_test.determine_timeout("auth", "linux-64-debug"), timedelta(minutes=60))

    def test_commit_queue_items_use_commit_queue_timeout(self):
        timeout = under_test.determine_timeout("auth", "variant",
                                               evg_alias=under_test.COMMIT_QUEUE_ALIAS)
        self.assertEqual(timeout, under_test.COMMIT_QUEUE_TIMEOUT)

    def test_use_idle_timeout_if_greater_than_exec_timeout(self):
        idle_timeout = timedelta(hours=2)
        exec_timeout = timedelta(minutes=10)
        timeout = under_test.determine_timeout("task_name", "variant", idle_timeout=idle_timeout,
                                               exec_timeout=exec_timeout)

        self.assertEqual(timeout, idle_timeout)
