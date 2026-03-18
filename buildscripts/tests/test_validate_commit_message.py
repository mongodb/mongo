"""Unit tests for the evergreen_task_timeout script."""

import unittest
from unittest.mock import patch

from git import Commit, Repo

from buildscripts.validate_commit_message import (
    get_non_merge_queue_squashed_commits,
    is_valid_commit,
)


class ValidateCommitMessageTest(unittest.TestCase):
    def test_valid(self):
        fake_repo = Repo()
        messages = [
            Commit(repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="SERVER-44338"),
            Commit(repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message='Revert "SERVER-60'),
            Commit(
                repo=fake_repo,
                binsha=b"deadbeefdeadbeefdead",
                message="Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4",
            ),
            Commit(
                repo=fake_repo,
                binsha=b"deadbeefdeadbeefdead",
                message='Revert "Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4"',
            ),
            Commit(
                repo=fake_repo,
                binsha=b"deadbeefdeadbeefdead",
                message="SERVER-44338 blablablalbabla\nmultiline message\nasdfasdf",
            ),
        ]

        self.assertTrue(all(is_valid_commit(message) for message in messages))

    def test_invalid(self):
        fake_repo = Repo()
        messages = [
            Commit(
                repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="SERVER-"
            ),  # missing number
            Commit(
                repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="Revert SERVER-60"
            ),  # missing quote before SERVER
            Commit(repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message=""),  # empty value
            Commit(
                repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="nonsense"
            ),  # nonsense value
            Commit(
                repo=fake_repo,
                binsha=b"deadbeefdeadbeefdead",
                message="SERVER-123 asdf\nhttps://spruce.mongodb.com",
            ),  # Contains some banned strings
            Commit(
                repo=fake_repo,
                binsha=b"deadbeefdeadbeefdead",
                message="SERVER-123 asdf\nhttps://evergreen.mongodb.com",
            ),  # Contains some banned strings
            Commit(
                repo=fake_repo,
                binsha=b"deadbeefdeadbeefdead",
                message="SERVER-123 asdf\nAnything in this description will be included in the commit message. Replace or delete this text before merging. Add links to testing in the comments of the PR.",
            ),  # Contains some banned strings
            Commit(
                repo=fake_repo,
                binsha=b"deadbeefdeadbeefdead",
                message="SERVER-123 asdf\nAnything\n\n in this description will be included in the commit message.\nReplace or delete this text before merging. Add links to testing in the\ncomments of the PR.",
            ),  # Contains some banned strings with extra newlines
        ]

        self.assertTrue(all(not is_valid_commit(message) for message in messages))

    def test_valid_commit_with_changed_files_server_project(self):
        """Test that SERVER project can modify any file."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="SERVER-12345 Add new feature"
        )

        # SERVER project should be able to modify any file
        changed_files = [
            "src/mongo/db/query.cpp",
            "monguard/test.cpp",
            "buildscripts/test.py",
            "random/path/to/file.txt",
        ]

        self.assertTrue(is_valid_commit(commit, changed_files))

    def test_valid_commit_with_changed_files_guard_project(self):
        """Test that GUARD project can only modify files in monguard/ directory."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="GUARD-12345 Update monguard"
        )

        # GUARD project should be able to modify files in monguard/
        changed_files = [
            "monguard/test.cpp",
            "monguard/subdir/file.h",
            "monguard/deep/nested/path/file.py",
        ]

        self.assertTrue(is_valid_commit(commit, changed_files))

    def test_invalid_commit_guard_project_wrong_path(self):
        """Test that GUARD project cannot modify files outside monguard/ directory."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="GUARD-12345 Update monguard"
        )

        # GUARD project should NOT be able to modify files outside monguard/
        changed_files = [
            "monguard/test.cpp",  # This is allowed
            "src/mongo/db/query.cpp",  # This is NOT allowed
        ]

        self.assertFalse(is_valid_commit(commit, changed_files))

    def test_invalid_commit_unknown_jira_project(self):
        """Test that unknown JIRA projects are rejected."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="UNKNOWN-12345 Some change"
        )

        changed_files = ["src/mongo/db/query.cpp"]

        self.assertFalse(is_valid_commit(commit, changed_files))

    def test_github_pr_allows_alphanumeric_suffix(self):
        """Test that github_pr allows alphanumeric suffixes of 3-6 characters."""
        fake_repo = Repo()
        messages = [
            Commit(
                repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="SERVER-12345 Some change"
            ),
            Commit(
                repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="SERVER-123 Some change"
            ),
        ]

        self.assertTrue(all(is_valid_commit(m, [], requester="github_pr") for m in messages))

    def test_github_pr_rejects_short_suffix(self):
        """Test that github_pr rejects suffixes shorter than 3 characters."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="SERVER-12 Some change"
        )

        self.assertFalse(is_valid_commit(commit, [], requester="github_pr"))

    def test_github_pr_rejects_long_suffix(self):
        """Test that github_pr rejects suffixes longer than 6 characters."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="SERVER-1234567 Some change"
        )

        self.assertFalse(is_valid_commit(commit, [], requester="github_pr"))

    def test_github_pr_still_validates_project(self):
        """Test that github_pr still checks ALLOWED_JIRA_PROJECTS."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="ABC-12345 Some change"
        )

        self.assertFalse(is_valid_commit(commit, [], requester="github_pr"))

    def test_github_pr_still_validates_file_paths(self):
        """Test that github_pr still enforces file path restrictions."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="GUARD-12345 Update monguard"
        )

        changed_files = ["src/mongo/db/query.cpp"]

        self.assertFalse(is_valid_commit(commit, changed_files, requester="github_pr"))

    def test_wiredtiger_import_no_path_validation(self):
        """Test that wiredtiger imports don't trigger path validation."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo,
            binsha=b"deadbeefdeadbeefdead",
            message="Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4",
        )

        # Wiredtiger imports don't have JIRA project validation
        changed_files = ["src/third_party/wiredtiger/file.c"]

        self.assertTrue(is_valid_commit(commit, changed_files))

    def test_recursive_glob_pattern_matching(self):
        """Test that ** pattern correctly matches nested paths."""
        fake_repo = Repo()
        commit = Commit(
            repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message="GUARD-12345 Deep nested change"
        )

        # Test deeply nested paths work with **/* pattern
        changed_files = ["monguard/a/b/c/d/e/f/g/deeply/nested/file.cpp"]

        self.assertTrue(is_valid_commit(commit, changed_files))

    @patch("requests.post")
    def test_squashed_commit(self, mock_request):
        class FakeResponse:
            def json(self):
                return {
                    "data": {
                        "repository": {
                            "pullRequest": {
                                "viewerMergeHeadlineText": "SERVER-1234 Add a ton of great support (#32823)",
                                "viewerMergeBodyText": "This PR adds back support for a lot of things\nMany great things!",
                            }
                        }
                    }
                }

        mock_request.return_value = FakeResponse()
        commits = get_non_merge_queue_squashed_commits(
            github_org="fun_org", github_repo="fun_repo", pr_number=1024, github_token="fun_token"
        )
        self.assertEqual(len(commits), 1)
        self.assertEqual(
            commits[0].message,
            "SERVER-1234 Add a ton of great support (#32823)\nThis PR adds back support for a lot of things\nMany great things!",
        )
