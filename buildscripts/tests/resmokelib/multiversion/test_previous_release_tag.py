"""Unit tests for previous_release_tag.py.

Each test builds a small, hermetic git repository in a tempdir, then calls
``find_previous_release_tag`` against the controlled history. This avoids
depending on the surrounding repo's tag layout while still exercising real
``git``.
"""

import os
import tempfile
import unittest
from typing import Optional
from unittest import TestCase

from git import Git, Repo
from git.exc import GitCommandError
from gitdb.exc import BadName

from buildscripts.resmokelib.multiversion.previous_release_tag import find_previous_release_tag


def _commit(repo: Repo, message: str, filename: str = "f", content: Optional[str] = None) -> str:
    """Create a commit in ``repo`` and return its full SHA."""
    path = os.path.join(repo.working_dir, filename)
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(content if content is not None else message + "\n")
    repo.index.add([filename])
    repo.index.commit(message)
    return repo.head.commit.hexsha


def _init_repo(repo: str) -> Repo:
    # Make commits deterministic and not dependent on the host's git config.
    # git init -b was added in git 2.28; fall back to symbolic-ref on older versions.
    if Git().version_info >= (2, 28):
        r = Repo.init(repo, b="master")
    else:
        r = Repo.init(repo)
        r.git.symbolic_ref("HEAD", "refs/heads/master")
    with r.config_writer() as cfg:
        cfg.set_value("user", "email", "test@example.com")
        cfg.set_value("user", "name", "Test")
        cfg.set_value("commit", "gpgsign", "false")
    return r


class _RepoCase(TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = _init_repo(self._tmp.name)

    def tearDown(self) -> None:
        self.repo.close()
        self._tmp.cleanup()


class TestNoMatchingTags(_RepoCase):
    def test_returns_none(self):
        _commit(self.repo, "first")
        self.assertIsNone(find_previous_release_tag("HEAD", repo_root=self.repo.working_dir))


class TestInvalidTarget(_RepoCase):
    def test_raises_git_error(self):
        _commit(self.repo, "first")
        with self.assertRaises((GitCommandError, BadName)):
            find_previous_release_tag("does-not-exist", repo_root=self.repo.working_dir)


class TestMasterWithTag(_RepoCase):
    def test_returns_most_recent_tag_below_target(self):
        _commit(self.repo, "c1")
        self.repo.create_tag("r8.0.0")
        _commit(self.repo, "c2")
        self.repo.create_tag("r8.1.0")
        _commit(self.repo, "c3")  # target sits one commit past r8.1.0

        self.assertEqual(
            find_previous_release_tag("HEAD", repo_root=self.repo.working_dir), "r8.1.0"
        )


class TestTargetEqualsTaggedCommit(_RepoCase):
    def test_excludes_tag_pointing_at_target(self):
        _commit(self.repo, "c1")
        self.repo.create_tag("r8.0.0")
        _commit(self.repo, "c2")
        self.repo.create_tag("r8.1.0")  # HEAD is exactly r8.1.0

        # The script must return the *previous* tag, never the one at HEAD.
        self.assertEqual(
            find_previous_release_tag("HEAD", repo_root=self.repo.working_dir), "r8.0.0"
        )


class TestReleaseBranchPrefersAncestorTag(_RepoCase):
    def test_release_branch_returns_its_own_tag(self):
        # master:        c1 -> c2 (r8.0.0) -> c3 (r9.0.0) -> c4
        # release v8.0:                \-> r8.0.1 -> v8.0-tip
        # The target on v8.0 sits past r8.0.1; r9.0.0 is on master.
        _commit(self.repo, "c1")
        _commit(self.repo, "c2")
        self.repo.create_tag("r8.0.0")
        self.repo.git.checkout("-b", "v8.0")
        _commit(self.repo, "c2-release")
        self.repo.create_tag("r8.0.1")
        _commit(self.repo, "c2-release-next")

        self.repo.git.checkout("master")
        _commit(self.repo, "c3")
        self.repo.create_tag("r9.0.0")
        _commit(self.repo, "c4")

        # On v8.0, the closest fork point is the branch ancestor r8.0.1.
        self.assertEqual(
            find_previous_release_tag("v8.0", repo_root=self.repo.working_dir), "r8.0.1"
        )
        # On master, r9.0.0 sits one commit back and is the closest tag.
        self.assertEqual(
            find_previous_release_tag("HEAD", repo_root=self.repo.working_dir), "r9.0.0"
        )


class TestTagPattern(_RepoCase):
    def test_pattern_narrows_to_major_minor(self):
        # master:  c1 (r8.0.0) -> c2 (r8.1.0) -> c3 (r9.0.0) -> c4
        _commit(self.repo, "c1")
        self.repo.create_tag("r8.0.0")
        _commit(self.repo, "c2")
        self.repo.create_tag("r8.1.0")
        _commit(self.repo, "c3")
        self.repo.create_tag("r9.0.0")
        _commit(self.repo, "c4")

        # Unfiltered: r9.0.0 wins.
        self.assertEqual(
            find_previous_release_tag("HEAD", repo_root=self.repo.working_dir), "r9.0.0"
        )
        # Restrict to 8.x: r8.1.0 wins.
        self.assertEqual(
            find_previous_release_tag(
                "HEAD", repo_root=self.repo.working_dir, tag_pattern="r8.*.*"
            ),
            "r8.1.0",
        )
        # Restrict to 8.0.x: r8.0.0 wins.
        self.assertEqual(
            find_previous_release_tag(
                "HEAD", repo_root=self.repo.working_dir, tag_pattern="r8.0.*"
            ),
            "r8.0.0",
        )
        # Pattern with no matches: returns None.
        self.assertIsNone(
            find_previous_release_tag("HEAD", repo_root=self.repo.working_dir, tag_pattern="r7.*.*")
        )


class TestMultipleTagsOnSameReleaseBranch(_RepoCase):
    def test_target_past_several_tags_picks_highest_ancestor(self):
        # master: c1 -> c2 (r8.0.0)
        # v8.0:               \-> c3 (r8.0.1) -> c4 (r8.0.2) -> c5 (target)
        _commit(self.repo, "c1")
        _commit(self.repo, "c2")
        self.repo.create_tag("r8.0.0")
        self.repo.git.checkout("-b", "v8.0")
        _commit(self.repo, "c3")
        self.repo.create_tag("r8.0.1")
        _commit(self.repo, "c4")
        self.repo.create_tag("r8.0.2")
        _commit(self.repo, "c5")

        # All three tags are ancestors of target; highest-version wins and
        # the iteration short-circuits before walking back to r8.0.0.
        self.assertEqual(
            find_previous_release_tag("v8.0", repo_root=self.repo.working_dir), "r8.0.2"
        )


class TestPreReleaseVsRelease(_RepoCase):
    def test_release_outranks_prerelease_at_same_fork_point(self):
        # Both tags point at the same commit; r8.3.1 must win over r8.3.1-rc0.
        _commit(self.repo, "c1")
        sha = _commit(self.repo, "c2")
        self.repo.create_tag("r8.3.1-rc0", ref=sha)
        self.repo.create_tag("r8.3.1", ref=sha)
        _commit(self.repo, "c3")

        self.assertEqual(
            find_previous_release_tag("HEAD", repo_root=self.repo.working_dir), "r8.3.1"
        )


if __name__ == "__main__":
    unittest.main()
