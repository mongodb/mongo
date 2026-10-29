import os
import platform
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

from git import Repo
from mock import MagicMock

from buildscripts.bazel_rules_mongo.utils import evergreen_git

changed_file_name = "changed_file.txt"
new_file_name = "new_file.txt"


def write_file(repo: Repo, file_name: str) -> None:
    # just adding more text to the file so git thinks it has changed or is created
    with open(os.path.join(repo.working_tree_dir, file_name), "a+") as file:
        file.write("change\n")


class TestFilesToLint(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp_dir.cleanup)
        original_dir = os.getcwd()
        os.chdir(self.tmp_dir.name)
        self.addCleanup(os.chdir, original_dir)
        self.repo = Repo.init(self.tmp_dir.name)
        self.addCleanup(self.repo.close)

    @unittest.skipIf(sys.platform == "win32", reason="This test requires symlink support")
    def test_includes_tracked_and_untracked_files(self) -> None:
        Path("tracked.txt").write_text("tracked\n")
        self.repo.index.add(["tracked.txt"])
        Path("tracked.txt").write_text("modified\n")
        Path("untracked.txt").write_text("untracked\n")
        Path(".gitignore").write_text("ignored.txt\n")
        Path("ignored.txt").write_text("ignored\n")
        Path("tracked-link").symlink_to("missing-target")
        self.repo.index.add(["tracked-link"])
        Path("untracked-link").symlink_to("missing-target")

        self.assertEqual(
            set(evergreen_git.get_files_to_lint()),
            {"tracked.txt", "untracked.txt", ".gitignore", "tracked-link", "untracked-link"},
        )

    def test_excludes_staged_and_unstaged_deletions(self) -> None:
        Path("remaining.txt").write_text("remaining\n")
        self.repo.index.add(["remaining.txt"])
        Path("untracked/OWNERS.yaml").parent.mkdir()
        Path("untracked/OWNERS.yaml").write_text("version: 2.0.0\n")

        for staged in (False, True):
            with self.subTest(staged=staged):
                deleted_files = ["nested/OWNERS.yaml", "nested/OWNERS.yml", "nested/file.py"]
                Path("nested").mkdir(exist_ok=True)
                for file in deleted_files:
                    Path(file).write_text("test\n")
                self.repo.index.add(deleted_files)
                self.assertTrue(set(deleted_files) <= set(evergreen_git.get_files_to_lint()))

                for file in deleted_files:
                    Path(file).unlink()
                if staged:
                    self.repo.git.add("-u", "--", "nested")

                self.assertEqual(
                    set(evergreen_git.get_files_to_lint()),
                    {"remaining.txt", "untracked/OWNERS.yaml"},
                )


