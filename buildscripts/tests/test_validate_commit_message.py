"""Unit tests for the evergreen_task_timeout script."""
import itertools
import unittest
from mock import MagicMock, patch

from buildscripts.validate_commit_message import main, STATUS_OK, STATUS_ERROR, GIT_SHOW_COMMAND

# pylint: disable=missing-docstring,no-self-use

INVALID_MESSAGES = [
    "",  # You must provide a message
    "RevertEVG-1",  # revert and ticket must be formatted
    "revert EVG-1",  # revert must be capitalized
    "This is not a valid message",  # message must be valid
    "Fix Lint",  # Fix lint is strict in terms of caps
]

NS = "buildscripts.validate_commit_message"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def interleave_new_format(older):
    """Create a new list containing a new and old format copy of each string."""
    newer = [
        f"Commit Queue Merge: '{old}' into 'mongodb/mongo:SERVER-45949-validate-message-format'"
        for old in older
    ]
    return list(itertools.chain(*zip(older, newer)))


class ValidateCommitMessageTest(unittest.TestCase):
    def test_valid(self):
        messages = [
            "Fix lint",
            "EVG-1",  # Test valid projects with various number lengths
            "SERVER-20",
            "WT-300",
            "SERVER-44338",
            "Revert EVG-5",
            "Revert SERVER-60",
            "Revert WT-700",
            "Revert 'SERVER-8000",
            'Revert "SERVER-90000',
            "Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4",
            "Import tools: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4",
            'Revert "Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4"',
        ]

        self.assertTrue(
            all(main([message]) == STATUS_OK for message in interleave_new_format(messages)))

    def test_private(self):
        self.assertEqual(main(["XYZ-1"]), STATUS_ERROR)

    def test_catch_all(self):
        self.assertTrue(
            all(
                main([message]) == STATUS_ERROR
                for message in interleave_new_format(INVALID_MESSAGES)))

    def test_last_git_commit_success(self):
        with patch(
                ns("subprocess.check_output"),
                return_value=bytearray('SERVER-1111 this is a test', 'utf-8')) as check_output_mock:
            self.assertEqual(main([]), 0)
            check_output_mock.assert_called_with(GIT_SHOW_COMMAND)
