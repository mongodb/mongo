"""Unit tests for the evergreen_task_timeout script."""

import unittest

from buildscripts import evergreen_task_timeout as ett

# pylint: disable=missing-docstring,no-self-use


class DetermineTimeoutTest(unittest.TestCase):
    def test_timeout_used_if_specified(self):
        self.assertEqual(ett.determine_timeout("task_name", "variant", 42), 42)

    def test_default_is_returned_with_no_timeout(self):
        self.assertEqual(
            ett.determine_timeout("task_name", "variant"),
            ett.DEFAULT_NON_REQUIRED_BUILD_TIMEOUT_SECS)

    def test_default_is_returned_with_timeout_at_zero(self):
        self.assertEqual(
            ett.determine_timeout("task_name", "variant", 0),
            ett.DEFAULT_NON_REQUIRED_BUILD_TIMEOUT_SECS)

    def test_default_required_returned_on_required_variants(self):
        self.assertEqual(
            ett.determine_timeout("task_name", next(iter(ett.REQUIRED_BUILD_VARIANTS))),
            ett.DEFAULT_REQUIRED_BUILD_TIMEOUT_SECS)

    def test_task_specific_timeout(self):
        self.assertEqual(ett.determine_timeout("auth", "linux-64-debug"), 60 * 60)
