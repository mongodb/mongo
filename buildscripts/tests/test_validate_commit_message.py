"""Unit tests for the evergreen_task_timeout script."""

import unittest

from buildscripts.validate_commit_message import main, STATUS_OK, STATUS_ERROR

# pylint: disable=missing-docstring,no-self-use

INVALID_MESSAGES = [
    [""],  # You must provide a message
    ["RevertEVG-1"],  # revert and ticket must be formatted
    ["revert EVG-1"],  # revert must be capitalized
    ["This is not a valid message"],  # message must be valid
    ["Fix lint plus extras is not a valid message"],  # Fix lint is strict
]


class ValidateCommitMessageTest(unittest.TestCase):
    def test_valid(self):
        messages = [
            ["Fix lint"],
            ["EVG-1"],  # Test valid projects with various number lengths
            ["SERVER-20"],
            ["WT-300"],
            ["SERVER-44338"],
            ["Revert EVG-5"],
            ["Revert SERVER-60"],
            ["Revert WT-700"],
            ["Revert 'SERVER-8000"],
            ['Revert "SERVER-90000'],
            ["Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4"],
            ["Import tools: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4"]
        ]

        self.assertTrue(all(main(message) == STATUS_OK for message in messages))

    def test_private(self):
        self.assertEqual(main(["XYZ-1"]), STATUS_ERROR)

    def test_catch_all(self):
        self.assertTrue(all(main(message) == STATUS_ERROR for message in INVALID_MESSAGES))

    def test_ignore_warnings(self):
        self.assertTrue(all(main(["-i"] + message) == STATUS_OK for message in INVALID_MESSAGES))
