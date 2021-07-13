"""Unit tests for the evergreen_task_timeout script."""
import itertools
import unittest
from typing import List
from mock import MagicMock, patch

import evergreen

from buildscripts.validate_commit_message import main, STATUS_OK, STATUS_ERROR

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


def create_mock_evg_client(code_change_messages: List[str]) -> MagicMock:
    mock_code_change = MagicMock()
    mock_code_change.commit_messages = code_change_messages

    mock_patch = MagicMock()
    mock_patch.module_code_changes = [mock_code_change]

    mock_evg_client = MagicMock()
    mock_evg_client.patch_by_id.return_value = mock_patch
    return mock_evg_client


def interleave_new_format(older):
    """Create a new list containing a new and old format copy of each string."""
    newer = [
        f"Commit Queue Merge: '{old}' into 'mongodb/mongo:SERVER-45949-validate-message-format'"
        for old in older
    ]
    return list(itertools.chain(*zip(older, newer)))


class ValidateCommitMessageTest(unittest.TestCase):
    @patch.object(evergreen.RetryingEvergreenApi, "get_api")
    def test_valid_commits(self, get_api_mock):
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
            "Import tools: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4"
        ]
        api_mock = create_mock_evg_client(interleave_new_format(messages))

        get_api_mock.return_value = api_mock
        self.assertTrue(main(["fake_version"]) == STATUS_OK)

    @patch.object(evergreen.RetryingEvergreenApi, "get_api")
    def test_private(self, get_api_mock):
        messages = ["XYZ-1"]
        api_mock = create_mock_evg_client(interleave_new_format(messages))

        get_api_mock.return_value = api_mock
        self.assertTrue(main(["fake_version"]) == STATUS_ERROR)

    @patch.object(evergreen.RetryingEvergreenApi, "get_api")
    def test_private_with_public(self, get_api_mock):
        messages = [
            "Fix lint",
            "EVG-1",  # Test valid projects with various number lengths
            "SERVER-20",
            "XYZ-1"
        ]
        api_mock = create_mock_evg_client(interleave_new_format(messages))

        get_api_mock.return_value = api_mock
        self.assertTrue(main(["fake_version"]) == STATUS_ERROR)
