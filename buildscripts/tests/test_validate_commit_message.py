"""Unit tests for the evergreen_task_timeout script."""

import unittest

from git import Commit, Repo

from buildscripts.validate_commit_message import is_valid_commit


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
