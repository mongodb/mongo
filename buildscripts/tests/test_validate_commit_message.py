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
