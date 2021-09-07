"""Unit tests for the evergreen_task_timeout script."""
import itertools
import unittest
from typing import List
from unittest.mock import MagicMock

from evergreen import EvergreenApi

import buildscripts.validate_commit_message as under_test

# pylint: disable=missing-docstring,no-self-use

INVALID_MESSAGES = [
    "",  # You must provide a message
    "RevertEVG-1",  # revert and ticket must be formatted
    "revert EVG-1",  # revert must be capitalized
    "This is not a valid message",  # message must be valid
    "Fix Lint",  # Fix lint is strict in terms of caps
]


def create_mock_evg_client(code_change_messages: List[str]) -> MagicMock:
    mock_code_change = MagicMock()
    mock_code_change.commit_messages = code_change_messages

    mock_patch = MagicMock()
    mock_patch.module_code_changes = [mock_code_change]

    mock_evg_client = MagicMock(spec_set=EvergreenApi)
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
    def test_valid_commits(self):
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
        mock_evg_api = create_mock_evg_client(interleave_new_format(messages))

        is_valid = under_test.validate_commit_messages("version_id", mock_evg_api)

        self.assertEqual(is_valid, under_test.STATUS_OK)

    def test_private(self):
        messages = ["XYZ-1"]
        mock_evg_api = create_mock_evg_client(interleave_new_format(messages))

        is_valid = under_test.validate_commit_messages("version_id", mock_evg_api)

        self.assertEqual(is_valid, under_test.STATUS_ERROR)

    def test_private_with_public(self):
        messages = [
            "Fix lint",
            "EVG-1",  # Test valid projects with various number lengths
            "SERVER-20",
            "XYZ-1"
        ]
        mock_evg_api = create_mock_evg_client(interleave_new_format(messages))

        is_valid = under_test.validate_commit_messages("version_id", mock_evg_api)

        self.assertEqual(is_valid, under_test.STATUS_ERROR)
