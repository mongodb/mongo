#!/usr/bin/env python3
"""Unit tests for buildscripts/sbom/sbom_files_pr.py."""

import unittest
from unittest import mock

# sbom_files_pr.py is a script (not importable as-is at module scope because it does its work
# under `if __name__ == "__main__":`), so we exercise its top-level helper functions directly
# and re-implement the reset-onto-base-tip logic's call shape via mocked PyGithub objects.
from buildscripts.sbom import sbom_files_pr


class TestBranchResetOntoBaseTip(unittest.TestCase):
    """Verifies reset_branch_onto_base() always parents the new commit on the base branch's
    current tip (not the PR branch's own prior tip), and force-updates an existing ref rather
    than relying on a fast-forward.
    """

    def test_new_commit_parent_is_base_tip_not_stale_pr_branch_tip(self):
        repo = mock.MagicMock()
        base_branch_obj = mock.MagicMock()
        base_branch_obj.commit.sha = "BASE_TIP_SHA"
        repo.get_branch.return_value = base_branch_obj
        repo.get_git_commit.return_value = "BASE_COMMIT_OBJ"

        new_commit = mock.MagicMock()
        new_commit.sha = "NEW_COMMIT_SHA"
        repo.create_git_commit.return_value = new_commit

        # Existing (stale) ref is present, so ref.edit is exercised, not create_git_ref.
        existing_ref = mock.MagicMock()
        repo.get_git_ref.return_value = existing_ref

        sbom_files_pr.reset_branch_onto_base(
            repo,
            "the-base-branch",
            "SERVER-129061/sbom_update_the-base-branch",
            [("sbom.json", "{}")],
        )

        # The commit must be based on the base branch's tip, resolved by name, never the PR
        # branch's own (possibly stale) prior tip.
        repo.get_branch.assert_called_once_with("the-base-branch")
        repo.create_git_commit.assert_called_once_with(mock.ANY, mock.ANY, ["BASE_COMMIT_OBJ"])
        # Updating an existing ref onto a rewritten history requires force=True.
        existing_ref.edit.assert_called_once_with("NEW_COMMIT_SHA", force=True)
        repo.create_git_ref.assert_not_called()

    def test_creates_ref_when_branch_does_not_exist_yet(self):
        repo = mock.MagicMock()
        base_branch_obj = mock.MagicMock()
        base_branch_obj.commit.sha = "BASE_TIP_SHA"
        repo.get_branch.return_value = base_branch_obj
        repo.get_git_commit.return_value = "BASE_COMMIT_OBJ"

        new_commit = mock.MagicMock()
        new_commit.sha = "NEW_COMMIT_SHA"
        repo.create_git_commit.return_value = new_commit

        repo.get_git_ref.side_effect = sbom_files_pr.GithubException(404, "Not Found", None)

        sbom_files_pr.reset_branch_onto_base(
            repo,
            "the-base-branch",
            "SERVER-129061/sbom_update_the-base-branch",
            [("sbom.json", "{}")],
        )

        repo.create_git_ref.assert_called_once_with(
            ref="refs/heads/SERVER-129061/sbom_update_the-base-branch", sha="NEW_COMMIT_SHA"
        )


class TestMergeQueueDetection(unittest.TestCase):
    """Regression coverage: merge-queue gating is unaffected by the branch-reset change."""

    def test_pr_is_in_merge_queue_true(self):
        repo = mock.MagicMock()
        repo.get_git_matching_refs.return_value = [mock.MagicMock()]
        self.assertTrue(sbom_files_pr.pr_is_in_merge_queue(repo, "v7.0-staging", 123))
        repo.get_git_matching_refs.assert_called_once_with(
            "heads/gh-readonly-queue/v7.0-staging/pr-123-"
        )

    def test_pr_is_in_merge_queue_false(self):
        repo = mock.MagicMock()
        repo.get_git_matching_refs.return_value = []
        self.assertFalse(sbom_files_pr.pr_is_in_merge_queue(repo, "v7.0-staging", 123))

    def test_wait_for_merge_queue_clears_immediately(self):
        repo = mock.MagicMock()
        repo.get_git_matching_refs.return_value = []
        self.assertTrue(
            sbom_files_pr.wait_for_merge_queue(
                repo, "v7.0-staging", 123, poll_seconds=1, max_wait_seconds=5
            )
        )

    def test_wait_for_merge_queue_times_out(self):
        repo = mock.MagicMock()
        repo.get_git_matching_refs.return_value = [mock.MagicMock()]
        with mock.patch("buildscripts.sbom.sbom_files_pr.time.sleep"):
            self.assertFalse(
                sbom_files_pr.wait_for_merge_queue(
                    repo, "v7.0-staging", 123, poll_seconds=1, max_wait_seconds=2
                )
            )


class TestRequestSsdlcReview(unittest.TestCase):
    """request_ssdlc_review() must always ask for the SSDLC team's review, tolerating the
    already-requested/already-reviewed case but not other failures.
    """

    def test_requests_team_review(self):
        pull_request = mock.MagicMock()
        sbom_files_pr.request_ssdlc_review(pull_request)
        pull_request.create_review_request.assert_called_once_with(
            team_reviewers=[sbom_files_pr.SSDLC_REVIEW_TEAM]
        )

    def test_tolerates_already_requested(self):
        pull_request = mock.MagicMock()
        pull_request.create_review_request.side_effect = sbom_files_pr.GithubException(
            422, "Reviews may only be requested from collaborators.", None
        )
        # Should not raise.
        sbom_files_pr.request_ssdlc_review(pull_request)

    def test_reraises_other_errors(self):
        pull_request = mock.MagicMock()
        pull_request.create_review_request.side_effect = sbom_files_pr.GithubException(
            500, "Internal Server Error", None
        )
        with self.assertRaises(sbom_files_pr.GithubException):
            sbom_files_pr.request_ssdlc_review(pull_request)


if __name__ == "__main__":
    unittest.main()
