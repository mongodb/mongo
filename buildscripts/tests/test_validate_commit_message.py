"""Unit tests for the evergreen_task_timeout script."""
import itertools
import unittest
from typing import List, Optional
from unittest.mock import MagicMock

import buildscripts.validate_commit_message as under_test
from buildscripts.client.jiraclient import JiraClient, SecurityLevel
from evergreen import EvergreenApi

INVALID_MESSAGES = [
    "",  # You must provide a message
    "RevertEVG-1",  # revert and ticket must be formatted
    "revert EVG-1",  # revert must be capitalized
    "This is not a valid message",  # message must be valid
    "Fix Lint",  # Fix lint is strict in terms of caps
]


def create_mock_code_change(code_change_messages: List[str], branch_name: Optional[str] = None):
    mock_code_change = MagicMock(
        commit_messages=code_change_messages,
        branch_name=branch_name if branch_name else "mongodb-mongo-master",
    )
    return mock_code_change


def create_mock_patch(code_change_messages: List[str], branch_name: Optional[str] = None):
    mock_code_change = create_mock_code_change(code_change_messages, branch_name)
    mock_patch = MagicMock(module_code_changes=[mock_code_change])
    return mock_patch


def create_mock_evg_client(code_change_messages: List[str],
                           branch_name: Optional[str] = None) -> MagicMock:
    mock_patch = create_mock_patch(code_change_messages, branch_name)

    mock_evg_client = MagicMock(spec_set=EvergreenApi)
    mock_evg_client.patch_by_id.return_value = mock_patch
    return mock_evg_client


def create_mock_jira_client():
    mock_jira = MagicMock(spec_set=JiraClient)
    mock_jira.get_ticket_security_level.return_value = SecurityLevel.NONE
    return mock_jira


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
        mock_jira = create_mock_jira_client()
        orchestrator = under_test.CommitMessageValidationOrchestrator(mock_evg_api, mock_jira)

        is_valid = orchestrator.validate_commit_messages("version_id")

        self.assertEqual(is_valid, under_test.STATUS_OK)

    def test_private(self):
        messages = ["XYZ-1"]
        mock_evg_api = create_mock_evg_client(interleave_new_format(messages))
        mock_jira = create_mock_jira_client()
        orchestrator = under_test.CommitMessageValidationOrchestrator(mock_evg_api, mock_jira)

        is_valid = orchestrator.validate_commit_messages("version_id")

        self.assertEqual(is_valid, under_test.STATUS_ERROR)

    def test_private_with_public(self):
        messages = [
            "Fix lint",
            "EVG-1",  # Test valid projects with various number lengths
            "SERVER-20",
            "XYZ-1",
        ]
        mock_evg_api = create_mock_evg_client(interleave_new_format(messages))
        mock_jira = create_mock_jira_client()
        orchestrator = under_test.CommitMessageValidationOrchestrator(mock_evg_api, mock_jira)

        is_valid = orchestrator.validate_commit_messages("version_id")

        self.assertEqual(is_valid, under_test.STATUS_ERROR)

    def test_internal_ticket_to_public_repo_should_fail(self):
        message = "SERVER-20"
        mock_evg_api = create_mock_evg_client(interleave_new_format([message]))
        mock_jira = create_mock_jira_client()
        mock_jira.get_ticket_security_level.return_value = SecurityLevel.MONGO_INTERNAL
        orchestrator = under_test.CommitMessageValidationOrchestrator(mock_evg_api, mock_jira)

        is_valid = orchestrator.validate_commit_messages("version_id")

        self.assertEqual(is_valid, under_test.STATUS_ERROR)

    def test_internal_ticket_to_private_repo_should_succeed(self):
        message = "SERVER-20"
        mock_evg_api = create_mock_evg_client(interleave_new_format([message]), "private-repo")
        mock_jira = create_mock_jira_client()
        mock_jira.get_ticket_security_level.return_value = SecurityLevel.MONGO_INTERNAL
        orchestrator = under_test.CommitMessageValidationOrchestrator(mock_evg_api, mock_jira)

        is_valid = orchestrator.validate_commit_messages("version_id")

        self.assertEqual(is_valid, under_test.STATUS_OK)
