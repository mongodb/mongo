"""Unit tests for todo_linter.py."""

import os
import unittest
from unittest.mock import patch

import buildscripts.todo_linter as under_test


class TestShouldIgnoreTodoLintFailure(unittest.TestCase):
    @patch.object(
        under_test,
        "get_patch_description",
        return_value="SERVER-12345 auto-revert-app[bot] follow-up",
    )
    def test_github_pr_auto_revert_patch_description_is_ignored(self, mock_get_patch_description):
        with patch.dict(
            os.environ, {"requester": "github_pr", "version_id": "version-123"}, clear=True
        ):
            self.assertTrue(under_test.should_ignore_todo_lint_failure())

        mock_get_patch_description.assert_called_once_with("version-123")

    @patch.object(
        under_test,
        "get_patch_description",
        return_value="SERVER-12345 auto-revert-app[bot] follow-up",
    )
    def test_github_pull_request_user_auto_revert_patch_description_is_ignored(
        self, mock_get_patch_description
    ):
        with patch.dict(
            os.environ, {"AUTHOR": "github_pull_request", "VERSION_ID": "version-123"}, clear=True
        ):
            self.assertTrue(under_test.should_ignore_todo_lint_failure())

        mock_get_patch_description.assert_called_once_with("version-123")

    @patch.object(
        under_test,
        "get_patch_description",
        return_value="SERVER-12345 regular pull request",
    )
    def test_non_auto_revert_patch_description_is_not_ignored(self, mock_get_patch_description):
        with patch.dict(
            os.environ, {"requester": "github_pr", "version_id": "version-123"}, clear=True
        ):
            self.assertFalse(under_test.should_ignore_todo_lint_failure())

        mock_get_patch_description.assert_called_once_with("version-123")


class TestLintFiles(unittest.TestCase):
    @patch.object(under_test.parallel, "parallel_process", return_value=False)
    @patch.object(under_test, "should_ignore_todo_lint_failure", return_value=True)
    def test_lint_files_does_not_exit_for_auto_revert_patch(
        self, _mock_should_ignore, _mock_parallel_process
    ):
        under_test._lint_files(["buildscripts/todo_linter.py"])

    @patch.object(under_test.parallel, "parallel_process", return_value=False)
    @patch.object(under_test, "should_ignore_todo_lint_failure", return_value=False)
    def test_lint_files_exits_when_exemption_does_not_apply(
        self, _mock_should_ignore, _mock_parallel_process
    ):
        with self.assertRaises(SystemExit) as context:
            under_test._lint_files(["buildscripts/todo_linter.py"])

        self.assertEqual(context.exception.code, 1)