@unittest.skipIf(
    sys.platform == "win32" or platform.machine().lower() in {"ppc64le", "s390x"},
    reason="This test breaks on windows and only needs to work on linux",
)
class TestChangedFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp_dir = tempfile.mkdtemp()
        root_repo = Repo()

        # commit of HEAD
        commit = root_repo.head.commit.hexsha

        files_to_copy = set()

        # copy the current repo into a temp dir to do testing on
        root_repo.git.execute(["git", "worktree", "add", cls.tmp_dir, commit])

        # get tracked files that have been changed that are tracked by git
        diff_output = root_repo.git.execute(
            ["git", "diff", "--name-only", "--diff-filter=d", commit]
        )
        files_to_copy.update(diff_output.split("\n"))

        # gets all the untracked changes in the current repo
        untracked_changes = root_repo.git.execute(["git", "add", ".", "-n"])
        for line in untracked_changes.split("\n"):
            if not line:
                continue
            files_to_copy.add(line.strip()[5:-1])

        # copy all changed files from the current repo to the new worktree for testing.
        for file in files_to_copy:
            if not file:
                continue

            if not os.path.exists(file):
                raise RuntimeError(f"Changed file was found and does not exist: {file}")

            # This means the file is an embeded git repo, this happens when other evergreen modules
            # are present, we can just ignore them
            if os.path.isdir(file):
                continue

            new_dest = os.path.join(cls.tmp_dir, file)
            os.makedirs(os.path.dirname(new_dest), exist_ok=True)
            shutil.copy(file, new_dest)

        cls.repo = Repo(cls.tmp_dir)
        cls.repo.git.execute(["git", "config", "user.email", "test@example.com"])
        cls.repo.git.execute(["git", "config", "user.name", "Test User"])
        cls.repo.git.execute(["git", "config", "commit.gpgsign", "false"])
        # add a testing file to this original commit so we can treat it as a preexisting file that
        # is going to be modified
        write_file(cls.repo, changed_file_name)
        cls.repo.git.execute(["git", "add", "."])
        cls.repo.git.execute(["git", "commit", "-m", "Commit changed files"])
        # this new commit is out base revision to compare changes against
        cls.base_revision = cls.repo.head.commit.hexsha
        cls.original_dir = os.path.abspath(os.curdir)
        os.chdir(cls.tmp_dir)

    @classmethod
    def tearDownClass(cls):
        os.chdir(cls.original_dir)
        shutil.rmtree(cls.tmp_dir)

    def setUp(self):
        # change the file already commited to the repo
        write_file(self.repo, changed_file_name)
        # make a new file that has not been commited yet
        write_file(self.repo, new_file_name)

        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", delete=False) as tmp:
            tmp.write("fake_expansion: true\n")
            self.expansions_file = tmp.name

    def tearDown(self):
        # reset to the original state between tests
        self.repo.git.execute(["git", "reset", "--hard", self.base_revision])
        os.unlink(self.expansions_file)

    def test_local_unchanged_files(self):
        evergreen_git.get_remote_branch_ref = MagicMock(return_value=self.base_revision)
        new_files = evergreen_git.get_new_files()
        self.assertEqual(
            new_files, [], msg="New files list was not empty when no new files were added to git."
        )

        changed_files = evergreen_git.get_changed_files()
        self.assertEqual(
            changed_files, [changed_file_name], msg="Changed file list was not as expected."
        )

        self.repo.git.execute(["git", "add", "."])

        # random file not tracked by git
        write_file(self.repo, "random_other_untracked_file.txt")

        new_files = evergreen_git.get_new_files()
        self.assertEqual(
            new_files,
            [new_file_name],
            msg="New file list did not contain the new file added to git.",
        )

        changed_files = evergreen_git.get_changed_files()
        self.assertEqual(
            changed_files,
            [changed_file_name, new_file_name],
            msg="Changed file list was not as expected.",
        )

    def test_evergreen_patch(self):
        # the files in evergreen patches live as uncommited files added to the index
        with open(self.expansions_file, "a") as tmp:
            tmp.write("is_patch: true\n")
            tmp.write(f"revision: {self.base_revision}\n")
        self.repo.git.execute(["git", "add", "."])
        new_files = evergreen_git.get_new_files(expansions_file=self.expansions_file)
        self.assertEqual(
            new_files, [new_file_name], msg="New file list did not contain the new file."
        )

        changed_files = evergreen_git.get_changed_files(expansions_file=self.expansions_file)
        self.assertEqual(
            changed_files,
            [changed_file_name, new_file_name],
            msg="Changed file list was not as expected.",
        )

    def test_evergreen_waterfall(self):
        # Evergreen waterfall runs just check against the last commit so we need to commit the changes
        self.repo.git.execute(["git", "add", "."])
        self.repo.git.execute(["git", "commit", "-m", "Fake waterfall changes"])
        new_files = evergreen_git.get_new_files(expansions_file=self.expansions_file)
        self.assertEqual(
            new_files, [new_file_name], msg="New file list did not contain the new file."
        )

        changed_files = evergreen_git.get_changed_files(expansions_file=self.expansions_file)
        self.assertEqual(
            changed_files,
            [changed_file_name, new_file_name],
            msg="Changed file list was not as expected.",
        )

    def test_remote_picker(self):
        remote = evergreen_git.get_mongodb_remote(self.repo)
        self.assertIn("10gen/mongo", remote.url, msg="The wrong remote was found.")

    def test_default_origin_branch(self):
        remote = evergreen_git.get_mongodb_remote(self.repo)
        self.assertEqual(
            evergreen_git.get_default_origin_branch(self.repo),
            f"{remote.name}/{evergreen_git.DEFAULT_ORIGIN_BRANCH}",
        )
