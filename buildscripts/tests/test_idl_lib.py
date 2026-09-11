"""Tests for buildscripts.idl.lib.list_idls."""

import os
import shutil
import subprocess
import tempfile
import unittest

from buildscripts.idl.lib import list_idls

IDL_CONTENT = """
global:
    jsModule: 'test_idl_lib'
serverParameters:
    testParameter:
        description: "test"
        type: int
"""


def _git(repo, *args):
    return subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True, check=True)


@unittest.skipIf(shutil.which("git") is None, "git is not available")
class TestListIdls(unittest.TestCase):
    def setUp(self):
        self.repo = tempfile.mkdtemp(prefix="list_idls_test_")
        _git(self.repo, "init", "-q")
        _git(self.repo, "config", "user.email", "test@example.com")
        _git(self.repo, "config", "user.name", "test")

    def tearDown(self):
        shutil.rmtree(self.repo, ignore_errors=True)

    def _write_idl(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as idl_file:
            idl_file.write(IDL_CONTENT)

    def test_finds_tracked_and_untracked_idls_in_git_repo(self):
        tracked = os.path.join(self.repo, "src", "tracked.idl")
        untracked = os.path.join(self.repo, "src", "untracked.idl")
        self._write_idl(tracked)
        self._write_idl(untracked)
        _git(self.repo, "add", "src/tracked.idl")

        self.assertEqual(list_idls(os.path.join(self.repo, "src")), {tracked, untracked})

    def test_walks_when_directory_is_not_in_a_git_repo(self):
        outside = tempfile.mkdtemp(prefix="list_idls_test_no_repo_")
        try:
            idl = os.path.join(outside, "mongo", "plain.idl")
            self._write_idl(idl)
            self.assertEqual(list_idls(outside), {idl})
        finally:
            shutil.rmtree(outside, ignore_errors=True)

    def test_walks_when_git_discovery_hides_the_tree(self):
        # A directory nested inside a git repo but excluded by that repo's ignore
        # rules: git ls-files --others --exclude-standard reports nothing, even
        # though IDL files exist on the file system. list_idls must fall back to
        # walking the directory instead of returning an empty set.
        nested = os.path.join(self.repo, "downloaded_install", "9.0", "hash", "src")
        idl = os.path.join(nested, "mongo", "hidden.idl")
        self._write_idl(idl)
        with open(os.path.join(self.repo, ".gitignore"), "w") as gitignore:
            gitignore.write("/downloaded_install\n")

        self.assertEqual(list_idls(nested), {idl})

    def test_omits_idls_deleted_from_the_working_tree(self):
        tracked = os.path.join(self.repo, "src", "tracked.idl")
        self._write_idl(tracked)
        _git(self.repo, "add", "src/tracked.idl")
        os.remove(tracked)

        self.assertEqual(list_idls(os.path.join(self.repo, "src")), set())


if __name__ == "__main__":
    unittest.main()
