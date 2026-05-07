import io
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
import traceback
import unittest
from collections.abc import Sequence
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import MagicMock, call, patch

from buildscripts.copybara import generate_evergreen, sync_repo_with_copybara
from buildscripts.copybara.path_rules import render_copybara_path_rules_module_from_files

DEFAULT_COMMON_EXCLUDED_PATTERNS = [
    ".agents/**",
    ".claude/**",
    ".copybara_release_fragments/**",
    ".cursor/**",
    ".github/CODEOWNERS",
    ".github/workflows/**",
    "AGENTS.md",
    "CLAUDE.md",
    "buildscripts/copybara/**",
    "buildscripts/modules/**",
    "etc/evergreen_yml_components/**",
    "monguard/**",
    "sbom.private.json",
    "src/mongo/db/modules/**",
    "src/third_party/private/**",
]

DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES = ("**",)
DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES = tuple(DEFAULT_COMMON_EXCLUDED_PATTERNS)

REPO_COPYBARA_TEMPLATE_PATH = (
    Path(__file__).resolve().parents[2]
    / "buildscripts"
    / "copybara"
    / "copybara_path_rules.bara.sky.template"
)


def get_repo_base_copybara_config_path(root: Path) -> Path:
    return root / sync_repo_with_copybara.COPYBARA_BASE_CONFIG_PATH


def get_repo_copybara_path_rules_path(root: Path) -> Path:
    return root / sync_repo_with_copybara.COPYBARA_PATH_RULES_PATH


def get_repo_copybara_path_rules_template_path(root: Path) -> Path:
    return root / sync_repo_with_copybara.COPYBARA_PATH_RULES_TEMPLATE_PATH


def get_repo_copybara_path_rules_module_path(root: Path) -> Path:
    return root / sync_repo_with_copybara.COPYBARA_PATH_RULES_MODULE_PATH


def write_copybara_path_rules(
    path: Path,
    *,
    common_includes: Sequence[str],
    common_excludes: Sequence[str],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            {
                "common_files_to_include": list(common_includes),
                "common_files_to_exclude": list(common_excludes),
            },
            indent=2,
        )
        + "\n"
    )


def write_copybara_path_rules_module(path: Path, path_rules_path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        render_copybara_path_rules_module_from_files(
            REPO_COPYBARA_TEMPLATE_PATH,
            path_rules_path,
        )
    )


def make_source_commit(
    sha: str, subject: str | None = None
) -> sync_repo_with_copybara.SourceCommit:
    return sync_repo_with_copybara.SourceCommit(
        sha=sha,
        author="Test User <test@example.com>",
        author_date="2026-05-01T00:00:00+00:00",
        subject=subject or f"Subject for {sha}",
    )


def write_copybara_path_rules_template(path: Path, template_text: str | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if template_text is None:
        template_text = REPO_COPYBARA_TEMPLATE_PATH.read_text()
    path.write_text(template_text)


def expected_single_branch_clone_command(
    branch: str, remote_url: str, destination_dir: Path
) -> str:
    return " ".join(
        [
            "git",
            "clone",
            "--filter=blob:none",
            "--no-checkout",
            "--single-branch",
            "-b",
            sync_repo_with_copybara.shell_quote(branch),
            sync_repo_with_copybara.shell_quote(remote_url),
            sync_repo_with_copybara.shell_quote(destination_dir),
        ]
    )


def expected_copybara_config_rev_parse_fragment() -> str:
    quoted_ref = sync_repo_with_copybara.shell_quote(
        sync_repo_with_copybara.COPYBARA_CONFIG_FETCH_REF
    )
    return f"rev-parse {quoted_ref}"


class TestRunCommand(unittest.TestCase):
    def test_run_command_can_suppress_success_output(self):
        process = MagicMock()
        process.stdout = MagicMock()
        process.communicate.return_value = ("first noisy line\nsecond noisy line\n", None)
        process.returncode = 0

        stdout = io.StringIO()
        with (
            patch(
                "buildscripts.copybara.sync_repo_with_copybara.subprocess.Popen",
                return_value=process,
            ),
            redirect_stdout(stdout),
        ):
            output = sync_repo_with_copybara.run_command("git noisy", log_output=False)

        self.assertEqual(output, "first noisy line\nsecond noisy line\n")
        self.assertIn("git noisy", stdout.getvalue())
        self.assertNotIn("first noisy line", stdout.getvalue())

    def test_run_command_can_suppress_success_command_and_output(self):
        process = MagicMock()
        process.stdout = MagicMock()
        process.communicate.return_value = ("first noisy line\nsecond noisy line\n", None)
        process.returncode = 0

        stdout = io.StringIO()
        with (
            patch(
                "buildscripts.copybara.sync_repo_with_copybara.subprocess.Popen",
                return_value=process,
            ),
            redirect_stdout(stdout),
        ):
            output = sync_repo_with_copybara.run_command(
                "git noisy", log_output=False, log_command=False
            )

        self.assertEqual(output, "first noisy line\nsecond noisy line\n")
        self.assertEqual(stdout.getvalue(), "")

    def test_run_command_prints_suppressed_output_on_failure(self):
        process = MagicMock()
        process.stdout = MagicMock()
        process.communicate.return_value = ("fatal: something broke\n", None)
        process.returncode = 128

        stdout = io.StringIO()
        with (
            patch(
                "buildscripts.copybara.sync_repo_with_copybara.subprocess.Popen",
                return_value=process,
            ),
            redirect_stdout(stdout),
            self.assertRaises(subprocess.CalledProcessError) as raised,
        ):
            sync_repo_with_copybara.run_command("git noisy", log_output=False)

        self.assertIn("fatal: something broke", stdout.getvalue())
        self.assertEqual(raised.exception.output, "fatal: something broke\n")

    def test_run_command_prints_suppressed_command_on_failure(self):
        process = MagicMock()
        process.stdout = MagicMock()
        process.communicate.return_value = ("fatal: something broke\n", None)
        process.returncode = 128

        stdout = io.StringIO()
        with (
            patch(
                "buildscripts.copybara.sync_repo_with_copybara.subprocess.Popen",
                return_value=process,
            ),
            redirect_stdout(stdout),
            self.assertRaises(subprocess.CalledProcessError),
        ):
            sync_repo_with_copybara.run_command("git noisy", log_output=False, log_command=False)

        self.assertIn("git noisy", stdout.getvalue())
        self.assertIn("fatal: something broke", stdout.getvalue())

    def test_run_command_without_stderr_merge_returns_stdout_only(self):
        process = MagicMock()
        process.stdout = MagicMock()
        process.communicate.return_value = ("tracked.txt\0", "warning on stderr\n")
        process.returncode = 0

        with (
            patch(
                "buildscripts.copybara.sync_repo_with_copybara.subprocess.Popen",
                return_value=process,
            ) as mock_popen,
            redirect_stdout(io.StringIO()),
        ):
            output = sync_repo_with_copybara.run_command("git diff -z", merge_stderr=False)

        self.assertEqual(output, "tracked.txt\0")
        mock_popen.assert_called_once()
        self.assertEqual(mock_popen.call_args.kwargs["stderr"], subprocess.PIPE)


class TestSourceCommitParsing(unittest.TestCase):
    def test_parse_source_commit_log_handles_git_record_newlines(self):
        output = (
            "commit1\0Author One <one@example.com>\0"
            "2026-05-01T00:00:00+00:00\0Subject one\0\n"
            "commit2\0Author Two <two@example.com>\0"
            "2026-05-01T00:01:00+00:00\0Subject two\0\n"
        )

        self.assertEqual(
            sync_repo_with_copybara.parse_source_commit_log(output),
            [
                sync_repo_with_copybara.SourceCommit(
                    sha="commit1",
                    author="Author One <one@example.com>",
                    author_date="2026-05-01T00:00:00+00:00",
                    subject="Subject one",
                ),
                sync_repo_with_copybara.SourceCommit(
                    sha="commit2",
                    author="Author Two <two@example.com>",
                    author_date="2026-05-01T00:01:00+00:00",
                    subject="Subject two",
                ),
            ],
        )


class TestGitOriginRevIdParsing(unittest.TestCase):
    def test_extract_git_origin_rev_id_returns_none_without_trailer(self):
        self.assertIsNone(sync_repo_with_copybara.extract_git_origin_rev_id("subject\n\nbody"))

    def test_extract_git_origin_rev_id_uses_bottom_most_trailer(self):
        commit_message = textwrap.dedent(
            """\
            SERVER-82640 Upload mongod --version output to S3 during compile.
            GitOrigin-RevId: bf6eaef

            (cherry picked from commit c97e1c1)

            GitOrigin-RevId: 757225a
            """
        )

        self.assertEqual(
            sync_repo_with_copybara.extract_git_origin_rev_id(commit_message),
            "757225a",
        )


@unittest.skipIf(
    sys.platform == "win32" or sys.platform == "darwin",
    reason="No need to run this unittest on windows or macos",
)
class TestBranchFunctions(unittest.TestCase):
    @staticmethod
    def create_mock_repo_git_config(mongodb_mongo_dir, config_content):
        """
        Create a mock Git repository configuration.

        :param mongodb_mongo_dir: The directory path of the mock MongoDB repository.
        :param config_content: The content to be written into the Git configuration file.
        """
        os.makedirs(mongodb_mongo_dir, exist_ok=True)

        # Create .git directory
        git_dir = os.path.join(mongodb_mongo_dir, ".git")
        os.makedirs(git_dir, exist_ok=True)

        # Write contents to .git/config
        config_path = os.path.join(git_dir, "config")
        with open(config_path, "w") as f:
            # Write contents to .git/config
            f.write(config_content)

    @staticmethod
    def create_mock_repo_commits(repo_directory, num_commits, private_commit_hashes=None):
        """
        Create mock commits in a Git repository.

        :param repo_directory: The directory path of the Git repository where the commits will be created.
        :param num_commits: The number of commits to create.
        :param private_commit_hashes: Optional. A list of private commit hashes to be included in the commit messages.
        :return: A list of commit hashes generated for the new commits.
        """
        os.chdir(repo_directory)
        sync_repo_with_copybara.run_command("git init")
        sync_repo_with_copybara.run_command('git config --local user.email "test@example.com"')
        sync_repo_with_copybara.run_command('git config --local user.name "Test User"')
        sync_repo_with_copybara.run_command("git config --local commit.gpgsign false")
        # Used to store commit hashes
        commit_hashes = []
        for i in range(num_commits):
            with open("test.txt", "a") as f:
                f.write(str(i))

            sync_repo_with_copybara.run_command("git add test.txt")
            commit_message = f"test commit {i}"
            # If there are private commit hashes need to be added in public repo commits, include them in the commit message
            if private_commit_hashes:
                commit_message += f"\nGitOrigin-RevId: {private_commit_hashes[i]}"

            # Get the current commit hash
            sync_repo_with_copybara.run_command(f'git commit -m "{commit_message}"')
            commit_hashes.append(
                sync_repo_with_copybara.run_command('git log --pretty=format:"%H" -1')
            )
        return commit_hashes

    @staticmethod
    def create_repo_branch(repo_directory, branch_name):
        """
        Create a branch in a Git repository

        :param repo_directory: The directory path of the Git repository where the branch will be created.
        :param branch_name: The name of the new branch
        """
        os.chdir(repo_directory)
        sync_repo_with_copybara.run_command(f"git branch {branch_name}")

    @staticmethod
    def mock_search(test_name, num_commits, matched_public_commits):
        """
        Mock search function to simulate finding matching commits.

        :param test_name: The name of the test.
        :param num_commits: The number of commits in the repository.
        :param matched_public_commits: The number of commits in the public repository that match the private repository with tag 'GitOrigin-RevId'.
        :return: True if the last commit in the search result matches the last commit in the public repository, False otherwise.
        """
        with tempfile.TemporaryDirectory() as tmpdir:
            try:
                os.chdir(tmpdir)
                mock_10gen_dir = os.path.join(tmpdir, "mock_10gen")
                mock_mongodb_dir = os.path.join(tmpdir, "mock_mongodb")
                os.mkdir(mock_10gen_dir)
                os.mkdir(mock_mongodb_dir)

                os.chdir(mock_10gen_dir)
                # Create a mock private repository and get all commit hashes
                private_hashes = TestBranchFunctions.create_mock_repo_commits(
                    mock_10gen_dir, num_commits
                )

                # Create a mock public repository and pass the list of private commit hashes
                if matched_public_commits != 0:
                    public_hashes = TestBranchFunctions.create_mock_repo_commits(
                        mock_mongodb_dir, matched_public_commits, private_hashes
                    )
                else:
                    public_hashes = TestBranchFunctions.create_mock_repo_commits(
                        mock_mongodb_dir, num_commits
                    )

                os.chdir(tmpdir)
                result = sync_repo_with_copybara.find_matching_commit(
                    mock_10gen_dir, mock_mongodb_dir
                )

                # Check if the commit in the search result matches the last commit in the public repository
                if result == public_hashes[-1]:
                    return True
                else:
                    assert result is None
            except Exception as err:
                print(f"{test_name}: FAIL!\n Exception occurred: {err}\n {traceback.format_exc()}")
                return False

    def test_no_search(self):
        """Perform a test where no search is required."""
        test_name = "no_search_test"
        result = self.mock_search(test_name, 5, 5)
        self.assertTrue(result, f"{test_name}: SUCCESS!")

    def test_search(self):
        """Perform a test where searching back 5 commits to find the matching commit."""
        test_name = "search_test"
        result = self.mock_search(test_name, 10, 5)
        self.assertTrue(result, f"{test_name}: SUCCESS!")

    def test_no_commit_found(self):
        """Perform a test where no matching commit is found."""
        test_name = "no_commit_found_test"
        result = self.mock_search(test_name, 2, 0)
        self.assertIsNone(result, f"{test_name}: SUCCESS!")

    def test_duplicate_destination_origin_commits_fail_on_same_branch(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            source_dir = os.path.join(tmpdir, "source")
            destination_dir = os.path.join(tmpdir, "destination")
            os.mkdir(source_dir)
            os.mkdir(destination_dir)

            private_hashes = TestBranchFunctions.create_mock_repo_commits(source_dir, 1)
            TestBranchFunctions.create_mock_repo_commits(
                destination_dir,
                2,
                [private_hashes[0], private_hashes[0]],
            )

            with self.assertRaises(SystemExit):
                sync_repo_with_copybara.find_matching_commit(source_dir, destination_dir)

    def test_duplicate_destination_origin_commits_fail_outside_source_ref(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            source_dir = os.path.join(tmpdir, "source")
            destination_dir = os.path.join(tmpdir, "destination")
            os.mkdir(source_dir)
            os.mkdir(destination_dir)

            TestBranchFunctions.create_mock_repo_commits(source_dir, 1)
            unrelated_private_hash = "a" * 40
            TestBranchFunctions.create_mock_repo_commits(
                destination_dir,
                2,
                [unrelated_private_hash, unrelated_private_hash],
            )

            with self.assertRaises(SystemExit):
                sync_repo_with_copybara.find_matching_commit(source_dir, destination_dir)

    def test_duplicate_destination_origin_commits_fail_on_unrelated_branches(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            source_dir = os.path.join(tmpdir, "source")
            destination_dir = os.path.join(tmpdir, "destination")
            os.mkdir(source_dir)
            os.mkdir(destination_dir)

            private_hashes = TestBranchFunctions.create_mock_repo_commits(source_dir, 1)
            os.chdir(destination_dir)
            sync_repo_with_copybara.run_command("git init")
            sync_repo_with_copybara.run_command('git config --local user.email "test@example.com"')
            sync_repo_with_copybara.run_command('git config --local user.name "Test User"')
            sync_repo_with_copybara.run_command("git config --local commit.gpgsign false")

            with open("test.txt", "w") as file:
                file.write("master")
            sync_repo_with_copybara.run_command("git add test.txt")
            sync_repo_with_copybara.run_command(
                f'git commit -m "master commit\nGitOrigin-RevId: {private_hashes[0]}"'
            )

            sync_repo_with_copybara.run_command("git checkout --orphan v8.2")
            with open("test.txt", "w") as file:
                file.write("release")
            sync_repo_with_copybara.run_command("git add test.txt")
            sync_repo_with_copybara.run_command(
                f'git commit -m "release commit\nGitOrigin-RevId: {private_hashes[0]}"'
            )

            with self.assertRaises(SystemExit):
                sync_repo_with_copybara.find_matching_commit(source_dir, destination_dir)

    def test_find_matching_commit_pair_uses_bottom_most_origin_trailer(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            source_dir = os.path.join(tmpdir, "source")
            destination_dir = os.path.join(tmpdir, "destination")
            os.mkdir(source_dir)
            os.mkdir(destination_dir)

            private_hashes = TestBranchFunctions.create_mock_repo_commits(source_dir, 3)

            os.chdir(destination_dir)
            sync_repo_with_copybara.run_command("git init")
            sync_repo_with_copybara.run_command('git config --local user.email "test@example.com"')
            sync_repo_with_copybara.run_command('git config --local user.name "Test User"')
            sync_repo_with_copybara.run_command("git config --local commit.gpgsign false")

            public_hashes = []
            stale_origin = private_hashes[2]
            for i in range(2):
                with open("test.txt", "a") as file:
                    file.write(str(i))
                sync_repo_with_copybara.run_command("git add test.txt")
                commit_message = textwrap.dedent(
                    f"""\
                    public backport {i}

                    GitOrigin-RevId: {stale_origin}

                    (cherry picked from commit c97e1c1)

                    GitOrigin-RevId: {private_hashes[i]}
                    """
                ).strip()
                sync_repo_with_copybara.run_command(f"git commit -m {shlex.quote(commit_message)}")
                public_hashes.append(
                    sync_repo_with_copybara.run_command('git log --pretty=format:"%H" -1')
                )

            result = sync_repo_with_copybara.find_matching_commit_pair(source_dir, destination_dir)

            self.assertEqual(
                result,
                sync_repo_with_copybara.MatchingCommit(
                    source_commit=private_hashes[1],
                    destination_commit=public_hashes[1],
                ),
            )

    def test_find_matching_commit_pair_returns_source_and_destination(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            source_dir = os.path.join(tmpdir, "source")
            destination_dir = os.path.join(tmpdir, "destination")
            os.mkdir(source_dir)
            os.mkdir(destination_dir)

            private_hashes = TestBranchFunctions.create_mock_repo_commits(source_dir, 3)
            public_hashes = TestBranchFunctions.create_mock_repo_commits(
                destination_dir,
                2,
                [private_hashes[0], private_hashes[1]],
            )

            result = sync_repo_with_copybara.find_matching_commit_pair(source_dir, destination_dir)

            self.assertEqual(
                result,
                sync_repo_with_copybara.MatchingCommit(
                    source_commit=private_hashes[1],
                    destination_commit=public_hashes[-1],
                ),
            )

    def test_find_matching_commit_pair_searches_all_destination_branches(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            source_dir = os.path.join(tmpdir, "source")
            destination_dir = os.path.join(tmpdir, "destination")
            os.mkdir(source_dir)
            os.mkdir(destination_dir)

            private_hashes = TestBranchFunctions.create_mock_repo_commits(source_dir, 3)
            public_hashes = TestBranchFunctions.create_mock_repo_commits(
                destination_dir,
                1,
                [private_hashes[0]],
            )

            os.chdir(destination_dir)
            sync_repo_with_copybara.run_command("git checkout -b v8.2")
            with open("test.txt", "a") as file:
                file.write("release")
            sync_repo_with_copybara.run_command("git add test.txt")
            sync_repo_with_copybara.run_command(
                f'git commit -m "release branch commit\nGitOrigin-RevId: {private_hashes[1]}"'
            )
            release_public_hash = sync_repo_with_copybara.run_command(
                'git log --pretty=format:"%H" -1'
            )

            result = sync_repo_with_copybara.find_matching_commit_pair(source_dir, destination_dir)

            self.assertEqual(
                result,
                sync_repo_with_copybara.MatchingCommit(
                    source_commit=private_hashes[1],
                    destination_commit=release_public_hash,
                ),
            )
            self.assertNotEqual(result.destination_commit, public_hashes[0])

    def test_branch_exists(self):
        """Perform a test to check that the branch exists in a repository."""
        test_name = "branch_exists_test"
        branch = "v0.0"

        with tempfile.TemporaryDirectory() as tmpdir:
            remote_repo_dir = os.path.join(tmpdir, "remote_repo")
            os.mkdir(remote_repo_dir)
            self.create_mock_repo_commits(remote_repo_dir, 1)
            self.create_repo_branch(remote_repo_dir, branch)

            copybara_config = sync_repo_with_copybara.CopybaraConfig(
                source=None,
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=remote_repo_dir,
                    branch=branch,
                ),
            )
            result = sync_repo_with_copybara.check_destination_branch_exists(copybara_config)
            self.assertTrue(result, f"{test_name}: SUCCESS!")

    def test_branch_not_exists(self):
        """Perform a test to check that the branch does not exist in a repository."""
        test_name = "branch_not_exists_test"
        branch = "..invalid-therefore-impossible-to-create-branch-name"

        with tempfile.TemporaryDirectory() as tmpdir:
            remote_repo_dir = os.path.join(tmpdir, "remote_repo")
            os.mkdir(remote_repo_dir)
            self.create_mock_repo_commits(remote_repo_dir, 1)

            copybara_config = sync_repo_with_copybara.CopybaraConfig(
                source=None,
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=remote_repo_dir,
                    branch=branch,
                ),
            )
            result = sync_repo_with_copybara.check_destination_branch_exists(copybara_config)
            self.assertFalse(result, f"{test_name}: SUCCESS!")

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_branch_exists_remote_requires_exact_branch_match(self, mock_run_command):
        mock_run_command.return_value = "\n".join(
            [
                "08995ea824ba2492ba1e496a2fb58e80ea2d22c3\trefs/heads/markbenvenuto/master",
                "b4dbdfd07f20ec4e0f4873bff4059073c9da62c4\trefs/heads/sql/master",
            ]
        )

        self.assertFalse(
            sync_repo_with_copybara.branch_exists_remote("https://example.com/source.git", "master")
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_get_remote_branch_head_prefers_exact_branch_match(self, mock_run_command):
        mock_run_command.return_value = "\n".join(
            [
                "08995ea824ba2492ba1e496a2fb58e80ea2d22c3\trefs/heads/markbenvenuto/master",
                "78efcf74c13378efe35e4e49fdf7cf0c9206af56\trefs/heads/master",
                "b4dbdfd07f20ec4e0f4873bff4059073c9da62c4\trefs/heads/sql/master",
            ]
        )

        self.assertEqual(
            sync_repo_with_copybara.get_remote_branch_head(
                "https://example.com/source.git", "master"
            ),
            "78efcf74c13378efe35e4e49fdf7cf0c9206af56",
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.time.sleep")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_get_remote_branch_head_retries_transient_git_remote_failure(
        self, mock_run_command, mock_sleep
    ):
        mock_run_command.side_effect = [
            subprocess.CalledProcessError(
                128,
                "git ls-remote --heads https://example.com/source.git master",
                output="fatal: repository not found",
            ),
            "78efcf74c13378efe35e4e49fdf7cf0c9206af56\trefs/heads/master",
        ]

        self.assertEqual(
            sync_repo_with_copybara.get_remote_branch_head(
                "https://example.com/source.git", "master"
            ),
            "78efcf74c13378efe35e4e49fdf7cf0c9206af56",
        )
        self.assertEqual(mock_run_command.call_count, 2)
        mock_sleep.assert_called_once_with(
            sync_repo_with_copybara.GIT_REMOTE_COMMAND_RETRY_DELAY_SECONDS
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_check_destination_branch_exists_requires_exact_branch_match(self, mock_run_command):
        mock_run_command.return_value = "\n".join(
            [
                "08995ea824ba2492ba1e496a2fb58e80ea2d22c3\trefs/heads/markbenvenuto/master",
                "b4dbdfd07f20ec4e0f4873bff4059073c9da62c4\trefs/heads/sql/master",
            ]
        )

        self.assertFalse(
            sync_repo_with_copybara.check_destination_branch_exists(
                sync_repo_with_copybara.CopybaraConfig(
                    destination=sync_repo_with_copybara.CopybaraRepoConfig(
                        git_url="https://example.com/destination.git",
                        branch="master",
                    )
                )
            )
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_check_destination_branch_exists_accepts_exact_branch_match(self, mock_run_command):
        mock_run_command.return_value = "\n".join(
            [
                "78efcf74c13378efe35e4e49fdf7cf0c9206af56\trefs/heads/master",
                "b4dbdfd07f20ec4e0f4873bff4059073c9da62c4\trefs/heads/sql/master",
            ]
        )

        self.assertTrue(
            sync_repo_with_copybara.check_destination_branch_exists(
                sync_repo_with_copybara.CopybaraConfig(
                    destination=sync_repo_with_copybara.CopybaraRepoConfig(
                        git_url="https://example.com/destination.git",
                        branch="master",
                    )
                )
            )
        )

    def test_only_mongodb_mongo_repo(self):
        """Perform a test that the repository is only the MongoDB official repository."""
        test_name = "only_mongodb_mongo_repo_test"

        # Define the content for the Git configuration file
        config_content = "blalla\n"
        config_content += "url = git@github.com:mongodb/mongo.git "

        with tempfile.TemporaryDirectory() as tmpdir:
            mongodb_mongo_dir = os.path.join(tmpdir, "mock_mongodb_mongo_repo")
            # Create Git configuration file
            self.create_mock_repo_git_config(mongodb_mongo_dir, config_content)
            os.chdir(mongodb_mongo_dir)

            try:
                # Check if the repository is only the MongoDB official repository
                result = sync_repo_with_copybara.has_only_destination_repo_remote("mongodb/mongo")
            except Exception as err:
                print(f"{test_name}: FAIL!\n Exception occurred: {err}\n {traceback.format_exc()}")
                self.fail(f"{test_name}: FAIL!")
                return

            self.assertTrue(result, f"{test_name}: SUCCESS!")

    def test_not_only_mongodb_mongo_repo(self):
        """Perform a test that the repository is not only the MongoDB official repository."""
        test_name = "not_only_mongodb_mongo_repo_test"

        # Define the content for the Git configuration file
        config_content = "blalla\n"
        config_content += "url = git@github.com:mongodb/mongo.git "
        config_content += "url = git@github.com:10gen/mongo.git "

        with tempfile.TemporaryDirectory() as tmpdir:
            mongodb_mongo_dir = os.path.join(tmpdir, "mock_mongodb_mongo_repo")

            # Create Git configuration file with provided content
            self.create_mock_repo_git_config(mongodb_mongo_dir, config_content)

            try:
                # Call function to push branch to public repository, expecting an exception
                sync_repo_with_copybara.push_branch_to_destination_repo(
                    mongodb_mongo_dir,
                    copybara_config=sync_repo_with_copybara.CopybaraConfig(
                        source=sync_repo_with_copybara.CopybaraRepoConfig(
                            git_url="",
                            repo_name="",
                            branch="",
                        ),
                        destination=sync_repo_with_copybara.CopybaraRepoConfig(
                            git_url="",
                            repo_name="",
                            branch="",
                        ),
                    ),
                    branching_off_commit="",
                )
            except Exception as err:
                if (
                    str(err)
                    == f"{mongodb_mongo_dir} git repo has not only the destination repo remote"
                ):
                    return

            self.fail(f"{test_name}: FAIL!")

    def test_new_branch_commits_not_match_branching_off_commit(self):
        """Perform a test that the new branch commits do not match the branching off commit."""
        test_name = "new_branch_commits_not_match_branching_off_commit_test"

        # Define the content for the Git configuration file
        config_content = "blalla\n"
        config_content += "url = git@github.com:mongodb/mongo.git "

        # Define a invalid branching off commit
        invalid_branching_off_commit = "123456789"

        with tempfile.TemporaryDirectory() as tmpdir:
            mongodb_mongo_dir = os.path.join(tmpdir, "mock_mongodb_mongo_repo")

            # Create Git configuration file with provided content
            self.create_mock_repo_git_config(mongodb_mongo_dir, config_content)
            os.chdir(mongodb_mongo_dir)

            # Create some mock commits in the repository
            self.create_mock_repo_commits(mongodb_mongo_dir, 2)
            try:
                # Call function to push branch to public repository, expecting an exception
                sync_repo_with_copybara.push_branch_to_destination_repo(
                    mongodb_mongo_dir,
                    sync_repo_with_copybara.CopybaraConfig(
                        source=sync_repo_with_copybara.CopybaraRepoConfig(
                            git_url="",
                            repo_name="",
                            branch="",
                        ),
                        destination=sync_repo_with_copybara.CopybaraRepoConfig(
                            git_url="",
                            repo_name="",
                            branch="",
                        ),
                    ),
                    invalid_branching_off_commit,
                )
            except Exception as err:
                if (
                    str(err)
                    == "The new branch top commit does not match the branching_off_commit. Aborting push."
                ):
                    return

            self.fail(f"{test_name}: FAIL!")


class TestReleaseTagHelpers(unittest.TestCase):
    def test_parse_release_tag_request_maps_public_branch(self):
        self.assertEqual(
            sync_repo_with_copybara.parse_release_tag_request("r8.2.7"),
            sync_repo_with_copybara.ReleaseTagRequest(
                release_tag="r8.2.7",
                public_branch="v8.2.7",
            ),
        )

    def test_parse_release_tag_request_preserves_suffix_in_public_branch(self):
        self.assertEqual(
            sync_repo_with_copybara.parse_release_tag_request("r8.2.7-hotfix"),
            sync_repo_with_copybara.ReleaseTagRequest(
                release_tag="r8.2.7-hotfix",
                public_branch="v8.2.7-hotfix",
            ),
        )

    def test_parse_release_tag_request_rejects_invalid_format(self):
        with self.assertRaises(SystemExit):
            sync_repo_with_copybara.parse_release_tag_request("r8.2")

    def test_prepared_copybara_workflow_name_prefers_release_tag(self):
        self.assertEqual(
            sync_repo_with_copybara.get_prepared_copybara_workflow_name(
                "prod", "v8.2.7-hotfix", "r8.2.7-hotfix"
            ),
            "prod_r8.2.7-hotfix",
        )

    def test_prepared_copybara_workflow_name_uses_branch_without_release_tag(self):
        self.assertEqual(
            sync_repo_with_copybara.get_prepared_copybara_workflow_name("prod", "v8.2"),
            "prod_v8.2",
        )

    def test_parse_remote_tag_commit_prefers_exact_peeled_match(self):
        output = "\n".join(
            [
                "1111111111111111111111111111111111111111\trefs/tags/r8.2.70",
                "2222222222222222222222222222222222222222\trefs/tags/r8.2.7",
                "3333333333333333333333333333333333333333\trefs/tags/r8.2.7^{}",
            ]
        )

        self.assertEqual(
            sync_repo_with_copybara.parse_remote_tag_commit(output, "r8.2.7"),
            "3333333333333333333333333333333333333333",
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_tag_exists_remote_requires_exact_tag_match(self, mock_run_command):
        mock_run_command.return_value = (
            "1111111111111111111111111111111111111111\trefs/tags/r8.2.70\n"
            "2222222222222222222222222222222222222222\trefs/tags/r8.2.7-hotfix"
        )

        self.assertFalse(
            sync_repo_with_copybara.tag_exists_remote("https://example.com/public.git", "r8.2.7")
        )

    def test_resolve_requested_release_tag_branches_creates_synthetic_fragment(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            branch_to_fragment = {"master": Path("master.sky")}

            requested_branches, release_requests = (
                sync_repo_with_copybara.resolve_requested_release_tag_branches(
                    requested_branches="master, r8.2.7",
                    branch_to_fragment=branch_to_fragment,
                    bundle_dir=Path(tmpdir),
                )
            )

            self.assertEqual(requested_branches, "master,v8.2.7")
            self.assertEqual(
                release_requests,
                {
                    "v8.2.7": sync_repo_with_copybara.ReleaseTagRequest(
                        release_tag="r8.2.7",
                        public_branch="v8.2.7",
                    )
                },
            )
            synthetic_fragment = branch_to_fragment["v8.2.7"]
            self.assertTrue(synthetic_fragment.is_file())
            self.assertIn('sync_tag("r8.2.7")', synthetic_fragment.read_text())

    def test_resolve_requested_release_tag_branches_preserves_suffix(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            branch_to_fragment = {"master": Path("master.sky")}

            requested_branches, release_requests = (
                sync_repo_with_copybara.resolve_requested_release_tag_branches(
                    requested_branches="r8.2.7-hotfix",
                    branch_to_fragment=branch_to_fragment,
                    bundle_dir=Path(tmpdir),
                )
            )

            self.assertEqual(requested_branches, "v8.2.7-hotfix")
            self.assertEqual(
                release_requests,
                {
                    "v8.2.7-hotfix": sync_repo_with_copybara.ReleaseTagRequest(
                        release_tag="r8.2.7-hotfix",
                        public_branch="v8.2.7-hotfix",
                    )
                },
            )
            self.assertIn(
                'sync_tag("r8.2.7-hotfix")',
                branch_to_fragment["v8.2.7-hotfix"].read_text(),
            )

    def test_add_test_sync_tag_request_creates_version_scoped_test_tag_fragment(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            branch_to_fragment = {"master": Path("master.sky")}

            release_request = sync_repo_with_copybara.add_test_sync_tag_request(
                branch_to_fragment=branch_to_fragment,
                bundle_dir=Path(tmpdir),
                test_branch_prefix="copybara_test_branch_patch123",
            )

            self.assertEqual(release_request.release_tag, "r0.0.0-copybara-test-tag-patch123")
            self.assertEqual(release_request.public_branch, "v0.0.0-copybara-test-tag-patch123")
            synthetic_fragment = branch_to_fragment[release_request.public_branch]
            self.assertTrue(synthetic_fragment.is_file())
            self.assertIn(
                'sync_tag("r0.0.0-copybara-test-tag-patch123")',
                synthetic_fragment.read_text(),
            )

    def test_extract_release_tags_from_fragment_reads_sync_tag(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fragment_path = Path(tmpdir) / "release_tag.sky"
            fragment_path.write_text('sync_tag("r8.2.7-hotfix")\n')

            self.assertEqual(
                sync_repo_with_copybara.extract_release_tags_from_fragment(fragment_path),
                ["r8.2.7-hotfix"],
            )

    def test_extract_branch_calls_from_fragment_reads_evergreen_activate(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fragment_path = Path(tmpdir) / "branch.sky"
            fragment_path.write_text(
                'sync_branch("master")\nsync_branch("v8.2", evergreen_activate = True)\n'
            )

            self.assertEqual(
                sync_repo_with_copybara.extract_branch_calls_from_fragment(fragment_path),
                [
                    sync_repo_with_copybara.CopybaraFragmentCall("master"),
                    sync_repo_with_copybara.CopybaraFragmentCall("v8.2", evergreen_activate=True),
                ],
            )

    def test_extract_release_tag_calls_from_fragment_reads_evergreen_activate(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fragment_path = Path(tmpdir) / "release_tag.sky"
            fragment_path.write_text(
                'sync_tag("r8.2.7")\nsync_tag("r8.2.7-hotfix", evergreen_activate = True)\n'
            )

            self.assertEqual(
                sync_repo_with_copybara.extract_release_tag_calls_from_fragment(fragment_path),
                [
                    sync_repo_with_copybara.CopybaraFragmentCall("r8.2.7"),
                    sync_repo_with_copybara.CopybaraFragmentCall(
                        "r8.2.7-hotfix", evergreen_activate=True
                    ),
                ],
            )

    def test_extract_calls_reject_invalid_evergreen_activate(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fragment_path = Path(tmpdir) / "branch.sky"
            fragment_path.write_text('sync_branch("master", evergreen_activate = true)\n')

            with self.assertRaises(SystemExit):
                sync_repo_with_copybara.extract_branch_calls_from_fragment(fragment_path)

    def test_extract_branches_from_fragment_rejects_release_tag_as_branch(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fragment_path = Path(tmpdir) / "bad_branch.sky"
            fragment_path.write_text('sync_branch("r8.2.7")\n')

            with self.assertRaises(SystemExit):
                sync_repo_with_copybara.extract_branches_from_fragment(fragment_path)

    def test_extract_release_tags_from_fragment_rejects_branch_as_release_tag(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fragment_path = Path(tmpdir) / "bad_tag.sky"
            fragment_path.write_text('sync_tag("v8.2")\n')

            with self.assertRaises(SystemExit):
                sync_repo_with_copybara.extract_release_tags_from_fragment(fragment_path)


def write_base_copybara_config(
    path: Path,
    common_patterns: list[str] | None = None,
    common_includes: list[str] | None = None,
    source_url: str = sync_repo_with_copybara.SOURCE_REPO_URL,
    prod_url: str = sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
    test_url: str = sync_repo_with_copybara.TEST_REPO_URL,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if common_patterns is None:
        common_patterns = list(DEFAULT_COMMON_EXCLUDED_PATTERNS)
    if common_includes is None:
        common_includes = ["**"]

    include_entries = "\n".join(f'    "{pattern}",' for pattern in common_includes)
    exclude_entries = "\n".join(f'    "{pattern}",' for pattern in common_patterns)
    path.write_text(
        f'source_url = "{source_url}"\n'
        f'prod_url = "{prod_url}"\n'
        f'test_url = "{test_url}"\n'
        f'test_branch_prefix = "{sync_repo_with_copybara.DEFAULT_TEST_BRANCH_PREFIX}"\n'
        f'test_workflow_base_branch = ""\n'
        f'test_workflow_source_branch = ""\n'
        f"source_refs = {{}}\n"
        f"\n"
        f"common_files_to_include = [\n"
        f"{include_entries}\n"
        f"]\n"
        f"\n"
        f"common_files_to_exclude = [\n"
        f"{exclude_entries}\n"
        f"]\n"
        f"\n"
        f"def make_workflow(\n"
        f"    workflow_name,\n"
        f"    destination_url,\n"
        f"    source_ref,\n"
        f"    destination_ref,\n"
        f"):\n"
        f"    pass\n"
        f"\n"
        f"def sync_branch(branch_name, evergreen_activate = False):\n"
        f"    source_ref = source_refs.get(branch_name, branch_name)\n"
        f'    make_workflow("prod_" + branch_name, prod_url, source_ref, branch_name)\n'
        f"    make_workflow(\n"
        f'        "test_" + branch_name,\n'
        f"        test_url,\n"
        f"        source_ref,\n"
        f'        test_branch_prefix + "_" + branch_name,\n'
        f"    )\n"
    )


class TestSkyExclusionChecks(unittest.TestCase):
    def test_extract_sky_excluded_patterns(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)

            patterns = sync_repo_with_copybara.extract_sky_excluded_patterns(str(sky_path))

            self.assertIn("src/mongo/db/modules/**", patterns)
            self.assertIn("AGENTS.md", patterns)

    def test_extract_sky_excluded_patterns_prefers_adjacent_path_rules(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            sky_path.write_text('common_files_to_exclude = ["ignored/**"]\n')
            write_copybara_path_rules(
                sky_path.with_name("copybara_path_rules.json"),
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=["src/authoritative/**"],
            )

            patterns = sync_repo_with_copybara.extract_sky_excluded_patterns(str(sky_path))

            self.assertEqual(patterns, {"src/authoritative/**"})

    def test_get_preview_excluded_patterns_includes_common_exclusions(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                sky_path,
                common_patterns=DEFAULT_COMMON_EXCLUDED_PATTERNS + ["docs/private-notes/**"],
            )
            sky_path.write_text(sky_path.read_text() + '\nsync_branch("master")\n')

            patterns = sync_repo_with_copybara.get_preview_excluded_patterns(
                str(sky_path), "master"
            )

            self.assertIn("docs/private-notes/**", patterns)
            self.assertIn("AGENTS.md", patterns)

    def test_get_preview_excluded_patterns_includes_branch_specific_additions(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)
            sky_path.write_text(
                sky_path.read_text()
                + '\nrelease_files_to_exclude = [\n    "docs/private-notes/**",\n]\n'
                + 'sync_branch("v8.2", release_files_to_exclude)\n'
            )

            patterns = sync_repo_with_copybara.get_preview_excluded_patterns(str(sky_path), "v8.2")

            self.assertIn("docs/private-notes/**", patterns)
            self.assertIn("AGENTS.md", patterns)

    def test_get_preview_excluded_patterns_ignores_evergreen_activate(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)
            sky_path.write_text(
                sky_path.read_text()
                + '\nrelease_files_to_exclude = [\n    "docs/private-notes/**",\n]\n'
                + 'sync_branch("v8.2", release_files_to_exclude, evergreen_activate = True)\n'
            )

            patterns = sync_repo_with_copybara.get_preview_excluded_patterns(str(sky_path), "v8.2")

            self.assertIn("docs/private-notes/**", patterns)
            self.assertIn("AGENTS.md", patterns)

    def test_get_preview_excluded_patterns_deduplicates_common_and_branch_exclusions(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                sky_path,
                common_patterns=["AGENTS.md", "internal/"],
            )
            sky_path.write_text(
                sky_path.read_text()
                + '\nrelease_files_to_exclude = [\n    "internal/",\n    "private/**",\n]\n'
                + 'sync_branch("v8.2", release_files_to_exclude)\n'
            )

            patterns = sync_repo_with_copybara.get_preview_excluded_patterns(str(sky_path), "v8.2")

            self.assertEqual(patterns, ["AGENTS.md", "internal/", "private/**"])

    def test_extract_branch_public_patterns_defaults_to_all_files(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)
            sky_path.write_text(sky_path.read_text() + '\nsync_branch("master")\n')

            patterns = sync_repo_with_copybara.extract_branch_public_patterns(
                str(sky_path), "master"
            )

            self.assertEqual(patterns, {"**"})

    def test_extract_branch_public_patterns_from_common_list(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                sky_path,
                common_includes=["README.md", "docs/**"],
            )
            sky_path.write_text(sky_path.read_text() + '\nsync_branch("master")\n')

            patterns = sync_repo_with_copybara.extract_branch_public_patterns(
                str(sky_path), "master"
            )

            self.assertEqual(patterns, {"README.md", "docs/**"})

    def test_extract_branch_public_patterns_supports_sync_tag(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                sky_path,
                common_includes=["README.md", "docs/**"],
            )
            sky_path.write_text(sky_path.read_text() + '\nsync_tag("r8.2.7")\n')

            patterns = sync_repo_with_copybara.extract_branch_public_patterns(
                str(sky_path), "v8.2.7"
            )

            self.assertEqual(patterns, {"README.md", "docs/**"})

    def test_extract_branch_public_patterns_ignores_branch_exclusions_only(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                sky_path,
                common_patterns=["AGENTS.md", "internal/**"],
                common_includes=["src/", "buildscripts/", "jstests/**"],
            )
            sky_path.write_text(
                sky_path.read_text()
                + '\nrelease_exclusions = [\n    "private/**",\n    "secrets/**",\n]\n'
                + 'sync_branch("v8.2", release_exclusions)\n'
            )

            patterns = sync_repo_with_copybara.extract_branch_public_patterns(str(sky_path), "v8.2")

            self.assertEqual(patterns, {"src/", "buildscripts/", "jstests/**"})

    def test_extract_branch_public_patterns_prefers_adjacent_path_rules(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            sky_path.write_text(
                'common_files_to_include = ["ignored/**"]\n' + '\nsync_branch("master")\n'
            )
            write_copybara_path_rules(
                sky_path.with_name("copybara_path_rules.json"),
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
                common_includes=["README.md", "docs/**"],
            )

            patterns = sync_repo_with_copybara.extract_branch_public_patterns(
                str(sky_path), "master"
            )

            self.assertEqual(patterns, {"README.md", "docs/**"})

    def test_extract_branch_public_patterns_from_named_list(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)
            sky_path.write_text(
                sky_path.read_text()
                + '\nmaster_public_files = [\n    "README.md",\n    "docs/**",\n]\n'
                + 'sync_branch("master", [], master_public_files, evergreen_activate = True)\n'
            )

            patterns = sync_repo_with_copybara.extract_branch_public_patterns(
                str(sky_path), "master"
            )

            self.assertEqual(patterns, {"README.md", "docs/**"})

    def test_check_branch_top_level_paths_are_labeled_passes(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                sky_path,
                common_includes=["README.md", "docs/**"],
            )
            sky_path.write_text(sky_path.read_text() + '\nsync_branch("master")\n')

            sync_repo_with_copybara.check_branch_top_level_paths_are_labeled(
                str(sky_path),
                "master",
                {"README.md", "docs", "monguard", "sbom.private.json"},
            )

    def test_check_branch_top_level_paths_are_labeled_supports_sync_tag(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                sky_path,
                common_includes=["README.md", "docs/**"],
            )
            sky_path.write_text(sky_path.read_text() + '\nsync_tag("r8.2.7")\n')

            sync_repo_with_copybara.check_branch_top_level_paths_are_labeled(
                str(sky_path),
                "v8.2.7",
                {"README.md", "docs", "monguard", "sbom.private.json"},
            )

    def test_check_branch_top_level_paths_are_labeled_fails_for_unlabeled_path(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                sky_path,
                common_includes=["README.md", "docs/**"],
            )
            sky_path.write_text(sky_path.read_text() + '\nsync_branch("master")\n')

            with self.assertRaises(SystemExit):
                sync_repo_with_copybara.check_branch_top_level_paths_are_labeled(
                    str(sky_path),
                    "master",
                    {"README.md", "docs", "src"},
                )


class TestCopybaraConfigHelpers(unittest.TestCase):
    def test_build_copybara_config_uses_test_branch_prefix(self):
        config = sync_repo_with_copybara.build_copybara_config(
            workflow="test",
            branch="v8.2",
            source_ref="deadbeef123",
            test_branch_prefix="copybara_test_branch_patch123",
        )

        self.assertEqual(config.source.branch, "v8.2")
        self.assertEqual(config.source.ref, "deadbeef123")
        self.assertEqual(config.destination.branch, "copybara_test_branch_patch123_v8.2")
        self.assertEqual(config.destination.repo_name, sync_repo_with_copybara.TEST_REPO_NAME)

    @patch("buildscripts.copybara.sync_repo_with_copybara.get_installation_access_token")
    def test_get_copybara_tokens_uses_master_sync_public_app_expansion(
        self, mock_get_installation_access_token
    ):
        expansions = {
            "app_id_copybara_syncer_master_sync": "101",
            "private_key_copybara_syncer": "public-private-key",
            "installation_id_copybara_syncer": "202",
            "app_id_copybara_syncer_10gen": "303",
            "private_key_copybara_syncer_10gen": "private-private-key",
            "installation_id_copybara_syncer_10gen": "404",
        }
        mock_get_installation_access_token.side_effect = ["public-token", "private-token"]

        tokens = sync_repo_with_copybara.get_copybara_tokens(expansions)

        self.assertEqual(
            mock_get_installation_access_token.call_args_list,
            [
                call(101, "public-private-key", 202),
                call(303, "private-private-key", 404),
            ],
        )
        self.assertEqual(
            tokens,
            {
                sync_repo_with_copybara.SOURCE_REPO_URL: "private-token",
                sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "public-token",
                sync_repo_with_copybara.TEST_REPO_URL: "private-token",
            },
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_list_copybara_fragment_paths_excludes_base_config(self, mock_run_command):
        mock_run_command.return_value = "\n".join(
            [
                sync_repo_with_copybara.COPYBARA_BASE_CONFIG_PATH.as_posix(),
                sync_repo_with_copybara.COPYBARA_PATH_RULES_MODULE_PATH.as_posix(),
                "buildscripts/copybara/master.sky",
                "buildscripts/copybara/v8_2.sky",
            ]
        )

        fragment_paths = sync_repo_with_copybara.list_copybara_fragment_paths(
            Path("/tmp/fetch"),
            sync_repo_with_copybara.COPYBARA_CONFIG_FETCH_REF,
        )

        self.assertEqual(
            fragment_paths,
            ["buildscripts/copybara/master.sky", "buildscripts/copybara/v8_2.sky"],
        )

    def test_discover_copybara_branches_reads_fragments(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            write_base_copybara_config(get_repo_base_copybara_config_path(root))
            write_copybara_path_rules(
                get_repo_copybara_path_rules_path(root),
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
            )
            write_copybara_path_rules_module(
                get_repo_copybara_path_rules_module_path(root),
                get_repo_copybara_path_rules_path(root),
            )
            fragment_dir = root / "buildscripts" / "copybara"
            fragment_dir.mkdir(parents=True, exist_ok=True)
            (fragment_dir / "master.sky").write_text('sync_branch("master")\n')
            (fragment_dir / "v8_2.sky").write_text(
                'sync_branch("v8.2")\nsync_branch("v8.2.6-hotfix")\n'
            )
            (fragment_dir / "v8_2_7_tag.sky").write_text('sync_tag("r8.2.7")\n')

            branch_to_fragment = sync_repo_with_copybara.discover_copybara_branches(tmpdir)

            self.assertEqual(branch_to_fragment["master"], fragment_dir / "master.sky")
            self.assertEqual(branch_to_fragment["v8.2"], fragment_dir / "v8_2.sky")
            self.assertEqual(branch_to_fragment["v8.2.6-hotfix"], fragment_dir / "v8_2.sky")
            self.assertNotIn("v8.2.7", branch_to_fragment)

    def test_discover_copybara_branches_skips_disabled_fragments(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            write_base_copybara_config(get_repo_base_copybara_config_path(root))
            write_copybara_path_rules(
                get_repo_copybara_path_rules_path(root),
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
            )
            write_copybara_path_rules_module(
                get_repo_copybara_path_rules_module_path(root),
                get_repo_copybara_path_rules_path(root),
            )
            fragment_dir = root / "buildscripts" / "copybara"
            fragment_dir.mkdir(parents=True, exist_ok=True)
            (fragment_dir / "master.sky").write_text('sync_branch("master")\n')
            (fragment_dir / "v8_0.sky").write_text(
                '"""Copybara branch definitions for v8.0."""\n\n#sync_branch("v8.0")\n'
            )

            branch_to_fragment = sync_repo_with_copybara.discover_copybara_branches(tmpdir)

            self.assertEqual(branch_to_fragment, {"master": fragment_dir / "master.sky"})

    def test_discover_copybara_branches_ignores_fragments_without_sync_call(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            write_base_copybara_config(get_repo_base_copybara_config_path(root))
            write_copybara_path_rules(
                get_repo_copybara_path_rules_path(root),
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
            )
            write_copybara_path_rules_module(
                get_repo_copybara_path_rules_module_path(root),
                get_repo_copybara_path_rules_path(root),
            )
            fragment_dir = root / "buildscripts" / "copybara"
            fragment_dir.mkdir(parents=True, exist_ok=True)
            (fragment_dir / "master.sky").write_text('sync_branch("master")\n')
            (fragment_dir / "typo.sky").write_text('sync_brach("v8.0")\n')

            branch_to_fragment = sync_repo_with_copybara.discover_copybara_branches(tmpdir)

            self.assertEqual(branch_to_fragment, {"master": fragment_dir / "master.sky"})

    def test_resolve_requested_branches_preserves_user_order(self):
        branch_to_fragment = {
            "master": Path("master.sky"),
            "v8.0": Path("v8_0.sky"),
            "v8.2": Path("v8_2.sky"),
        }

        selected = sync_repo_with_copybara.resolve_requested_branches(
            "v8.2, master, v8.2", branch_to_fragment
        )

        self.assertEqual(selected, ["v8.2", "master"])

    def test_prepare_branch_sync_for_test_workflow_generates_branch_specific_config(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            base_config_path = get_repo_base_copybara_config_path(root)
            write_base_copybara_config(base_config_path)
            path_rules_path = get_repo_copybara_path_rules_path(root)
            write_copybara_path_rules(
                path_rules_path,
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
            )
            path_rules_module_path = get_repo_copybara_path_rules_module_path(root)
            write_copybara_path_rules_module(path_rules_module_path, path_rules_path)
            fragment_dir = root / "buildscripts" / "copybara"
            fragment_dir.mkdir(parents=True, exist_ok=True)
            fragment_path = fragment_dir / "v8_2.sky"
            fragment_path.write_text('sync_branch("v8.2")\n')
            config_bundle = sync_repo_with_copybara.CopybaraConfigBundle(
                config_sha="configsha123",
                bundle_dir=root,
                base_config_path=base_config_path,
                path_rules_path=path_rules_path,
                path_rules_module_path=path_rules_module_path,
                branch_to_fragment={"v8.2": fragment_path},
            )
            test_baseline = sync_repo_with_copybara.TestWorkflowBaseline(
                source_last_rev="feedface123",
                destination_base_revision="publicdeadbeef456",
                public_branch="v8.2",
            )

            stdout = io.StringIO()
            with redirect_stdout(stdout):
                prepared = sync_repo_with_copybara.prepare_branch_sync(
                    current_dir=tmpdir,
                    workflow="test",
                    branch="v8.2",
                    source_ref="deadbeef123",
                    config_bundle=config_bundle,
                    fragment_path=fragment_path,
                    tokens_map={
                        sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                        sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                        sync_repo_with_copybara.TEST_REPO_URL: "test-token",
                    },
                    test_branch_prefix="copybara_test_branch_patch123",
                    test_baseline=test_baseline,
                )
            log_output = stdout.getvalue()

            self.assertTrue(prepared.config_file.with_name("copybara_path_rules.json").is_file())
            self.assertTrue(
                prepared.config_file.with_name("copybara_path_rules.bara.sky").is_file()
            )

            generated_config = prepared.config_file.read_text()
            expected_test_branch = "copybara_test_branch_patch123_v8.2"

            self.assertIn('sync_branch("v8.2")', generated_config)
            self.assertIn('test_branch_prefix = "copybara_test_branch_patch123"', generated_config)
            self.assertIn(f'source_refs = {{"v8.2": "{expected_test_branch}"}}', generated_config)
            self.assertEqual(prepared.source_ref, expected_test_branch)
            self.assertEqual(prepared.config_sha, "configsha123")
            self.assertEqual(prepared.workflow_name, "test_v8.2")
            self.assertEqual(prepared.copybara_config.source.branch, expected_test_branch)
            self.assertEqual(prepared.copybara_config.source.ref, expected_test_branch)
            self.assertEqual(prepared.copybara_config.destination.branch, expected_test_branch)
            self.assertEqual(prepared.last_rev, "feedface123")
            self.assertEqual(prepared.test_baseline, test_baseline)
            self.assertEqual(
                prepared.dry_run_args,
                ("--init-history", "--last-rev=feedface123"),
            )
            self.assertIn(f"{prepared.config_file.parent}:/usr/src/app", prepared.docker_command)
            self.assertEqual(
                prepared.copybara_output_dir, prepared.config_file.parent / "copybara-output"
            )
            self.assertEqual(
                prepared.source_mirror_dir,
                prepared.config_file.parent
                / "copybara-output"
                / sync_repo_with_copybara.COPYBARA_SOURCE_MIRROR_DIR_NAME,
            )
            self.assertTrue(
                (prepared.preview_dir / sync_repo_with_copybara.COPYBARA_WORKDIR_NAME).is_dir()
            )
            self.assertIn(
                f"{prepared.preview_dir}:{sync_repo_with_copybara.COPYBARA_PREVIEW_CONTAINER_PATH}",
                prepared.docker_command,
            )
            self.assertIn(
                f"{prepared.copybara_output_dir}:{sync_repo_with_copybara.COPYBARA_OUTPUT_CONTAINER_PATH}",
                prepared.docker_command,
            )
            self.assertIn("--entrypoint", prepared.docker_command)
            self.assertIn("/bin/sh", prepared.docker_command)
            self.assertIn("-c", prepared.docker_command)
            self.assertIn(
                sync_repo_with_copybara.shell_quote(
                    sync_repo_with_copybara.COPYBARA_CONTAINER_ENTRYPOINT_SCRIPT
                ),
                prepared.docker_command,
            )
            self.assertIn("copybara-wrapper", prepared.docker_command)
            self.assertIn(
                sync_repo_with_copybara.COPYBARA_CONTAINER_GIT_CONFIG_PATH,
                sync_repo_with_copybara.COPYBARA_CONTAINER_ENTRYPOINT_SCRIPT,
            )
            self.assertIn(
                sync_repo_with_copybara.COPYBARA_SOURCE_MIRROR_CONTAINER_PATH,
                sync_repo_with_copybara.COPYBARA_CONTAINER_ENTRYPOINT_SCRIPT,
            )
            self.assertIn(
                "/usr/local/bin/copybara",
                sync_repo_with_copybara.COPYBARA_CONTAINER_ENTRYPOINT_SCRIPT,
            )
            self.assertIn(
                f"--work-dir={sync_repo_with_copybara.COPYBARA_WORKDIR_CONTAINER_PATH}",
                prepared.docker_command,
            )
            self.assertIn(
                f"--output-root={sync_repo_with_copybara.COPYBARA_OUTPUT_CONTAINER_PATH}",
                prepared.docker_command,
            )
            copybara_args = prepared.docker_command[prepared.docker_command.index("migrate") :]
            self.assertNotIn("-v", copybara_args)
            self.assertNotIn(
                f"{prepared.config_file}:/usr/src/app/copy.bara.sky", prepared.docker_command
            )
            self.assertIn("[v8.2] BEGIN generated Copybara config:", log_output)
            self.assertIn("[v8.2] BEGIN generated Copybara path rules module:", log_output)
            self.assertIn(
                'source_url = "https://x-access-token:<REDACTED>@github.com/10gen/mongo.git"',
                log_output,
            )
            self.assertIn("common_files_to_exclude = [", log_output)
            self.assertIn('"src/mongo/db/modules/**"', log_output)
            self.assertNotIn("source-token", log_output)
            self.assertNotIn("prod-token", log_output)
            self.assertNotIn("test-token", log_output)

    def test_prepare_branch_sync_for_test_workflow_uses_test_branch_for_sync_tag(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            base_config_path = get_repo_base_copybara_config_path(root)
            write_base_copybara_config(base_config_path)
            path_rules_path = get_repo_copybara_path_rules_path(root)
            write_copybara_path_rules(
                path_rules_path,
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
            )
            path_rules_module_path = get_repo_copybara_path_rules_module_path(root)
            write_copybara_path_rules_module(path_rules_module_path, path_rules_path)
            release_tag = "r0.0.0-copybara-test-tag-patch123"
            release_request = sync_repo_with_copybara.get_release_tag_request(release_tag)
            fragment_path = sync_repo_with_copybara.write_synthetic_release_tag_fragment(
                root, release_request
            )
            config_bundle = sync_repo_with_copybara.CopybaraConfigBundle(
                config_sha="configsha123",
                bundle_dir=root,
                base_config_path=base_config_path,
                path_rules_path=path_rules_path,
                path_rules_module_path=path_rules_module_path,
                branch_to_fragment={release_request.public_branch: fragment_path},
            )
            test_baseline = sync_repo_with_copybara.TestWorkflowBaseline(
                source_last_rev="feedface123",
                destination_base_revision="publicdeadbeef456",
                public_branch="master",
            )
            expected_test_branch = "copybara_test_branch_patch123_v0.0.0-copybara-test-tag-patch123"

            prepared = sync_repo_with_copybara.prepare_branch_sync(
                current_dir=tmpdir,
                workflow="test",
                branch=release_request.public_branch,
                source_ref=expected_test_branch,
                config_bundle=config_bundle,
                fragment_path=fragment_path,
                tokens_map={
                    sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                    sync_repo_with_copybara.TEST_REPO_URL: "test-token",
                },
                test_branch_prefix="copybara_test_branch_patch123",
                test_baseline=test_baseline,
                release_tag=release_tag,
            )

            generated_config = prepared.config_file.read_text()
            self.assertIn(f'sync_tag("{release_tag}")', generated_config)
            self.assertIn(
                f'source_refs = {{"v0.0.0-copybara-test-tag-patch123": "{expected_test_branch}"}}',
                generated_config,
            )
            self.assertEqual(prepared.workflow_name, f"test_{release_tag}")
            self.assertEqual(prepared.source_ref, expected_test_branch)
            self.assertEqual(prepared.copybara_config.source.branch, expected_test_branch)
            self.assertEqual(prepared.copybara_config.source.ref, expected_test_branch)
            self.assertEqual(prepared.copybara_config.destination.branch, expected_test_branch)

    @patch("buildscripts.copybara.sync_repo_with_copybara.check_destination_branch_exists")
    def test_prepare_branch_sync_for_prod_workflow_uses_configured_prod_destination(
        self, mock_check_destination_branch_exists
    ):
        mock_check_destination_branch_exists.return_value = True

        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            base_config_path = get_repo_base_copybara_config_path(root)
            write_base_copybara_config(
                base_config_path,
                prod_url=sync_repo_with_copybara.TEST_REPO_URL,
            )
            path_rules_path = get_repo_copybara_path_rules_path(root)
            write_copybara_path_rules(
                path_rules_path,
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
            )
            path_rules_module_path = get_repo_copybara_path_rules_module_path(root)
            write_copybara_path_rules_module(path_rules_module_path, path_rules_path)
            fragment_dir = root / "buildscripts" / "copybara"
            fragment_dir.mkdir(parents=True, exist_ok=True)
            fragment_path = fragment_dir / "v8_2.sky"
            fragment_path.write_text('sync_branch("v8.2")\n')
            config_bundle = sync_repo_with_copybara.CopybaraConfigBundle(
                config_sha="configsha123",
                bundle_dir=root,
                base_config_path=base_config_path,
                path_rules_path=path_rules_path,
                path_rules_module_path=path_rules_module_path,
                branch_to_fragment={"v8.2": fragment_path},
            )

            prepared = sync_repo_with_copybara.prepare_branch_sync(
                current_dir=tmpdir,
                workflow="prod",
                branch="v8.2",
                source_ref="deadbeef123",
                config_bundle=config_bundle,
                fragment_path=fragment_path,
                tokens_map={
                    sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                    sync_repo_with_copybara.TEST_REPO_URL: "test-token",
                },
                test_branch_prefix="copybara_test_branch_patch123",
            )

        self.assertEqual(
            prepared.copybara_config.destination.git_url,
            "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git",
        )
        self.assertEqual(
            prepared.copybara_config.destination.repo_name,
            sync_repo_with_copybara.TEST_REPO_NAME,
        )
        mock_check_destination_branch_exists.assert_called_once_with(prepared.copybara_config)

    @patch("buildscripts.copybara.sync_repo_with_copybara.check_destination_branch_exists")
    def test_prepare_branch_sync_can_run_prod_workflow_against_test_destination(
        self, mock_check_destination_branch_exists
    ):
        mock_check_destination_branch_exists.return_value = True

        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            base_config_path = get_repo_base_copybara_config_path(root)
            write_base_copybara_config(
                base_config_path,
                prod_url=sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                test_url=sync_repo_with_copybara.TEST_REPO_URL,
            )
            path_rules_path = get_repo_copybara_path_rules_path(root)
            write_copybara_path_rules(
                path_rules_path,
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
            )
            path_rules_module_path = get_repo_copybara_path_rules_module_path(root)
            write_copybara_path_rules_module(path_rules_module_path, path_rules_path)
            fragment_dir = root / "buildscripts" / "copybara"
            fragment_dir.mkdir(parents=True, exist_ok=True)
            fragment_path = fragment_dir / "v8_2.sky"
            fragment_path.write_text('sync_branch("v8.2")\n')
            config_bundle = sync_repo_with_copybara.CopybaraConfigBundle(
                config_sha="configsha123",
                bundle_dir=root,
                base_config_path=base_config_path,
                path_rules_path=path_rules_path,
                path_rules_module_path=path_rules_module_path,
                branch_to_fragment={"v8.2": fragment_path},
            )

            prepared = sync_repo_with_copybara.prepare_branch_sync(
                current_dir=tmpdir,
                workflow="prod",
                branch="v8.2",
                source_ref="deadbeef123",
                config_bundle=config_bundle,
                fragment_path=fragment_path,
                tokens_map={
                    sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                    sync_repo_with_copybara.TEST_REPO_URL: "test-token",
                },
                test_branch_prefix="copybara_test_branch_patch123",
                prod_url_override=sync_repo_with_copybara.TEST_REPO_URL,
            )
            generated_config = prepared.config_file.read_text()

        self.assertEqual(prepared.workflow_name, "prod_v8.2")
        self.assertEqual(prepared.copybara_config.destination.branch, "v8.2")
        self.assertEqual(
            prepared.copybara_config.destination.git_url,
            "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git",
        )
        self.assertEqual(prepared.copybara_config.destination.repo_name, "10gen/mongo-copybara")
        self.assertIn(
            'prod_url = "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git"',
            generated_config,
        )
        mock_check_destination_branch_exists.assert_called_once_with(prepared.copybara_config)


class TestCopybaraConfigAndTestWorkflowHelpers(unittest.TestCase):
    def test_get_test_workflow_base_branch_override_reads_sky_variable(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)
            sky_path.write_text(
                sky_path.read_text().replace(
                    'test_workflow_base_branch = ""',
                    'test_workflow_base_branch = "  v8.2  "',
                )
            )

            self.assertEqual(
                sync_repo_with_copybara.get_test_workflow_base_branch_override(sky_path),
                "v8.2",
            )

    def test_get_test_workflow_base_branch_override_defaults_to_none(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)

            self.assertIsNone(
                sync_repo_with_copybara.get_test_workflow_base_branch_override(sky_path)
            )

    def test_resolve_test_workflow_requested_branches_prefers_base_branch_override(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)
            sky_path.write_text(
                sky_path.read_text().replace(
                    'test_workflow_base_branch = ""',
                    'test_workflow_base_branch = "v8.2"',
                )
            )

            requested, override = sync_repo_with_copybara.resolve_test_workflow_requested_branches(
                "master",
                sky_path,
            )

            self.assertEqual(requested, "v8.2")
            self.assertEqual(override, "v8.2")

    def test_resolve_test_workflow_requested_branches_defaults_to_requested_branches(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)

            requested, override = sync_repo_with_copybara.resolve_test_workflow_requested_branches(
                "master",
                sky_path,
            )

            self.assertEqual(requested, "master")
            self.assertIsNone(override)

    def test_get_test_workflow_source_branch_override_reads_sky_variable(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)
            sky_path.write_text(
                sky_path.read_text().replace(
                    'test_workflow_source_branch = ""',
                    'test_workflow_source_branch = "  daniel.moody/8.3_test_branch  "',
                )
            )

            self.assertEqual(
                sync_repo_with_copybara.get_test_workflow_source_branch_override(sky_path),
                "daniel.moody/8.3_test_branch",
            )

    def test_get_test_workflow_source_branch_override_defaults_to_none(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)

            self.assertIsNone(
                sync_repo_with_copybara.get_test_workflow_source_branch_override(sky_path)
            )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_list_untracked_paths_skips_untracked_directories(self, mock_run_command):
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_dir = Path(tmpdir)
            (repo_dir / "new.txt").write_text("new")
            (repo_dir / "copybara").mkdir()
            mock_run_command.return_value = "new.txt\0copybara\0"

            untracked_paths = sync_repo_with_copybara.list_untracked_paths(repo_dir)

        self.assertEqual(untracked_paths, [Path("new.txt")])
        mock_run_command.assert_called_once_with(
            f"git -C {sync_repo_with_copybara.shell_quote(repo_dir)} "
            "ls-files --others --exclude-standard -z",
            merge_stderr=False,
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_list_untracked_paths_keeps_symlinked_directories(self, mock_run_command):
        if sys.platform == "win32":
            self.skipTest("symlink permissions vary on Windows")

        with tempfile.TemporaryDirectory() as tmpdir:
            repo_dir = Path(tmpdir)
            (repo_dir / "target_dir").mkdir()
            os.symlink("target_dir", repo_dir / "linked_dir")
            mock_run_command.return_value = "linked_dir\0"

            untracked_paths = sync_repo_with_copybara.list_untracked_paths(repo_dir)

        self.assertEqual(untracked_paths, [Path("linked_dir")])

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_list_changed_paths_between_refs_reads_machine_output_from_stdout_only(
        self, mock_run_command
    ):
        mock_run_command.side_effect = ["tracked.txt\0nested/file.cpp\0", "deleted.txt\0"]

        changed_paths, deleted_paths = sync_repo_with_copybara.list_changed_paths_between_refs(
            "/repo",
            "patchbase456",
        )

        self.assertEqual(changed_paths, [Path("tracked.txt"), Path("nested/file.cpp")])
        self.assertEqual(deleted_paths, [Path("deleted.txt")])
        self.assertEqual(mock_run_command.call_count, 2)
        for command_call in mock_run_command.call_args_list:
            self.assertEqual(command_call.kwargs, {"merge_stderr": False})

    def test_filter_untracked_paths_for_test_source_skips_task_generated_paths(self):
        filtered_paths = sync_repo_with_copybara.filter_untracked_paths_for_test_source(
            [
                Path(".evergreen.yml"),
                Path("tmp_copybara/config_bundle/copy.bara.sky"),
                Path("new.txt"),
                Path("buildscripts/new_copybara_fragment.sky"),
            ]
        )

        self.assertEqual(
            filtered_paths,
            [
                Path("new.txt"),
                Path("buildscripts/new_copybara_fragment.sky"),
            ],
        )

    def test_copy_paths_into_repo_skips_directories(self):
        with (
            tempfile.TemporaryDirectory() as source_tmpdir,
            tempfile.TemporaryDirectory() as dest_tmpdir,
        ):
            source_dir = Path(source_tmpdir)
            destination_dir = Path(dest_tmpdir)

            (source_dir / "tracked.txt").write_text("tracked")
            (source_dir / "copybara").mkdir()
            (source_dir / "copybara" / "README.md").write_text("nested repo contents")

            sync_repo_with_copybara.copy_paths_into_repo(
                source_dir,
                destination_dir,
                [Path("copybara"), Path("tracked.txt")],
            )

            self.assertFalse((destination_dir / "copybara").exists())
            self.assertEqual((destination_dir / "tracked.txt").read_text(), "tracked")

    def test_copy_paths_into_repo_preserves_symlinks(self):
        if sys.platform == "win32":
            self.skipTest("symlink permissions vary on Windows")

        with (
            tempfile.TemporaryDirectory() as source_tmpdir,
            tempfile.TemporaryDirectory() as dest_tmpdir,
        ):
            source_dir = Path(source_tmpdir)
            destination_dir = Path(dest_tmpdir)

            (source_dir / "target.txt").write_text("target")
            os.symlink("target.txt", source_dir / "linked.txt")
            (destination_dir / "linked.txt").write_text("stale")

            sync_repo_with_copybara.copy_paths_into_repo(
                source_dir,
                destination_dir,
                [Path("linked.txt")],
            )

            self.assertTrue((destination_dir / "linked.txt").is_symlink())
            self.assertEqual(os.readlink(destination_dir / "linked.txt"), "target.txt")

    def test_remove_paths_from_repo_removes_symlinks(self):
        if sys.platform == "win32":
            self.skipTest("symlink permissions vary on Windows")

        with tempfile.TemporaryDirectory() as tmpdir:
            repo_dir = Path(tmpdir)
            (repo_dir / "target.txt").write_text("target")
            os.symlink("target.txt", repo_dir / "linked.txt")

            sync_repo_with_copybara.remove_paths_from_repo(repo_dir, [Path("linked.txt")])

            self.assertFalse((repo_dir / "linked.txt").exists())
            self.assertFalse((repo_dir / "linked.txt").is_symlink())

    @patch("buildscripts.copybara.sync_repo_with_copybara.tempfile.mkdtemp")
    @patch("buildscripts.copybara.sync_repo_with_copybara.find_matching_commit_pair")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch("buildscripts.copybara.sync_repo_with_copybara.branch_exists_remote")
    def test_resolve_test_workflow_baseline_uses_public_branch_when_present(
        self,
        mock_branch_exists_remote,
        mock_run_command,
        mock_find_matching_commit_pair,
        mock_mkdtemp,
    ):
        mock_branch_exists_remote.return_value = True
        public_baseline_dir = Path("/tmp/public-baseline")
        mock_mkdtemp.return_value = str(public_baseline_dir)
        mock_find_matching_commit_pair.return_value = sync_repo_with_copybara.MatchingCommit(
            source_commit="private123",
            destination_commit="public456",
        )

        baseline = sync_repo_with_copybara.resolve_test_workflow_baseline(
            current_dir="/repo",
            branch="v8.2",
            patch_base_revision="deadbeef123",
            public_repo_url="https://example.com/public.git",
        )

        self.assertEqual(
            baseline,
            sync_repo_with_copybara.TestWorkflowBaseline(
                source_last_rev="private123",
                destination_base_revision="public456",
                public_branch="v8.2",
            ),
        )
        mock_run_command.assert_called_once_with(
            expected_single_branch_clone_command(
                "v8.2",
                "https://example.com/public.git",
                public_baseline_dir,
            )
        )
        mock_find_matching_commit_pair.assert_called_once_with(
            "/repo",
            str(public_baseline_dir),
            source_ref="deadbeef123",
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.tempfile.mkdtemp")
    @patch("buildscripts.copybara.sync_repo_with_copybara.find_matching_commit_pair")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch("buildscripts.copybara.sync_repo_with_copybara.branch_exists_remote")
    def test_resolve_test_workflow_baseline_uses_source_branch_override(
        self,
        mock_branch_exists_remote,
        mock_run_command,
        mock_find_matching_commit_pair,
        mock_mkdtemp,
    ):
        mock_branch_exists_remote.return_value = True
        public_baseline_dir = Path("/tmp/public-baseline")
        private_source_dir = Path("/tmp/private-source")
        mock_mkdtemp.side_effect = [str(public_baseline_dir), str(private_source_dir)]
        mock_find_matching_commit_pair.return_value = sync_repo_with_copybara.MatchingCommit(
            source_commit="private123",
            destination_commit="public456",
        )

        baseline = sync_repo_with_copybara.resolve_test_workflow_baseline(
            current_dir="/repo",
            branch="v8.2",
            patch_base_revision="deadbeef123",
            public_repo_url="https://example.com/public.git",
            source_repo_url="https://example.com/source.git",
            test_source_branch="daniel.moody/8.3_test_branch",
        )

        self.assertEqual(
            baseline,
            sync_repo_with_copybara.TestWorkflowBaseline(
                source_last_rev="private123",
                destination_base_revision="public456",
                public_branch="v8.2",
            ),
        )
        self.assertEqual(
            mock_run_command.call_args_list[0].args[0],
            expected_single_branch_clone_command(
                "daniel.moody/8.3_test_branch",
                "https://example.com/source.git",
                private_source_dir,
            ),
        )
        self.assertEqual(
            mock_run_command.call_args_list[1].args[0],
            expected_single_branch_clone_command(
                "v8.2",
                "https://example.com/public.git",
                public_baseline_dir,
            ),
        )
        mock_find_matching_commit_pair.assert_called_once_with(
            str(private_source_dir),
            str(public_baseline_dir),
            source_ref="HEAD",
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.tempfile.mkdtemp")
    @patch("buildscripts.copybara.sync_repo_with_copybara.find_matching_commit_pair")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch("buildscripts.copybara.sync_repo_with_copybara.branch_exists_remote")
    def test_resolve_test_workflow_baseline_falls_back_to_public_default_branch(
        self,
        mock_branch_exists_remote,
        mock_run_command,
        mock_find_matching_commit_pair,
        mock_mkdtemp,
    ):
        mock_branch_exists_remote.return_value = False
        public_baseline_dir = Path("/tmp/public-baseline")
        mock_mkdtemp.return_value = str(public_baseline_dir)
        mock_find_matching_commit_pair.return_value = sync_repo_with_copybara.MatchingCommit(
            source_commit="private123",
            destination_commit="public456",
        )

        baseline = sync_repo_with_copybara.resolve_test_workflow_baseline(
            current_dir="/repo",
            branch="v8.2-hotfix",
            patch_base_revision="deadbeef123",
            public_repo_url="https://example.com/public.git",
        )

        self.assertEqual(baseline.public_branch, sync_repo_with_copybara.COPYBARA_CONFIG_REF)
        mock_run_command.assert_called_once_with(
            expected_single_branch_clone_command(
                sync_repo_with_copybara.COPYBARA_CONFIG_REF,
                "https://example.com/public.git",
                public_baseline_dir,
            )
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_clone_source_repo_for_commit_discovery_clones_test_tag_source_branch(
        self, mock_run_command
    ):
        test_branch = "copybara_test_branch_patch123_v0.0.0-copybara-test-tag-patch123"
        source_url = "https://example.com/source.git"
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v0.0.0-copybara-test-tag-patch123",
            source_ref=test_branch,
            config_sha="local",
            workflow_name="test_r0.0.0-copybara-test-tag-patch123",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo",),
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=source_url,
                    repo_name=sync_repo_with_copybara.SOURCE_REPO_NAME,
                    branch=test_branch,
                    ref=test_branch,
                )
            ),
            release_tag="r0.0.0-copybara-test-tag-patch123",
        )

        sync_repo_with_copybara.clone_source_repo_for_commit_discovery(
            sync,
            Path("/tmp/source"),
        )

        mock_run_command.assert_called_once_with(
            expected_single_branch_clone_command(test_branch, source_url, Path("/tmp/source"))
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_clone_source_repo_for_commit_discovery_keeps_prod_tag_full_clone(
        self, mock_run_command
    ):
        source_url = "https://example.com/source.git"
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2.7",
            source_ref="tagsha123",
            config_sha="local",
            workflow_name="prod_r8.2.7",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo",),
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=source_url,
                    repo_name=sync_repo_with_copybara.SOURCE_REPO_NAME,
                    branch="v8.2.7",
                    ref="tagsha123",
                )
            ),
            release_tag="r8.2.7",
        )

        sync_repo_with_copybara.clone_source_repo_for_commit_discovery(
            sync,
            Path("/tmp/source"),
        )

        mock_run_command.assert_called_once_with(
            "git clone --filter=blob:none --no-checkout "
            f"{sync_repo_with_copybara.shell_quote(source_url)} "
            f"{sync_repo_with_copybara.shell_quote(Path('/tmp/source'))}"
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_clone_source_repo_for_commit_discovery_uses_local_source_mirror_branch(
        self, mock_run_command
    ):
        source_url = "/tmp/source-mirror.git"
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.0",
            source_ref="headsha",
            config_sha="local",
            workflow_name="prod_v8.0",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo",),
            copybara_source_url="file:///tmp/copybara-output/source-mirror.git",
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=source_url,
                    repo_name=sync_repo_with_copybara.SOURCE_REPO_NAME,
                    branch="v8.0",
                    ref="headsha",
                )
            ),
        )

        sync_repo_with_copybara.clone_source_repo_for_commit_discovery(
            sync,
            Path("/tmp/source"),
        )

        mock_run_command.assert_called_once_with(
            expected_single_branch_clone_command(
                "copybara_sync_source_v8_0", source_url, Path("/tmp/source")
            )
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.remove_paths_from_repo")
    @patch("buildscripts.copybara.sync_repo_with_copybara.copy_paths_into_repo")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_untracked_paths")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_changed_paths_between_refs")
    @patch("buildscripts.copybara.sync_repo_with_copybara.tempfile.mkdtemp")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_create_patched_test_source_repo_rebuilds_on_copybara_base_from_patch_base(
        self,
        mock_run_command,
        mock_mkdtemp,
        mock_list_changed_paths_between_refs,
        mock_list_untracked_paths,
        mock_copy_paths_into_repo,
        mock_remove_paths_from_repo,
    ):
        patched_source_dir = Path("/tmp/patched-source")
        mock_mkdtemp.return_value = str(patched_source_dir)
        mock_list_changed_paths_between_refs.return_value = (
            [Path("tracked.txt")],
            [Path("deleted.txt")],
        )
        mock_list_untracked_paths.return_value = [
            Path(".copybara_release_fragments/v0_0_0.sky"),
            Path(".evergreen.yml"),
            Path("tmp_copybara/config_bundle/copy.bara.sky"),
            Path("new.txt"),
        ]
        mock_run_command.side_effect = ["", "", "", "M tracked.txt\n", ""]

        result = sync_repo_with_copybara.create_patched_test_source_repo(
            "/repo",
            "copybarabase123",
            "patchbase456",
            "patch123",
        )

        self.assertEqual(result, patched_source_dir)
        mock_list_changed_paths_between_refs.assert_called_once_with("/repo", "patchbase456")
        mock_list_untracked_paths.assert_called_once_with("/repo")
        mock_copy_paths_into_repo.assert_called_once_with(
            "/repo",
            patched_source_dir,
            [Path("new.txt"), Path("tracked.txt")],
        )
        mock_remove_paths_from_repo.assert_called_once_with(
            patched_source_dir,
            [Path("deleted.txt")],
        )
        self.assertEqual(
            mock_run_command.call_args_list[0].args[0],
            f"git clone --shared --no-checkout {sync_repo_with_copybara.shell_quote('/repo')} "
            f"{sync_repo_with_copybara.shell_quote(patched_source_dir)}",
        )
        self.assertEqual(
            mock_run_command.call_args_list[1].args[0],
            "git -C "
            f"{sync_repo_with_copybara.shell_quote(patched_source_dir)} "
            f"checkout --detach {sync_repo_with_copybara.shell_quote('copybarabase123')}",
        )
        for command_call in mock_run_command.call_args_list:
            self.assertNotIn(" apply ", command_call.args[0])

    def test_create_patched_test_source_repo_collapses_patch_diff_onto_copybara_base(self):
        os.chdir(Path(__file__).resolve().parents[2])
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_dir = Path(tmpdir) / "repo"
            repo_dir.mkdir()
            sync_repo_with_copybara.run_command(f"git -C {repo_dir} init")
            sync_repo_with_copybara.run_command(
                f'git -C {repo_dir} config --local user.email "test@example.com"'
            )
            sync_repo_with_copybara.run_command(
                f'git -C {repo_dir} config --local user.name "Test User"'
            )
            sync_repo_with_copybara.run_command(
                f"git -C {repo_dir} config --local commit.gpgsign false"
            )

            tracked_file = repo_dir / "tracked.txt"
            tracked_file.write_text("base\n")
            sync_repo_with_copybara.run_command(f"git -C {repo_dir} add tracked.txt")
            sync_repo_with_copybara.run_command(f'git -C {repo_dir} commit -m "copybara base"')
            copybara_base_revision = sync_repo_with_copybara.run_command(
                f"git -C {repo_dir} rev-parse HEAD"
            ).strip()

            tracked_file.write_text("intermediate\n")
            (repo_dir / "upstream_only.txt").write_text("upstream only\n")
            sync_repo_with_copybara.run_command(
                f"git -C {repo_dir} add tracked.txt upstream_only.txt"
            )
            sync_repo_with_copybara.run_command(f'git -C {repo_dir} commit -m "patch base"')
            patch_base_revision = sync_repo_with_copybara.run_command(
                f"git -C {repo_dir} rev-parse HEAD"
            ).strip()

            tracked_file.write_text("current workspace\n")
            (repo_dir / "new.txt").write_text("new file\n")

            patched_repo_dir = sync_repo_with_copybara.create_patched_test_source_repo(
                str(repo_dir),
                copybara_base_revision,
                patch_base_revision,
                "patch123",
            )
            try:
                self.assertEqual(
                    (patched_repo_dir / "tracked.txt").read_text(), "current workspace\n"
                )
                self.assertEqual((patched_repo_dir / "new.txt").read_text(), "new file\n")
                self.assertFalse((patched_repo_dir / "upstream_only.txt").exists())
                self.assertEqual(
                    sync_repo_with_copybara.run_command(
                        f"git -C {patched_repo_dir} rev-list --count "
                        f"{copybara_base_revision}..HEAD"
                    ).strip(),
                    "1",
                )
                self.assertEqual(
                    sync_repo_with_copybara.run_command(
                        f"git -C {patched_repo_dir} rev-parse HEAD~1"
                    ).strip(),
                    copybara_base_revision,
                )
                self.assertEqual(
                    sync_repo_with_copybara.run_command(
                        f"git -C {patched_repo_dir} log -1 --pretty=%s"
                    ).strip(),
                    "Evergreen patch for version_id patch123",
                )
            finally:
                shutil.rmtree(patched_repo_dir, ignore_errors=True)

    @patch("buildscripts.copybara.sync_repo_with_copybara.tempfile.mkdtemp")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_create_test_source_repo_from_branch_clones_existing_branch(
        self,
        mock_run_command,
        mock_mkdtemp,
    ):
        source_branch_dir = Path("/tmp/source-branch")
        mock_mkdtemp.return_value = str(source_branch_dir)

        result = sync_repo_with_copybara.create_test_source_repo_from_branch(
            "https://example.com/source.git",
            "daniel.moody/8.3_test_branch",
        )

        self.assertEqual(result, source_branch_dir)
        mock_run_command.assert_called_once_with(
            expected_single_branch_clone_command(
                "daniel.moody/8.3_test_branch",
                "https://example.com/source.git",
                source_branch_dir,
            )
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch("buildscripts.copybara.sync_repo_with_copybara.push_test_destination_branch")
    @patch("buildscripts.copybara.sync_repo_with_copybara.branch_exists_remote")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_patched_test_source_repo")
    @patch("buildscripts.copybara.sync_repo_with_copybara.is_current_repo_origin")
    def test_push_test_branches_pushes_clean_destination_and_patched_source(
        self,
        mock_is_current_repo_origin,
        mock_create_patched_test_source_repo,
        mock_branch_exists_remote,
        mock_push_test_destination_branch,
        mock_run_command,
    ):
        mock_is_current_repo_origin.return_value = True
        patched_source_dir = Path("/tmp/patched-source")
        mock_create_patched_test_source_repo.return_value = patched_source_dir
        mock_branch_exists_remote.return_value = True

        test_branch = "copybara_test_branch_patch123_v8.2"
        source_url = "https://example.com/source.git"
        destination_url = "https://example.com/destination.git"
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref=test_branch,
            config_sha="local",
            workflow_name="test_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo",),
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=source_url,
                    repo_name=sync_repo_with_copybara.SOURCE_REPO_NAME,
                    branch=test_branch,
                    ref=test_branch,
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=destination_url,
                    repo_name=sync_repo_with_copybara.TEST_REPO_NAME,
                    branch=test_branch,
                ),
            ),
            last_rev="privatebase123",
            test_baseline=sync_repo_with_copybara.TestWorkflowBaseline(
                source_last_rev="privatebase123",
                destination_base_revision="publicbase456",
                public_branch="v8.2",
            ),
        )

        sync_repo_with_copybara.push_test_branches(
            "/repo",
            [sync],
            "patch123",
            patch_base_revision="patchbase456",
            public_repo_url="https://example.com/public.git",
            source_repo_url=source_url,
        )

        mock_create_patched_test_source_repo.assert_called_once_with(
            "/repo",
            "privatebase123",
            "patchbase456",
            "patch123",
        )
        mock_push_test_destination_branch.assert_called_once_with(
            public_repo_url="https://example.com/public.git",
            public_branch="v8.2",
            destination_url=destination_url,
            destination_branch=test_branch,
            destination_base_revision="publicbase456",
        )
        mock_run_command.assert_has_calls(
            [
                call(
                    "git push "
                    f"{sync_repo_with_copybara.shell_quote(source_url)} --delete "
                    f"{sync_repo_with_copybara.shell_quote(test_branch)}"
                ),
                call(
                    "git push "
                    f"{sync_repo_with_copybara.shell_quote(destination_url)} --delete "
                    f"{sync_repo_with_copybara.shell_quote(test_branch)}"
                ),
                call(
                    f"git -C {sync_repo_with_copybara.shell_quote(patched_source_dir)} push "
                    f"{sync_repo_with_copybara.shell_quote(source_url)} "
                    f"{sync_repo_with_copybara.shell_quote(f'HEAD:refs/heads/{test_branch}')}"
                ),
            ]
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch("buildscripts.copybara.sync_repo_with_copybara.push_test_destination_branch")
    @patch("buildscripts.copybara.sync_repo_with_copybara.tag_exists_remote")
    @patch("buildscripts.copybara.sync_repo_with_copybara.branch_exists_remote")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_test_source_repo_from_branch")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_patched_test_source_repo")
    @patch("buildscripts.copybara.sync_repo_with_copybara.is_current_repo_origin")
    def test_push_test_branches_uses_source_branch_override(
        self,
        mock_is_current_repo_origin,
        mock_create_patched_test_source_repo,
        mock_create_test_source_repo_from_branch,
        mock_branch_exists_remote,
        mock_tag_exists_remote,
        mock_push_test_destination_branch,
        mock_run_command,
    ):
        mock_is_current_repo_origin.return_value = True
        source_branch_dir = Path("/tmp/source-branch")
        mock_create_test_source_repo_from_branch.return_value = source_branch_dir
        mock_branch_exists_remote.return_value = True
        mock_tag_exists_remote.return_value = False

        test_branch = "copybara_test_branch_patch123_v8.2"
        source_url = "https://example.com/source.git"
        destination_url = "https://example.com/destination.git"
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref=test_branch,
            config_sha="local",
            workflow_name="test_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo",),
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=source_url,
                    repo_name=sync_repo_with_copybara.SOURCE_REPO_NAME,
                    branch=test_branch,
                    ref=test_branch,
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=destination_url,
                    repo_name=sync_repo_with_copybara.TEST_REPO_NAME,
                    branch=test_branch,
                ),
            ),
            last_rev="privatebase123",
            test_baseline=sync_repo_with_copybara.TestWorkflowBaseline(
                source_last_rev="privatebase123",
                destination_base_revision="publicbase456",
                public_branch="v8.2",
            ),
        )

        sync_repo_with_copybara.push_test_branches(
            "/repo",
            [sync],
            "patch123",
            patch_base_revision="patchbase456",
            public_repo_url="https://example.com/public.git",
            source_repo_url=source_url,
            test_source_branch="daniel.moody/8.3_test_branch",
        )

        mock_create_patched_test_source_repo.assert_not_called()
        mock_create_test_source_repo_from_branch.assert_called_once_with(
            source_url,
            "daniel.moody/8.3_test_branch",
        )
        mock_push_test_destination_branch.assert_called_once()
        self.assertEqual(
            mock_run_command.call_args_list[-1].args[0],
            f"git -C {sync_repo_with_copybara.shell_quote(source_branch_dir)} push "
            f"{sync_repo_with_copybara.shell_quote(source_url)} "
            f"{sync_repo_with_copybara.shell_quote(f'HEAD:refs/heads/{test_branch}')}",
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch("buildscripts.copybara.sync_repo_with_copybara.push_test_destination_branch")
    @patch("buildscripts.copybara.sync_repo_with_copybara.branch_exists_remote")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_patched_test_source_repo")
    @patch("buildscripts.copybara.sync_repo_with_copybara.is_current_repo_origin")
    def test_push_test_branches_uses_test_branch_for_sync_tag(
        self,
        mock_is_current_repo_origin,
        mock_create_patched_test_source_repo,
        mock_branch_exists_remote,
        mock_push_test_destination_branch,
        mock_run_command,
    ):
        mock_is_current_repo_origin.return_value = True
        patched_source_dir = Path("/tmp/patched-source")
        mock_create_patched_test_source_repo.return_value = patched_source_dir
        mock_branch_exists_remote.return_value = True

        release_tag = "r0.0.0-copybara-test-tag-patch123"
        test_branch = "copybara_test_branch_patch123_v0.0.0-copybara-test-tag-patch123"
        source_url = "https://example.com/source.git"
        destination_url = "https://example.com/destination.git"
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v0.0.0-copybara-test-tag-patch123",
            source_ref=test_branch,
            config_sha="local",
            workflow_name=f"test_{release_tag}",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo",),
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=source_url,
                    repo_name=sync_repo_with_copybara.SOURCE_REPO_NAME,
                    branch=test_branch,
                    ref=test_branch,
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=destination_url,
                    repo_name=sync_repo_with_copybara.TEST_REPO_NAME,
                    branch=test_branch,
                ),
            ),
            last_rev="privatebase123",
            test_baseline=sync_repo_with_copybara.TestWorkflowBaseline(
                source_last_rev="privatebase123",
                destination_base_revision="publicbase456",
                public_branch="master",
            ),
            release_tag=release_tag,
        )

        sync_repo_with_copybara.push_test_branches(
            "/repo",
            [sync],
            "patch123",
            patch_base_revision="patchbase456",
            public_repo_url="https://example.com/public.git",
            source_repo_url=source_url,
        )

        mock_push_test_destination_branch.assert_called_once_with(
            public_repo_url="https://example.com/public.git",
            public_branch="master",
            destination_url=destination_url,
            destination_branch=test_branch,
            destination_base_revision="publicbase456",
        )
        mock_run_command.assert_has_calls(
            [
                call(
                    "git push "
                    f"{sync_repo_with_copybara.shell_quote(source_url)} --delete "
                    f"{sync_repo_with_copybara.shell_quote(test_branch)}"
                ),
                call(
                    "git push "
                    f"{sync_repo_with_copybara.shell_quote(destination_url)} --delete "
                    f"{sync_repo_with_copybara.shell_quote(test_branch)}"
                ),
                call(
                    f"git -C {sync_repo_with_copybara.shell_quote(patched_source_dir)} push "
                    f"{sync_repo_with_copybara.shell_quote(source_url)} "
                    f"{sync_repo_with_copybara.shell_quote(f'HEAD:refs/heads/{test_branch}')}"
                ),
            ]
        )


class TestTestReleaseTagPublishing(unittest.TestCase):
    @patch("buildscripts.copybara.sync_repo_with_copybara.shutil.rmtree")
    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.tempfile.mkdtemp",
        return_value="/tmp/copybara-test-tag-destination",
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.delete_remote_tag")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_publish_test_release_tag_pushes_destination_branch_head(
        self,
        mock_run_command,
        mock_delete_remote_tag,
        mock_mkdtemp,
        mock_rmtree,
    ):
        mock_run_command.side_effect = ["", "destsha123\n", ""]
        release_tag = "r0.0.0-copybara-test-tag-patch123"
        destination_branch = "copybara_test_branch_patch123_v0.0.0-copybara-test-tag-patch123"
        destination_url = sync_repo_with_copybara.TEST_REPO_URL
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v0.0.0-copybara-test-tag-patch123",
            source_ref=destination_branch,
            config_sha="local",
            workflow_name=f"test_{release_tag}",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo",),
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=destination_url,
                    repo_name=sync_repo_with_copybara.TEST_REPO_NAME,
                    branch=destination_branch,
                ),
            ),
            release_tag=release_tag,
        )
        tokens = {
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        authenticated_destination_url = sync_repo_with_copybara.auth_github_url(
            destination_url, "test-token"
        )

        sync_repo_with_copybara.publish_test_release_tag(sync, tokens)

        mock_mkdtemp.assert_called_once_with(prefix="copybara_test_tag_destination_")
        mock_delete_remote_tag.assert_called_once_with(authenticated_destination_url, release_tag)
        destination_repo_dir = Path("/tmp/copybara-test-tag-destination")
        mock_run_command.assert_has_calls(
            [
                call(
                    " ".join(
                        [
                            "git",
                            "clone",
                            "--filter=blob:none",
                            "--no-checkout",
                            "--single-branch",
                            "-b",
                            sync_repo_with_copybara.shell_quote(destination_branch),
                            sync_repo_with_copybara.shell_quote(authenticated_destination_url),
                            sync_repo_with_copybara.shell_quote(destination_repo_dir),
                        ]
                    )
                ),
                call(
                    f"git -C {sync_repo_with_copybara.shell_quote(destination_repo_dir)} "
                    "rev-parse HEAD"
                ),
                call(
                    " ".join(
                        [
                            "git",
                            "-C",
                            sync_repo_with_copybara.shell_quote(destination_repo_dir),
                            "push",
                            sync_repo_with_copybara.shell_quote(authenticated_destination_url),
                            sync_repo_with_copybara.shell_quote(f"HEAD:refs/tags/{release_tag}"),
                        ]
                    )
                ),
            ]
        )
        mock_rmtree.assert_called_once_with(destination_repo_dir, ignore_errors=True)


class TestMainWorkflow(unittest.TestCase):
    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.handle_failure")
    @patch("buildscripts.copybara.sync_repo_with_copybara.publish_test_release_tag")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_commit_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.push_test_branches")
    @patch("buildscripts.copybara.sync_repo_with_copybara.prepare_branch_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.resolve_test_workflow_baseline")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_test_workflow_source_branch_override")
    @patch("buildscripts.copybara.sync_repo_with_copybara.resolve_test_workflow_requested_branches")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_local_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_test_workflow_base_revision")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_mongodb_bot_gitconfig")
    @patch("buildscripts.copybara.sync_repo_with_copybara.ensure_copybara_checkout_and_image")
    @patch("buildscripts.copybara.sync_repo_with_copybara.read_config_file")
    @patch("buildscripts.copybara.sync_repo_with_copybara.os.getcwd")
    def test_main_runs_test_migration_after_successful_dry_run(
        self,
        mock_getcwd,
        mock_read_config_file,
        mock_ensure_copybara_checkout_and_image,
        mock_create_mongodb_bot_gitconfig,
        mock_get_copybara_tokens,
        mock_get_test_workflow_base_revision,
        mock_get_local_copybara_config_bundle,
        mock_resolve_test_workflow_requested_branches,
        mock_get_test_workflow_source_branch_override,
        mock_resolve_test_workflow_baseline,
        mock_prepare_branch_sync,
        mock_push_test_branches,
        mock_run_branch_commit_sync,
        mock_publish_test_release_tag,
        mock_handle_failure,
        mock_ensure_generated_copybara_evergreen_is_current,
    ):
        mock_read_config_file.return_value = {
            "project": sync_repo_with_copybara.EXPECTED_EVERGREEN_PROJECT,
            "version_id": "patch123",
        }
        mock_getcwd.return_value = "/repo"
        tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        mock_get_copybara_tokens.side_effect = [tokens, tokens]
        mock_get_test_workflow_base_revision.return_value = "base123"
        mock_resolve_test_workflow_requested_branches.return_value = ("v8.2", "v8.2")
        mock_get_test_workflow_source_branch_override.return_value = "daniel.moody/v8.2_test_branch"
        test_baseline = sync_repo_with_copybara.TestWorkflowBaseline(
            source_last_rev="private123",
            destination_base_revision="public456",
            public_branch="v8.2",
        )
        mock_resolve_test_workflow_baseline.return_value = test_baseline

        with tempfile.TemporaryDirectory() as tmpdir:
            base_config_path = Path(tmpdir) / "copy.bara.sky"
            fragment_path = Path(tmpdir) / "v8_2.sky"
            write_base_copybara_config(base_config_path)
            fragment_path.write_text('sync_branch("v8.2")\n')
            mock_get_local_copybara_config_bundle.return_value = (
                sync_repo_with_copybara.CopybaraConfigBundle(
                    config_sha="local",
                    bundle_dir=Path(tmpdir),
                    base_config_path=base_config_path,
                    path_rules_path=Path(tmpdir) / "copybara_path_rules.json",
                    path_rules_module_path=Path(tmpdir) / "copybara_path_rules.bara.sky",
                    branch_to_fragment={"v8.2": fragment_path},
                )
            )
            prepared_sync = sync_repo_with_copybara.PreparedBranchSync(
                branch="v8.2",
                source_ref="copybara_test_branch_patch123_v8.2",
                config_sha="local",
                workflow_name="test_v8.2",
                config_file=Path(tmpdir) / "generated.sky",
                preview_dir=Path(tmpdir) / "preview",
                docker_command=("echo",),
                copybara_config=sync_repo_with_copybara.CopybaraConfig(
                    source=sync_repo_with_copybara.CopybaraRepoConfig(
                        git_url="https://example.com/source.git",
                        repo_name=sync_repo_with_copybara.SOURCE_REPO_NAME,
                        branch="copybara_test_branch_patch123_v8.2",
                        ref="copybara_test_branch_patch123_v8.2",
                    ),
                    destination=sync_repo_with_copybara.CopybaraRepoConfig(
                        git_url="https://example.com/destination.git",
                        repo_name=sync_repo_with_copybara.TEST_REPO_NAME,
                        branch="copybara_test_branch_patch123_v8.2",
                    ),
                ),
                last_rev="private123",
                test_baseline=test_baseline,
            )
            mock_prepare_branch_sync.return_value = prepared_sync
            mock_run_branch_commit_sync.return_value = (
                sync_repo_with_copybara.BranchCommitSyncResult(
                    branch="v8.2",
                    discovered_commits=1,
                    migrated_commits=1,
                )
            )

            argv = ["buildscripts/copybara/sync_repo_with_copybara.py", "--workflow=test"]
            with patch.object(sys, "argv", argv):
                sync_repo_with_copybara.main()

        mock_prepare_branch_sync.assert_called_once_with(
            current_dir="/repo",
            workflow="test",
            branch="v8.2",
            source_ref="private123",
            config_bundle=mock_get_local_copybara_config_bundle.return_value,
            fragment_path=fragment_path,
            tokens_map=tokens,
            test_branch_prefix="copybara_test_branch_patch123",
            test_baseline=test_baseline,
            expansions=mock_read_config_file.return_value,
            release_tag=None,
            release_source_commit=None,
        )
        mock_push_test_branches.assert_called_once()
        self.assertEqual(
            mock_push_test_branches.call_args.kwargs["patch_base_revision"],
            "base123",
        )
        mock_ensure_generated_copybara_evergreen_is_current.assert_called_once_with(Path(tmpdir))
        mock_run_branch_commit_sync.assert_called_once_with(prepared_sync, tokens)
        mock_publish_test_release_tag.assert_not_called()
        mock_handle_failure.assert_not_called()

    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.handle_failure")
    @patch("buildscripts.copybara.sync_repo_with_copybara.publish_test_release_tag")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_commit_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.push_test_branches")
    @patch("buildscripts.copybara.sync_repo_with_copybara.prepare_branch_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.resolve_test_workflow_baseline")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_test_workflow_source_branch_override")
    @patch("buildscripts.copybara.sync_repo_with_copybara.resolve_test_workflow_requested_branches")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_local_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_test_workflow_base_revision")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_mongodb_bot_gitconfig")
    @patch("buildscripts.copybara.sync_repo_with_copybara.ensure_copybara_checkout_and_image")
    @patch("buildscripts.copybara.sync_repo_with_copybara.read_config_file")
    @patch("buildscripts.copybara.sync_repo_with_copybara.os.getcwd")
    def test_main_adds_synthetic_test_tag_when_requested(
        self,
        mock_getcwd,
        mock_read_config_file,
        mock_ensure_copybara_checkout_and_image,
        mock_create_mongodb_bot_gitconfig,
        mock_get_copybara_tokens,
        mock_get_test_workflow_base_revision,
        mock_get_local_copybara_config_bundle,
        mock_resolve_test_workflow_requested_branches,
        mock_get_test_workflow_source_branch_override,
        mock_resolve_test_workflow_baseline,
        mock_prepare_branch_sync,
        mock_push_test_branches,
        mock_run_branch_commit_sync,
        mock_publish_test_release_tag,
        mock_handle_failure,
        mock_ensure_generated_copybara_evergreen_is_current,
    ):
        mock_read_config_file.return_value = {
            "project": sync_repo_with_copybara.EXPECTED_EVERGREEN_PROJECT,
            "version_id": "patch123",
        }
        mock_getcwd.return_value = "/repo"
        tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        mock_get_copybara_tokens.return_value = tokens
        mock_get_test_workflow_base_revision.return_value = "base123"
        mock_resolve_test_workflow_requested_branches.return_value = ("master", None)
        mock_get_test_workflow_source_branch_override.return_value = None
        test_baseline = sync_repo_with_copybara.TestWorkflowBaseline(
            source_last_rev="private123",
            destination_base_revision="public456",
            public_branch="master",
        )
        mock_resolve_test_workflow_baseline.return_value = test_baseline

        with tempfile.TemporaryDirectory() as tmpdir:
            base_config_path = Path(tmpdir) / "copy.bara.sky"
            fragment_path = Path(tmpdir) / "master.sky"
            write_base_copybara_config(base_config_path)
            fragment_path.write_text('sync_branch("master")\n')
            config_bundle = sync_repo_with_copybara.CopybaraConfigBundle(
                config_sha="local",
                bundle_dir=Path(tmpdir),
                base_config_path=base_config_path,
                path_rules_path=Path(tmpdir) / "copybara_path_rules.json",
                path_rules_module_path=Path(tmpdir) / "copybara_path_rules.bara.sky",
                branch_to_fragment={"master": fragment_path},
            )
            mock_get_local_copybara_config_bundle.return_value = config_bundle

            def prepare_side_effect(**kwargs):
                return sync_repo_with_copybara.PreparedBranchSync(
                    branch=kwargs["branch"],
                    source_ref=kwargs["source_ref"],
                    config_sha="local",
                    workflow_name=sync_repo_with_copybara.get_prepared_copybara_workflow_name(
                        kwargs["workflow"],
                        kwargs["branch"],
                        kwargs["release_tag"],
                    ),
                    config_file=Path(tmpdir) / f"{kwargs['branch']}.sky",
                    preview_dir=Path(tmpdir) / "preview",
                    docker_command=("echo",),
                    release_tag=kwargs["release_tag"],
                )

            mock_prepare_branch_sync.side_effect = prepare_side_effect
            mock_run_branch_commit_sync.side_effect = lambda sync, tokens: (
                sync_repo_with_copybara.BranchCommitSyncResult(branch=sync.branch)
            )

            argv = [
                "buildscripts/copybara/sync_repo_with_copybara.py",
                "--workflow=test",
                "--branches=master",
                "--test-sync-tag",
            ]
            with patch.object(sys, "argv", argv):
                sync_repo_with_copybara.main()
            tag_fragment_contents = (
                mock_prepare_branch_sync.call_args_list[1].kwargs["fragment_path"].read_text()
            )

        release_tag = "r0.0.0-copybara-test-tag-patch123"
        public_branch = "v0.0.0-copybara-test-tag-patch123"
        self.assertEqual(mock_prepare_branch_sync.call_count, 2)
        branch_prepare_call = mock_prepare_branch_sync.call_args_list[0].kwargs
        tag_prepare_call = mock_prepare_branch_sync.call_args_list[1].kwargs
        self.assertEqual(branch_prepare_call["branch"], "master")
        self.assertIsNone(branch_prepare_call["release_tag"])
        self.assertEqual(tag_prepare_call["branch"], public_branch)
        self.assertEqual(
            tag_prepare_call["source_ref"],
            f"copybara_test_branch_patch123_{public_branch}",
        )
        self.assertEqual(tag_prepare_call["release_tag"], release_tag)
        self.assertIn(f'sync_tag("{release_tag}")', tag_fragment_contents)
        mock_push_test_branches.assert_called_once()
        self.assertEqual(
            mock_push_test_branches.call_args.kwargs["patch_base_revision"],
            "base123",
        )
        self.assertEqual(len(mock_push_test_branches.call_args.kwargs["prepared_syncs"]), 2)
        self.assertEqual(mock_run_branch_commit_sync.call_count, 2)
        mock_publish_test_release_tag.assert_called_once()
        published_sync, published_tokens = mock_publish_test_release_tag.call_args.args
        self.assertEqual(published_sync.release_tag, release_tag)
        self.assertEqual(published_tokens, tokens)
        mock_handle_failure.assert_not_called()

    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.handle_failure")
    @patch("buildscripts.copybara.sync_repo_with_copybara.publish_release_tag")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_commit_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.prepare_branch_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.activate_new_hotfix_tasks")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_commit")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_origin")
    @patch("buildscripts.copybara.sync_repo_with_copybara.fetch_remote_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_local_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_mongodb_bot_gitconfig")
    @patch("buildscripts.copybara.sync_repo_with_copybara.ensure_copybara_checkout_and_image")
    @patch("buildscripts.copybara.sync_repo_with_copybara.read_config_file")
    @patch("buildscripts.copybara.sync_repo_with_copybara.os.getcwd")
    def test_main_runs_prod_release_tag_request_against_test_repo(
        self,
        mock_getcwd,
        mock_read_config_file,
        mock_ensure_copybara_checkout_and_image,
        mock_create_mongodb_bot_gitconfig,
        mock_get_copybara_tokens,
        mock_get_local_copybara_config_bundle,
        mock_fetch_remote_copybara_config_bundle,
        mock_get_remote_tag_origin,
        mock_get_remote_tag_commit,
        mock_activate_new_hotfix_tasks,
        mock_prepare_branch_sync,
        mock_run_branch_commit_sync,
        mock_publish_release_tag,
        mock_handle_failure,
        mock_ensure_generated_copybara_evergreen_is_current,
    ):
        mock_read_config_file.return_value = {
            "project": sync_repo_with_copybara.EXPECTED_EVERGREEN_PROJECT,
        }
        mock_getcwd.return_value = "/repo"
        tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        mock_get_copybara_tokens.side_effect = [tokens, tokens]
        mock_get_remote_tag_origin.return_value = None
        mock_get_remote_tag_commit.return_value = "tagsha123"

        with tempfile.TemporaryDirectory() as tmpdir:
            base_config_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                base_config_path,
                prod_url=sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                test_url=sync_repo_with_copybara.TEST_REPO_URL,
            )
            fragment_path = Path(tmpdir) / "master.sky"
            fragment_path.write_text('sync_branch("master")\n')
            config_bundle = sync_repo_with_copybara.CopybaraConfigBundle(
                config_sha="configsha123",
                bundle_dir=Path(tmpdir),
                base_config_path=base_config_path,
                path_rules_path=Path(tmpdir) / "copybara_path_rules.json",
                path_rules_module_path=Path(tmpdir) / "copybara_path_rules.bara.sky",
                branch_to_fragment={"master": fragment_path},
            )
            mock_get_local_copybara_config_bundle.return_value = config_bundle

            def prepare_side_effect(**kwargs):
                return sync_repo_with_copybara.PreparedBranchSync(
                    branch=kwargs["branch"],
                    source_ref=kwargs["source_ref"],
                    config_sha="configsha123",
                    workflow_name=sync_repo_with_copybara.get_prepared_copybara_workflow_name(
                        kwargs["workflow"],
                        kwargs["branch"],
                        kwargs["release_tag"],
                    ),
                    config_file=Path(tmpdir) / f"{kwargs['branch']}.sky",
                    preview_dir=Path(tmpdir) / "preview",
                    docker_command=("echo",),
                    release_tag=kwargs["release_tag"],
                )

            mock_prepare_branch_sync.side_effect = prepare_side_effect
            mock_run_branch_commit_sync.return_value = (
                sync_repo_with_copybara.BranchCommitSyncResult(branch="v8.2.7")
            )

            argv = [
                "buildscripts/copybara/sync_repo_with_copybara.py",
                "--workflow=prod",
                "--prod-destination=test",
                "--branches=r8.2.7",
            ]
            with patch.object(sys, "argv", argv):
                sync_repo_with_copybara.main()
            tag_fragment_contents = mock_prepare_branch_sync.call_args.kwargs[
                "fragment_path"
            ].read_text()

        prepare_call = mock_prepare_branch_sync.call_args.kwargs
        authenticated_test_url = (
            "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git"
        )
        self.assertEqual(prepare_call["branch"], "v8.2.7")
        self.assertEqual(prepare_call["workflow"], "prod")
        self.assertEqual(prepare_call["source_ref"], "tagsha123")
        self.assertEqual(prepare_call["release_tag"], "r8.2.7")
        self.assertEqual(prepare_call["release_source_commit"], "tagsha123")
        self.assertEqual(prepare_call["prod_url_override"], authenticated_test_url)
        self.assertIn('sync_tag("r8.2.7")', tag_fragment_contents)
        mock_get_local_copybara_config_bundle.assert_called_once_with("/repo")
        mock_fetch_remote_copybara_config_bundle.assert_not_called()
        mock_ensure_generated_copybara_evergreen_is_current.assert_called_once_with(Path(tmpdir))
        mock_get_remote_tag_origin.assert_called_once_with(authenticated_test_url, "r8.2.7")
        mock_activate_new_hotfix_tasks.assert_not_called()
        mock_run_branch_commit_sync.assert_called_once()
        mock_publish_release_tag.assert_called_once()
        published_sync, published_tokens = mock_publish_release_tag.call_args.args
        self.assertEqual(published_sync.release_tag, "r8.2.7")
        self.assertEqual(published_tokens, tokens)
        mock_handle_failure.assert_not_called()

    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.handle_failure")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_commit_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.prepare_branch_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.activate_new_hotfix_tasks")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_branch_head")
    @patch("buildscripts.copybara.sync_repo_with_copybara.fetch_remote_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_local_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_mongodb_bot_gitconfig")
    @patch("buildscripts.copybara.sync_repo_with_copybara.ensure_copybara_checkout_and_image")
    @patch("buildscripts.copybara.sync_repo_with_copybara.read_config_file")
    @patch("buildscripts.copybara.sync_repo_with_copybara.os.getcwd")
    def test_main_runs_prod_branch_request_against_test_repo_with_local_config(
        self,
        mock_getcwd,
        mock_read_config_file,
        mock_ensure_copybara_checkout_and_image,
        mock_create_mongodb_bot_gitconfig,
        mock_get_copybara_tokens,
        mock_get_local_copybara_config_bundle,
        mock_fetch_remote_copybara_config_bundle,
        mock_get_remote_branch_head,
        mock_activate_new_hotfix_tasks,
        mock_prepare_branch_sync,
        mock_run_branch_commit_sync,
        mock_handle_failure,
        mock_ensure_generated_copybara_evergreen_is_current,
    ):
        mock_read_config_file.return_value = {
            "project": sync_repo_with_copybara.EXPECTED_EVERGREEN_PROJECT,
        }
        mock_getcwd.return_value = "/repo"
        tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        mock_get_copybara_tokens.return_value = tokens
        mock_get_remote_branch_head.return_value = "mastersha123"

        with tempfile.TemporaryDirectory() as tmpdir:
            base_config_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                base_config_path,
                prod_url=sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                test_url=sync_repo_with_copybara.TEST_REPO_URL,
            )
            fragment_path = Path(tmpdir) / "master.sky"
            fragment_path.write_text('sync_branch("master")\n')
            config_bundle = sync_repo_with_copybara.CopybaraConfigBundle(
                config_sha="local",
                bundle_dir=Path(tmpdir),
                base_config_path=base_config_path,
                path_rules_path=Path(tmpdir) / "copybara_path_rules.json",
                path_rules_module_path=Path(tmpdir) / "copybara_path_rules.bara.sky",
                branch_to_fragment={"master": fragment_path},
            )
            mock_get_local_copybara_config_bundle.return_value = config_bundle
            prepared_sync = sync_repo_with_copybara.PreparedBranchSync(
                branch="master",
                source_ref="mastersha123",
                config_sha="local",
                workflow_name="prod_master",
                config_file=Path(tmpdir) / "generated.sky",
                preview_dir=Path(tmpdir) / "preview",
                docker_command=("echo",),
            )
            mock_prepare_branch_sync.return_value = prepared_sync
            mock_run_branch_commit_sync.return_value = (
                sync_repo_with_copybara.BranchCommitSyncResult(branch="master")
            )

            argv = [
                "buildscripts/copybara/sync_repo_with_copybara.py",
                "--workflow=prod",
                "--prod-destination=test",
                "--branches=master",
            ]
            with patch.object(sys, "argv", argv):
                sync_repo_with_copybara.main()

        authenticated_source_url = "https://x-access-token:source-token@github.com/10gen/mongo.git"
        authenticated_test_url = (
            "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git"
        )
        prepare_call = mock_prepare_branch_sync.call_args.kwargs
        self.assertEqual(prepare_call["branch"], "master")
        self.assertEqual(prepare_call["workflow"], "prod")
        self.assertEqual(prepare_call["source_ref"], "mastersha123")
        self.assertEqual(prepare_call["config_bundle"], config_bundle)
        self.assertEqual(prepare_call["prod_url_override"], authenticated_test_url)
        mock_get_local_copybara_config_bundle.assert_called_once_with("/repo")
        mock_fetch_remote_copybara_config_bundle.assert_not_called()
        mock_get_remote_branch_head.assert_called_once_with(authenticated_source_url, "master")
        mock_ensure_generated_copybara_evergreen_is_current.assert_called_once_with(Path(tmpdir))
        mock_activate_new_hotfix_tasks.assert_not_called()
        mock_run_branch_commit_sync.assert_called_once_with(prepared_sync, tokens)
        mock_handle_failure.assert_not_called()

    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.publish_release_tag")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_commit_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.prepare_branch_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.activate_new_hotfix_tasks")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_commit")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_origin")
    @patch("buildscripts.copybara.sync_repo_with_copybara.fetch_remote_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_mongodb_bot_gitconfig")
    @patch("buildscripts.copybara.sync_repo_with_copybara.ensure_copybara_checkout_and_image")
    @patch("buildscripts.copybara.sync_repo_with_copybara.read_config_file")
    @patch("buildscripts.copybara.sync_repo_with_copybara.os.getcwd")
    def test_main_runs_prod_release_tag_sync_and_publishes_tag(
        self,
        mock_getcwd,
        mock_read_config_file,
        mock_ensure_copybara_checkout_and_image,
        mock_create_mongodb_bot_gitconfig,
        mock_get_copybara_tokens,
        mock_fetch_remote_copybara_config_bundle,
        mock_get_remote_tag_origin,
        mock_get_remote_tag_commit,
        mock_activate_new_hotfix_tasks,
        mock_prepare_branch_sync,
        mock_run_branch_commit_sync,
        mock_publish_release_tag,
        mock_ensure_generated_copybara_evergreen_is_current,
    ):
        mock_read_config_file.return_value = {
            "project": sync_repo_with_copybara.EXPECTED_EVERGREEN_PROJECT,
        }
        mock_getcwd.return_value = "/repo"
        tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        mock_get_copybara_tokens.side_effect = [tokens, tokens]
        mock_get_remote_tag_origin.return_value = None
        mock_get_remote_tag_commit.return_value = "tagsha123"
        synthetic_fragment_contents = ""

        with tempfile.TemporaryDirectory() as tmpdir:
            base_config_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                base_config_path,
                prod_url=sync_repo_with_copybara.TEST_REPO_URL,
            )
            fragment_path = Path(tmpdir) / "master.sky"
            fragment_path.write_text('sync_branch("master")\n')
            mock_fetch_remote_copybara_config_bundle.return_value = (
                sync_repo_with_copybara.CopybaraConfigBundle(
                    config_sha="configsha123",
                    bundle_dir=Path(tmpdir),
                    base_config_path=base_config_path,
                    path_rules_path=Path(tmpdir) / "copybara_path_rules.json",
                    path_rules_module_path=Path(tmpdir) / "copybara_path_rules.bara.sky",
                    branch_to_fragment={"master": fragment_path},
                )
            )
            prepared_sync = sync_repo_with_copybara.PreparedBranchSync(
                branch="v8.2.7",
                source_ref="tagsha123",
                config_sha="configsha123",
                workflow_name="prod_r8.2.7",
                config_file=Path("/tmp/generated.sky"),
                preview_dir=Path("/tmp/preview"),
                docker_command=("echo",),
                release_tag="r8.2.7",
                release_source_commit="tagsha123",
            )
            mock_prepare_branch_sync.return_value = prepared_sync
            mock_run_branch_commit_sync.return_value = (
                sync_repo_with_copybara.BranchCommitSyncResult(branch="v8.2.7")
            )

            argv = [
                "buildscripts/copybara/sync_repo_with_copybara.py",
                "--workflow=prod",
                "--branches=r8.2.7",
            ]
            with patch.object(sys, "argv", argv):
                sync_repo_with_copybara.main()
            synthetic_fragment_contents = mock_prepare_branch_sync.call_args.kwargs[
                "fragment_path"
            ].read_text()

        prepare_call = mock_prepare_branch_sync.call_args.kwargs
        self.assertEqual(prepare_call["branch"], "v8.2.7")
        self.assertEqual(prepare_call["source_ref"], "tagsha123")
        self.assertEqual(prepare_call["release_tag"], "r8.2.7")
        self.assertEqual(prepare_call["release_source_commit"], "tagsha123")
        self.assertIn('sync_tag("r8.2.7")', synthetic_fragment_contents)
        mock_fetch_remote_copybara_config_bundle.assert_called_once_with(
            "/repo",
            "https://x-access-token:source-token@github.com/10gen/mongo.git",
            enforce_current_python_helpers=True,
        )
        mock_ensure_generated_copybara_evergreen_is_current.assert_called_once_with(Path(tmpdir))
        mock_run_branch_commit_sync.assert_called_once_with(prepared_sync, tokens)
        mock_publish_release_tag.assert_called_once_with(prepared_sync, tokens)
        mock_activate_new_hotfix_tasks.assert_called_once()
        mock_get_remote_tag_origin.assert_called_once_with(
            "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git",
            "r8.2.7",
        )

    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.prepare_branch_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_origin")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_commit")
    @patch("buildscripts.copybara.sync_repo_with_copybara.fetch_remote_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_mongodb_bot_gitconfig")
    @patch("buildscripts.copybara.sync_repo_with_copybara.ensure_copybara_checkout_and_image")
    @patch("buildscripts.copybara.sync_repo_with_copybara.read_config_file")
    @patch("buildscripts.copybara.sync_repo_with_copybara.os.getcwd")
    def test_main_rejects_existing_public_release_tag_with_different_origin(
        self,
        mock_getcwd,
        mock_read_config_file,
        mock_ensure_copybara_checkout_and_image,
        mock_create_mongodb_bot_gitconfig,
        mock_get_copybara_tokens,
        mock_fetch_remote_copybara_config_bundle,
        mock_get_remote_tag_commit,
        mock_get_remote_tag_origin,
        mock_prepare_branch_sync,
        mock_ensure_generated_copybara_evergreen_is_current,
    ):
        mock_read_config_file.return_value = {
            "project": sync_repo_with_copybara.EXPECTED_EVERGREEN_PROJECT,
        }
        mock_getcwd.return_value = "/repo"
        tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        mock_get_copybara_tokens.return_value = tokens
        mock_get_remote_tag_commit.return_value = "tagsha123"
        mock_get_remote_tag_origin.return_value = sync_repo_with_copybara.RemoteTagOrigin(
            destination_commit="publicsha123",
            git_origin_rev_id="othersha456",
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            base_config_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                base_config_path,
                prod_url=sync_repo_with_copybara.TEST_REPO_URL,
            )
            fragment_path = Path(tmpdir) / "master.sky"
            fragment_path.write_text('sync_branch("master")\n')
            mock_fetch_remote_copybara_config_bundle.return_value = (
                sync_repo_with_copybara.CopybaraConfigBundle(
                    config_sha="configsha123",
                    bundle_dir=Path(tmpdir),
                    base_config_path=base_config_path,
                    path_rules_path=Path(tmpdir) / "copybara_path_rules.json",
                    path_rules_module_path=Path(tmpdir) / "copybara_path_rules.bara.sky",
                    branch_to_fragment={"master": fragment_path},
                )
            )

            argv = [
                "buildscripts/copybara/sync_repo_with_copybara.py",
                "--workflow=prod",
                "--branches=r8.2.7",
            ]
            with patch.object(sys, "argv", argv):
                with self.assertRaises(SystemExit):
                    sync_repo_with_copybara.main()

        mock_prepare_branch_sync.assert_not_called()
        mock_ensure_generated_copybara_evergreen_is_current.assert_called_once_with(Path(tmpdir))
        mock_get_remote_tag_origin.assert_called_once_with(
            "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git",
            "r8.2.7",
        )

    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.publish_release_tag")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_commit_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.prepare_branch_sync")
    @patch("buildscripts.copybara.sync_repo_with_copybara.activate_new_hotfix_tasks")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_origin")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_commit")
    @patch("buildscripts.copybara.sync_repo_with_copybara.fetch_remote_copybara_config_bundle")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.create_mongodb_bot_gitconfig")
    @patch("buildscripts.copybara.sync_repo_with_copybara.ensure_copybara_checkout_and_image")
    @patch("buildscripts.copybara.sync_repo_with_copybara.read_config_file")
    @patch("buildscripts.copybara.sync_repo_with_copybara.os.getcwd")
    def test_main_skips_existing_public_release_tag_with_matching_origin(
        self,
        mock_getcwd,
        mock_read_config_file,
        mock_ensure_copybara_checkout_and_image,
        mock_create_mongodb_bot_gitconfig,
        mock_get_copybara_tokens,
        mock_fetch_remote_copybara_config_bundle,
        mock_get_remote_tag_commit,
        mock_get_remote_tag_origin,
        mock_activate_new_hotfix_tasks,
        mock_prepare_branch_sync,
        mock_run_branch_commit_sync,
        mock_publish_release_tag,
        mock_ensure_generated_copybara_evergreen_is_current,
    ):
        mock_read_config_file.return_value = {
            "project": sync_repo_with_copybara.EXPECTED_EVERGREEN_PROJECT,
        }
        mock_getcwd.return_value = "/repo"
        tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        mock_get_copybara_tokens.return_value = tokens
        mock_get_remote_tag_commit.return_value = "tagsha123"
        mock_get_remote_tag_origin.return_value = sync_repo_with_copybara.RemoteTagOrigin(
            destination_commit="publicsha123",
            git_origin_rev_id="tagsha123",
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            base_config_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                base_config_path,
                prod_url=sync_repo_with_copybara.TEST_REPO_URL,
            )
            fragment_path = Path(tmpdir) / "master.sky"
            fragment_path.write_text('sync_branch("master")\n')
            mock_fetch_remote_copybara_config_bundle.return_value = (
                sync_repo_with_copybara.CopybaraConfigBundle(
                    config_sha="configsha123",
                    bundle_dir=Path(tmpdir),
                    base_config_path=base_config_path,
                    path_rules_path=Path(tmpdir) / "copybara_path_rules.json",
                    path_rules_module_path=Path(tmpdir) / "copybara_path_rules.bara.sky",
                    branch_to_fragment={"master": fragment_path},
                )
            )

            argv = [
                "buildscripts/copybara/sync_repo_with_copybara.py",
                "--workflow=prod",
                "--branches=r8.2.7",
            ]
            stdout = io.StringIO()
            with patch.object(sys, "argv", argv), redirect_stdout(stdout):
                sync_repo_with_copybara.main()

        self.assertIn("already synced", stdout.getvalue())
        mock_prepare_branch_sync.assert_not_called()
        mock_run_branch_commit_sync.assert_not_called()
        mock_publish_release_tag.assert_not_called()
        mock_activate_new_hotfix_tasks.assert_not_called()
        mock_ensure_generated_copybara_evergreen_is_current.assert_called_once_with(Path(tmpdir))
        mock_get_remote_tag_origin.assert_called_once_with(
            "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git",
            "r8.2.7",
        )

    def test_rewrite_copybara_config_refreshes_existing_tokens_and_prefix(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            config_path = Path(tmpdir) / "copy.bara.sky"
            config_path.write_text(
                'source_url = "https://x-access-token:old@github.com/10gen/mongo.git"\n'
                'prod_url = "https://github.com/mongodb/mongo.git"\n'
                'test_url = "https://x-access-token:old@github.com/10gen/mongo-copybara.git"\n'
                'test_branch_prefix = "copybara_test_branch"\n'
                "\n"
                "def make_workflow(workflow_name, destination_url, source_ref, destination_ref, branch_excluded_files):\n"
                "    pass\n"
                "\n"
                "def sync_branch(branch_name, branch_excluded_files = []):\n"
                '    make_workflow("prod_" + branch_name, prod_url, branch_name, branch_name, branch_excluded_files)\n'
                "    make_workflow(\n"
                '        "test_" + branch_name,\n'
                "        test_url,\n"
                "        branch_name,\n"
                '        test_branch_prefix + "_" + branch_name,\n'
                "        branch_excluded_files,\n"
                "    )\n"
            )

            sync_repo_with_copybara.rewrite_copybara_config(
                config_file=config_path,
                tokens_map={
                    sync_repo_with_copybara.SOURCE_REPO_URL: "new-source-token",
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "new-prod-token",
                    sync_repo_with_copybara.TEST_REPO_URL: "new-test-token",
                },
                test_branch_prefix="copybara_test_branch_patch456",
                source_refs={"v8.2": "deadbeef123"},
            )

            rewritten = config_path.read_text()
            self.assertIn(
                "https://x-access-token:new-source-token@github.com/10gen/mongo.git",
                rewritten,
            )
            self.assertIn(
                "https://x-access-token:new-prod-token@github.com/mongodb/mongo.git",
                rewritten,
            )
            self.assertIn(
                "https://x-access-token:new-test-token@github.com/10gen/mongo-copybara.git",
                rewritten,
            )
            self.assertIn('test_branch_prefix = "copybara_test_branch_patch456"', rewritten)
            self.assertIn('source_refs = {"v8.2": "deadbeef123"}', rewritten)
            self.assertIn("source_ref = source_refs.get(branch_name, branch_name)", rewritten)

    def test_rewrite_copybara_config_can_switch_source_url_to_local_mirror(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            config_path = Path(tmpdir) / "copy.bara.sky"
            config_path.write_text(
                'source_url = "https://github.com/10gen/mongo.git"\n'
                'prod_url = "https://github.com/mongodb/mongo.git"\n'
                'test_url = "https://github.com/10gen/mongo-copybara.git"\n'
                'test_branch_prefix = "copybara_test_branch"\n'
                "source_refs = {}\n"
                "\n"
                "def make_workflow(workflow_name, destination_url, source_ref, destination_ref):\n"
                "    pass\n"
                "\n"
                "def sync_branch(branch_name, evergreen_activate = False):\n"
                "    source_ref = source_refs.get(branch_name, branch_name)\n"
                '    make_workflow("prod_" + branch_name, prod_url, source_ref, branch_name)\n'
                "    make_workflow(\n"
                '        "test_" + branch_name,\n'
                "        test_url,\n"
                "        source_ref,\n"
                '        test_branch_prefix + "_" + branch_name,\n'
                "    )\n"
            )

            sync_repo_with_copybara.rewrite_copybara_config(
                config_file=config_path,
                tokens_map={
                    sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                    sync_repo_with_copybara.TEST_REPO_URL: "test-token",
                },
                source_url_override="file:///tmp/copybara-output/source-mirror.git",
            )

            rewritten = config_path.read_text()
            self.assertIn(
                'source_url = "file:///tmp/copybara-output/source-mirror.git"',
                rewritten,
            )
            self.assertNotIn("x-access-token:source-token@github.com/10gen/mongo.git", rewritten)

    def test_rewrite_copybara_config_can_switch_prod_url_to_test_repo(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            config_path = Path(tmpdir) / "copy.bara.sky"
            config_path.write_text(
                'source_url = "https://github.com/10gen/mongo.git"\n'
                'prod_url = "https://github.com/mongodb/mongo.git"\n'
                'test_url = "https://github.com/10gen/mongo-copybara.git"\n'
                'test_branch_prefix = "copybara_test_branch"\n'
                "source_refs = {}\n"
                "\n"
                "def make_workflow(workflow_name, destination_url, source_ref, destination_ref):\n"
                "    pass\n"
                "\n"
                "def sync_branch(branch_name, evergreen_activate = False):\n"
                "    source_ref = source_refs.get(branch_name, branch_name)\n"
                '    make_workflow("prod_" + branch_name, prod_url, source_ref, branch_name)\n'
                "    make_workflow(\n"
                '        "test_" + branch_name,\n'
                "        test_url,\n"
                "        source_ref,\n"
                '        test_branch_prefix + "_" + branch_name,\n'
                "    )\n"
            )

            sync_repo_with_copybara.rewrite_copybara_config(
                config_file=config_path,
                tokens_map={
                    sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                    sync_repo_with_copybara.TEST_REPO_URL: "test-token",
                },
                prod_url_override=sync_repo_with_copybara.TEST_REPO_URL,
            )

            rewritten = config_path.read_text()
            self.assertIn(
                'prod_url = "https://x-access-token:test-token@github.com/10gen/mongo-copybara.git"',
                rewritten,
            )
            self.assertNotIn(
                'prod_url = "https://x-access-token:prod-token@github.com/mongodb/mongo.git"',
                rewritten,
            )

    @patch("buildscripts.copybara.sync_repo_with_copybara.validate_preview_exclusions")
    @patch("buildscripts.copybara.sync_repo_with_copybara.validate_sync_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_run_branch_dry_run_refreshes_tokens_on_auth_failure(
        self,
        mock_run_command,
        mock_get_copybara_tokens,
        mock_rewrite_copybara_config,
        mock_validate_sync_config,
        mock_validate_preview_exclusions,
    ):
        with tempfile.TemporaryDirectory() as tmpdir:
            preview_dir = Path(tmpdir) / "preview"
            preview_dir.mkdir()
            stale_file = preview_dir / "stale.txt"
            stale_file.write_text("stale")
            sync = sync_repo_with_copybara.PreparedBranchSync(
                branch="v8.2",
                source_ref="deadbeef123",
                config_sha="local",
                workflow_name="prod_v8.2",
                config_file=Path(tmpdir) / "copy.bara.sky",
                preview_dir=preview_dir,
                docker_command=("echo", "copybara"),
                expansions={"version_id": "patch123"},
            )
            auth_error = subprocess.CalledProcessError(
                128,
                "copybara",
                output=(
                    "remote: Invalid username or token. Password authentication is not supported "
                    "for Git operations.\n"
                    "fatal: Authentication failed for 'https://github.com/10gen/mongo-copybara.git/'"
                ),
            )
            mock_run_command.side_effect = [auth_error, ""]
            refreshed_tokens = {
                sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                sync_repo_with_copybara.TEST_REPO_URL: "test-token",
            }
            mock_get_copybara_tokens.return_value = refreshed_tokens

            result = sync_repo_with_copybara.run_branch_dry_run(sync)

            self.assertEqual(result, sync_repo_with_copybara.BranchDryRunResult(branch="v8.2"))
            self.assertFalse(stale_file.exists())
            self.assertEqual(mock_run_command.call_count, 2)
            self.assertTrue(
                all(
                    call_args.kwargs["log_output"] is False
                    and call_args.kwargs["log_command"] is False
                    for call_args in mock_run_command.call_args_list
                )
            )
            mock_get_copybara_tokens.assert_called_once_with(sync.expansions)
            mock_rewrite_copybara_config.assert_called_once_with(sync.config_file, refreshed_tokens)
            mock_validate_sync_config.assert_called()
            mock_validate_preview_exclusions.assert_called_once_with(sync)

    @patch("buildscripts.copybara.sync_repo_with_copybara.validate_preview_exclusions")
    @patch("buildscripts.copybara.sync_repo_with_copybara.validate_sync_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_run_branch_dry_run_refreshes_tokens_on_validation_auth_failure(
        self,
        mock_run_command,
        mock_get_copybara_tokens,
        mock_rewrite_copybara_config,
        mock_validate_sync_config,
        mock_validate_preview_exclusions,
    ):
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="deadbeef123",
            config_sha="local",
            workflow_name="prod_v8.2",
            config_file=root / "copy.bara.sky",
            preview_dir=root / "preview",
            docker_command=("echo", "copybara"),
            expansions={"version_id": "patch123"},
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.SOURCE_REPO_URL,
                        "old-source-token",
                    ),
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="deadbeef123",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                        "old-prod-token",
                    ),
                    repo_name="mongodb/mongo",
                    branch="v8.2",
                ),
            ),
        )
        auth_output = (
            "remote: Invalid username or token. Password authentication is not supported "
            "for Git operations.\n"
            "fatal: Authentication failed for 'https://github.com/10gen/mongo.git/'"
        )
        auth_error = sync_repo_with_copybara.BranchSyncError(
            branch="v8.2",
            stage="config validation",
            message="Copybara exited with code 128",
            output_logs=auth_output,
        )
        refreshed_tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "fresh-source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "fresh-prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "fresh-test-token",
        }
        mock_get_copybara_tokens.return_value = refreshed_tokens
        seen_source_urls = []

        def validate_side_effect(validation_sync):
            seen_source_urls.append(validation_sync.copybara_config.source.git_url)
            if len(seen_source_urls) == 1:
                raise auth_error

        mock_validate_sync_config.side_effect = validate_side_effect
        mock_run_command.return_value = ""

        result = sync_repo_with_copybara.run_branch_dry_run(sync)

        self.assertEqual(result, sync_repo_with_copybara.BranchDryRunResult(branch="v8.2"))
        self.assertEqual(
            seen_source_urls,
            [
                sync_repo_with_copybara.auth_github_url(
                    sync_repo_with_copybara.SOURCE_REPO_URL,
                    "old-source-token",
                ),
                sync_repo_with_copybara.auth_github_url(
                    sync_repo_with_copybara.SOURCE_REPO_URL,
                    "fresh-source-token",
                ),
            ],
        )
        mock_get_copybara_tokens.assert_called_once_with(sync.expansions)
        mock_rewrite_copybara_config.assert_called_once_with(sync.config_file, refreshed_tokens)
        mock_run_command.assert_called_once()
        self.assertIs(mock_run_command.call_args.kwargs["log_output"], False)
        self.assertIs(mock_run_command.call_args.kwargs["log_command"], False)
        mock_validate_preview_exclusions.assert_called_once()

    def test_get_filesystem_free_space_uses_existing_parent(self):
        class DiskUsage:
            total = 4096
            free = 2048

        with tempfile.TemporaryDirectory() as tmpdir:
            probe_path = Path(tmpdir) / "missing" / "child"
            with patch(
                "buildscripts.copybara.sync_repo_with_copybara.shutil.disk_usage",
                return_value=DiskUsage(),
            ) as mock_disk_usage:
                free_space = sync_repo_with_copybara.get_filesystem_free_space(probe_path)

            self.assertEqual(free_space, "2.0 KiB available of 4.0 KiB")
            mock_disk_usage.assert_called_once_with(Path(tmpdir))

    def test_reset_preview_dir_uses_container_cleanup_when_host_cleanup_fails(self):
        tmpdir = Path(tempfile.mkdtemp())
        try:
            preview_dir = tmpdir / "preview"
            preview_dir.mkdir()
            stale_file = preview_dir / "stale.txt"
            stale_file.write_text("stale")
            sync = sync_repo_with_copybara.PreparedBranchSync(
                branch="v8.2",
                source_ref="deadbeef123",
                config_sha="local",
                workflow_name="prod_v8.2",
                config_file=tmpdir / "copy.bara.sky",
                preview_dir=preview_dir,
                docker_command=("echo", "copybara"),
            )

            with (
                patch("buildscripts.copybara.sync_repo_with_copybara.shutil.rmtree") as mock_rmtree,
                patch(
                    "buildscripts.copybara.sync_repo_with_copybara.run_command"
                ) as mock_run_command,
            ):
                mock_rmtree.side_effect = OSError("permission denied")

                def container_cleanup_side_effect(command):
                    self.assertIn(sync_repo_with_copybara.shell_quote("--entrypoint"), command)
                    self.assertIn(sync_repo_with_copybara.shell_quote("/bin/sh"), command)
                    self.assertIn(sync_repo_with_copybara.COPYBARA_DOCKER_IMAGE, command)
                    stale_file.unlink()
                    return ""

                mock_run_command.side_effect = container_cleanup_side_effect

                sync_repo_with_copybara.reset_preview_dir(sync, reason="unit test")

                self.assertFalse(stale_file.exists())
                self.assertTrue(
                    (preview_dir / sync_repo_with_copybara.COPYBARA_WORKDIR_NAME).is_dir()
                )
                mock_rmtree.assert_called_once_with(preview_dir)
                mock_run_command.assert_called_once()
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_git_remote_command")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_prepare_local_source_mirror_fetches_source_and_returns_local_sync(
        self, mock_run_command, mock_run_git_remote_command
    ):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = Path(tmpdir)
            source_mirror_dir = (
                tmpdir_path
                / "copybara-output"
                / sync_repo_with_copybara.COPYBARA_SOURCE_MIRROR_DIR_NAME
            )
            sync = sync_repo_with_copybara.PreparedBranchSync(
                branch="v8.2",
                source_ref="headsha",
                config_sha="local",
                workflow_name="prod_v8.2",
                config_file=tmpdir_path / "copy.bara.sky",
                preview_dir=tmpdir_path / "preview",
                docker_command=("echo", "copybara"),
                copybara_output_dir=tmpdir_path / "copybara-output",
                source_mirror_dir=source_mirror_dir,
                copybara_config=sync_repo_with_copybara.CopybaraConfig(
                    source=sync_repo_with_copybara.CopybaraRepoConfig(
                        git_url=sync_repo_with_copybara.auth_github_url(
                            sync_repo_with_copybara.SOURCE_REPO_URL,
                            "source-token",
                        ),
                        repo_name="10gen/mongo",
                        branch="v8.2",
                        ref="headsha",
                    ),
                    destination=sync_repo_with_copybara.CopybaraRepoConfig(
                        git_url="https://example.com/destination.git",
                        repo_name="mongodb/mongo",
                        branch="v8.2",
                    ),
                ),
            )

            local_sync = sync_repo_with_copybara.prepare_local_source_mirror(sync)

            self.assertEqual(
                local_sync.copybara_config.source.git_url, source_mirror_dir.as_posix()
            )
            self.assertEqual(local_sync.source_ref, "copybara_sync_source_v8_2")
            self.assertEqual(local_sync.copybara_config.source.ref, "copybara_sync_source_v8_2")
            self.assertEqual(
                local_sync.copybara_source_url,
                sync_repo_with_copybara.get_copybara_source_mirror_url(),
            )
            self.assertEqual(local_sync.copybara_config.source.repo_name, "10gen/mongo")
            self.assertTrue(source_mirror_dir.parent.is_dir())
            self.assertIn(
                f"git init --bare {sync_repo_with_copybara.shell_quote(source_mirror_dir)}",
                mock_run_command.call_args_list[0].args[0],
            )
            fetch_command = mock_run_git_remote_command.call_args.args[0]
            self.assertIn(sync_repo_with_copybara.shell_quote(source_mirror_dir), fetch_command)
            self.assertIn("--tags", fetch_command)
            self.assertIn("+headsha:refs/heads/copybara_sync_source_v8_2", fetch_command)
            self.assertNotIn("+refs/heads/v8.2:refs/heads/v8.2", fetch_command)
            self.assertIn(
                sync_repo_with_copybara.auth_github_url(
                    sync_repo_with_copybara.SOURCE_REPO_URL,
                    "source-token",
                ),
                fetch_command,
            )
            symbolic_ref_command = mock_run_command.call_args_list[1].args[0]
            self.assertIn("symbolic-ref", symbolic_ref_command)
            self.assertIn("HEAD", symbolic_ref_command)
            self.assertIn("refs/heads/copybara_sync_source_v8_2", symbolic_ref_command)

    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_run_branch_migrate_refreshes_tokens_on_auth_failure(
        self,
        mock_run_command,
        mock_get_copybara_tokens,
        mock_rewrite_copybara_config,
    ):
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="deadbeef123",
            config_sha="local",
            workflow_name="prod_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo", "copybara"),
            expansions={"version_id": "patch123"},
        )
        auth_error = subprocess.CalledProcessError(
            128,
            "copybara",
            output=(
                "remote: Invalid username or token. Password authentication is not supported "
                "for Git operations.\n"
                "fatal: Authentication failed for 'https://github.com/10gen/mongo-copybara.git/'"
            ),
        )
        mock_run_command.side_effect = [auth_error, ""]
        refreshed_tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "test-token",
        }
        mock_get_copybara_tokens.return_value = refreshed_tokens

        sync_repo_with_copybara.run_branch_migrate(sync)

        self.assertEqual(mock_run_command.call_count, 2)
        self.assertTrue(
            all(
                call_args.kwargs["log_output"] is False and call_args.kwargs["log_command"] is False
                for call_args in mock_run_command.call_args_list
            )
        )
        mock_get_copybara_tokens.assert_called_once_with(sync.expansions)
        mock_rewrite_copybara_config.assert_called_once_with(sync.config_file, refreshed_tokens)

    def make_release_tag_sync(self) -> sync_repo_with_copybara.PreparedBranchSync:
        return sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2.7",
            source_ref="tagsha123",
            config_sha="local",
            workflow_name="prod_r8.2.7",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo",),
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="https://example.com/source.git",
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="tagsha123",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="https://example.com/destination.git",
                    repo_name="10gen/mongo-copybara",
                    branch="v8.2.7",
                ),
            ),
            release_tag="r8.2.7",
            release_source_commit="tagsha123",
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.resolve_release_tag_destination_commit_from_repos"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_origin")
    def test_publish_release_tag_skips_existing_tag_with_matching_origin(
        self,
        mock_get_remote_tag_origin,
        mock_resolve_release_tag_destination_commit_from_repos,
        mock_run_command,
    ):
        sync = self.make_release_tag_sync()
        mock_get_remote_tag_origin.return_value = sync_repo_with_copybara.RemoteTagOrigin(
            destination_commit="publicsha123",
            git_origin_rev_id="tagsha123",
        )

        stdout = io.StringIO()
        with redirect_stdout(stdout):
            sync_repo_with_copybara.publish_release_tag(sync, {})

        self.assertIn("already synced", stdout.getvalue())
        mock_resolve_release_tag_destination_commit_from_repos.assert_not_called()
        mock_run_command.assert_not_called()

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.resolve_release_tag_destination_commit_from_repos"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_origin")
    def test_publish_release_tag_rejects_existing_tag_with_different_origin(
        self,
        mock_get_remote_tag_origin,
        mock_resolve_release_tag_destination_commit_from_repos,
        mock_run_command,
    ):
        sync = self.make_release_tag_sync()
        mock_get_remote_tag_origin.return_value = sync_repo_with_copybara.RemoteTagOrigin(
            destination_commit="publicsha123",
            git_origin_rev_id="othersha456",
        )

        with self.assertRaises(sync_repo_with_copybara.BranchSyncError) as raised:
            sync_repo_with_copybara.publish_release_tag(sync, {})

        self.assertIn("expected tagsha123", str(raised.exception))
        mock_resolve_release_tag_destination_commit_from_repos.assert_not_called()
        mock_run_command.assert_not_called()

    @patch("buildscripts.copybara.sync_repo_with_copybara.shutil.rmtree")
    @patch("buildscripts.copybara.sync_repo_with_copybara.tempfile.mkdtemp")
    @patch(
        "buildscripts.copybara.sync_repo_with_copybara.resolve_release_tag_destination_commit_from_repos"
    )
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_tag_origin")
    def test_publish_release_tag_pushes_when_tag_is_absent(
        self,
        mock_get_remote_tag_origin,
        mock_run_command,
        mock_resolve_release_tag_destination_commit_from_repos,
        mock_mkdtemp,
        mock_rmtree,
    ):
        sync = self.make_release_tag_sync()
        mock_get_remote_tag_origin.return_value = None
        mock_resolve_release_tag_destination_commit_from_repos.return_value = "publicsha123"
        source_repo_dir = Path("/tmp/copybara-release-tag-source")
        destination_repo_dir = Path("/tmp/copybara-release-tag-destination")
        mock_mkdtemp.side_effect = [str(source_repo_dir), str(destination_repo_dir)]

        sync_repo_with_copybara.publish_release_tag(sync, {})

        mock_run_command.assert_has_calls(
            [
                call(
                    "git clone --filter=blob:none --no-checkout "
                    f"{sync_repo_with_copybara.shell_quote('https://example.com/source.git')} "
                    f"{sync_repo_with_copybara.shell_quote(source_repo_dir)}"
                ),
                call(
                    " ".join(
                        [
                            "git",
                            "clone",
                            "--filter=blob:none",
                            "--no-checkout",
                            "--single-branch",
                            "-b",
                            sync_repo_with_copybara.shell_quote("v8.2.7"),
                            sync_repo_with_copybara.shell_quote(
                                "https://example.com/destination.git"
                            ),
                            sync_repo_with_copybara.shell_quote(destination_repo_dir),
                        ]
                    )
                ),
                call(
                    " ".join(
                        [
                            "git",
                            "-C",
                            sync_repo_with_copybara.shell_quote(destination_repo_dir),
                            "push",
                            sync_repo_with_copybara.shell_quote(
                                "https://example.com/destination.git"
                            ),
                            sync_repo_with_copybara.shell_quote("publicsha123:refs/tags/r8.2.7"),
                        ]
                    )
                ),
            ]
        )
        mock_resolve_release_tag_destination_commit_from_repos.assert_called_once_with(
            sync, source_repo_dir, destination_repo_dir
        )
        mock_rmtree.assert_has_calls(
            [
                call(source_repo_dir, ignore_errors=True),
                call(destination_repo_dir, ignore_errors=True),
            ]
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_migrate")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_dry_run")
    @patch("buildscripts.copybara.sync_repo_with_copybara.reset_preview_dir")
    @patch("buildscripts.copybara.sync_repo_with_copybara.prepare_local_source_mirror")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_pending_source_commits")
    def test_run_branch_commit_sync_uses_local_source_mirror(
        self,
        mock_list_pending_source_commits,
        mock_rewrite_copybara_config,
        mock_get_copybara_tokens,
        mock_prepare_local_source_mirror,
        mock_reset_preview_dir,
        mock_run_branch_dry_run,
        mock_run_branch_migrate,
    ):
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="headsha",
            config_sha="local",
            workflow_name="prod_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo", "copybara"),
            expansions={"version_id": "patch123"},
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.SOURCE_REPO_URL,
                        "initial-source-token",
                    ),
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="headsha",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                        "initial-prod-token",
                    ),
                    repo_name="mongodb/mongo",
                    branch="v8.2",
                ),
            ),
        )
        local_sync = sync_repo_with_copybara.PreparedBranchSync(
            branch=sync.branch,
            source_ref=sync.source_ref,
            config_sha=sync.config_sha,
            workflow_name=sync.workflow_name,
            config_file=sync.config_file,
            preview_dir=sync.preview_dir,
            docker_command=sync.docker_command,
            expansions=sync.expansions,
            copybara_source_url="file:///tmp/copybara-output/source-mirror.git",
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="/tmp/source-mirror.git",
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="headsha",
                ),
                destination=sync.copybara_config.destination,
            ),
        )
        initial_tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "initial-source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "initial-prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "initial-test-token",
        }
        mock_prepare_local_source_mirror.return_value = local_sync
        mock_list_pending_source_commits.return_value = [make_source_commit("commit1")]
        mock_run_branch_dry_run.return_value = sync_repo_with_copybara.BranchDryRunResult(
            branch="v8.2"
        )

        sync_repo_with_copybara.run_branch_commit_sync(sync, initial_tokens)

        mock_prepare_local_source_mirror.assert_called_once_with(sync)
        mock_list_pending_source_commits.assert_called_once_with(local_sync)
        self.assertEqual(
            mock_rewrite_copybara_config.call_args.kwargs["source_url_override"],
            "file:///tmp/copybara-output/source-mirror.git",
        )
        self.assertEqual(
            mock_run_branch_dry_run.call_args.args[0].copybara_config.source.git_url,
            "/tmp/source-mirror.git",
        )
        mock_get_copybara_tokens.assert_not_called()
        self.assertEqual(mock_reset_preview_dir.call_count, 2)
        mock_run_branch_migrate.assert_called_once()

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_migrate")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_dry_run")
    @patch("buildscripts.copybara.sync_repo_with_copybara.reset_preview_dir")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_pending_source_commits")
    def test_run_branch_commit_sync_reuses_tokens_across_pending_commits(
        self,
        mock_list_pending_source_commits,
        mock_rewrite_copybara_config,
        mock_get_copybara_tokens,
        mock_reset_preview_dir,
        mock_run_branch_dry_run,
        mock_run_branch_migrate,
    ):
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="headsha",
            config_sha="local",
            workflow_name="prod_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo", "copybara"),
            expansions={"version_id": "patch123"},
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.SOURCE_REPO_URL,
                        "initial-source-token",
                    ),
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="headsha",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                        "initial-prod-token",
                    ),
                    repo_name="mongodb/mongo",
                    branch="v8.2",
                ),
            ),
        )
        initial_tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "initial-source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "initial-prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "initial-test-token",
        }
        mock_list_pending_source_commits.return_value = [
            make_source_commit("commit1"),
            make_source_commit("commit2"),
        ]

        def dry_run_side_effect(commit_sync, on_auth_refresh=None):
            self.assertIsNotNone(on_auth_refresh)
            self.assertEqual(commit_sync.source_ref, commit_sync.copybara_config.source.ref)
            self.assertEqual(
                commit_sync.copybara_config.source.git_url,
                sync_repo_with_copybara.auth_github_url(
                    sync_repo_with_copybara.SOURCE_REPO_URL,
                    "initial-source-token",
                ),
            )
            self.assertEqual(
                commit_sync.copybara_config.destination.git_url,
                sync_repo_with_copybara.auth_github_url(
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                    "initial-prod-token",
                ),
            )
            return sync_repo_with_copybara.BranchDryRunResult(branch=commit_sync.branch)

        mock_run_branch_dry_run.side_effect = dry_run_side_effect

        stdout = io.StringIO()
        with redirect_stdout(stdout):
            result = sync_repo_with_copybara.run_branch_commit_sync(sync, initial_tokens)

        self.assertIn(
            "[v8.2] Commit 1/2 source: commit1 | 2026-05-01T00:00:00+00:00 | "
            "Test User <test@example.com> | Subject for commit1",
            stdout.getvalue(),
        )
        self.assertIn(
            "[v8.2] Commit 2/2 source: commit2 | 2026-05-01T00:00:00+00:00 | "
            "Test User <test@example.com> | Subject for commit2",
            stdout.getvalue(),
        )
        self.assertEqual(
            result,
            sync_repo_with_copybara.BranchCommitSyncResult(
                branch="v8.2",
                discovered_commits=2,
                migrated_commits=2,
                noop_commits=0,
            ),
        )
        self.assertEqual(
            [call_args.args[0].source_ref for call_args in mock_run_branch_dry_run.call_args_list],
            ["commit1", "commit2"],
        )
        self.assertEqual(
            [call_args.args[0].source_ref for call_args in mock_run_branch_migrate.call_args_list],
            ["commit1", "commit2"],
        )
        self.assertEqual(
            [
                call_args.kwargs["source_refs"]
                for call_args in mock_rewrite_copybara_config.call_args_list
            ],
            [
                {"v8.2": "commit1"},
                {"v8.2": "commit2"},
            ],
        )
        self.assertEqual(
            [call_args.args[1] for call_args in mock_rewrite_copybara_config.call_args_list],
            [initial_tokens, initial_tokens],
        )
        mock_get_copybara_tokens.assert_not_called()
        self.assertEqual(mock_reset_preview_dir.call_count, 4)

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_migrate")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_dry_run")
    @patch("buildscripts.copybara.sync_repo_with_copybara.reset_preview_dir")
    @patch("buildscripts.copybara.sync_repo_with_copybara.should_refresh_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_pending_source_commits")
    def test_run_branch_commit_sync_refreshes_tokens_when_stale(
        self,
        mock_list_pending_source_commits,
        mock_rewrite_copybara_config,
        mock_get_copybara_tokens,
        mock_should_refresh_copybara_tokens,
        mock_reset_preview_dir,
        mock_run_branch_dry_run,
        mock_run_branch_migrate,
    ):
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="headsha",
            config_sha="local",
            workflow_name="prod_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo", "copybara"),
            expansions={"version_id": "patch123"},
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.SOURCE_REPO_URL,
                        "initial-source-token",
                    ),
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="headsha",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                        "initial-prod-token",
                    ),
                    repo_name="mongodb/mongo",
                    branch="v8.2",
                ),
            ),
        )
        initial_tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "initial-source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "initial-prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "initial-test-token",
        }
        fresh_tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "fresh-source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "fresh-prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "fresh-test-token",
        }
        mock_list_pending_source_commits.return_value = [
            make_source_commit("commit1"),
            make_source_commit("commit2"),
        ]
        mock_should_refresh_copybara_tokens.side_effect = [False, True]
        mock_get_copybara_tokens.return_value = fresh_tokens
        mock_run_branch_dry_run.return_value = sync_repo_with_copybara.BranchDryRunResult(
            branch="v8.2"
        )

        sync_repo_with_copybara.run_branch_commit_sync(sync, initial_tokens)

        mock_get_copybara_tokens.assert_called_once_with(sync.expansions)
        self.assertEqual(
            [call_args.args[1] for call_args in mock_rewrite_copybara_config.call_args_list],
            [initial_tokens, fresh_tokens],
        )
        self.assertEqual(
            mock_run_branch_dry_run.call_args_list[1].args[0].copybara_config.source.git_url,
            sync_repo_with_copybara.auth_github_url(
                sync_repo_with_copybara.SOURCE_REPO_URL,
                "fresh-source-token",
            ),
        )
        self.assertEqual(mock_reset_preview_dir.call_count, 4)
        self.assertEqual(mock_run_branch_migrate.call_count, 2)

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_migrate")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_dry_run")
    @patch("buildscripts.copybara.sync_repo_with_copybara.reset_preview_dir")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_pending_source_commits")
    def test_run_branch_commit_sync_reuses_auth_retry_tokens(
        self,
        mock_list_pending_source_commits,
        mock_rewrite_copybara_config,
        mock_get_copybara_tokens,
        mock_reset_preview_dir,
        mock_run_branch_dry_run,
        mock_run_branch_migrate,
    ):
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="headsha",
            config_sha="local",
            workflow_name="prod_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo", "copybara"),
            expansions={"version_id": "patch123"},
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.SOURCE_REPO_URL,
                        "initial-source-token",
                    ),
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="headsha",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url=sync_repo_with_copybara.auth_github_url(
                        sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL,
                        "initial-prod-token",
                    ),
                    repo_name="mongodb/mongo",
                    branch="v8.2",
                ),
            ),
        )
        initial_tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "initial-source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "initial-prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "initial-test-token",
        }
        refreshed_tokens = {
            sync_repo_with_copybara.SOURCE_REPO_URL: "refreshed-source-token",
            sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "refreshed-prod-token",
            sync_repo_with_copybara.TEST_REPO_URL: "refreshed-test-token",
        }
        mock_list_pending_source_commits.return_value = [
            make_source_commit("commit1"),
            make_source_commit("commit2"),
        ]

        def dry_run_side_effect(commit_sync, on_auth_refresh=None):
            if commit_sync.source_ref == "commit1":
                on_auth_refresh(refreshed_tokens)
            return sync_repo_with_copybara.BranchDryRunResult(branch=commit_sync.branch)

        mock_run_branch_dry_run.side_effect = dry_run_side_effect

        sync_repo_with_copybara.run_branch_commit_sync(sync, initial_tokens)

        mock_get_copybara_tokens.assert_not_called()
        self.assertEqual(
            mock_run_branch_migrate.call_args_list[0].args[0].copybara_config.source.git_url,
            sync_repo_with_copybara.auth_github_url(
                sync_repo_with_copybara.SOURCE_REPO_URL,
                "refreshed-source-token",
            ),
        )
        self.assertEqual(
            [call_args.args[1] for call_args in mock_rewrite_copybara_config.call_args_list],
            [initial_tokens, refreshed_tokens],
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_migrate")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_dry_run")
    @patch("buildscripts.copybara.sync_repo_with_copybara.reset_preview_dir")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_pending_source_commits")
    def test_run_branch_commit_sync_cleans_after_failed_dry_run(
        self,
        mock_list_pending_source_commits,
        mock_rewrite_copybara_config,
        mock_get_copybara_tokens,
        mock_reset_preview_dir,
        mock_run_branch_dry_run,
        mock_run_branch_migrate,
    ):
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="headsha",
            config_sha="local",
            workflow_name="prod_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo", "copybara"),
            expansions={"version_id": "patch123"},
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="https://example.com/source.git",
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="headsha",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="https://example.com/destination.git",
                    repo_name="mongodb/mongo",
                    branch="v8.2",
                ),
            ),
        )
        failure = sync_repo_with_copybara.BranchSyncError("v8.2", "dry-run", "boom")
        mock_list_pending_source_commits.return_value = [make_source_commit("commit1")]
        mock_get_copybara_tokens.return_value = {"source": "fresh-token"}
        mock_run_branch_dry_run.side_effect = failure

        with self.assertRaises(sync_repo_with_copybara.BranchSyncError) as raised:
            sync_repo_with_copybara.run_branch_commit_sync(sync, {"source": "initial-token"})

        self.assertIs(raised.exception, failure)
        self.assertEqual(mock_reset_preview_dir.call_count, 2)
        self.assertEqual(
            [call_args.kwargs["reason"] for call_args in mock_reset_preview_dir.call_args_list],
            ["commit 1 dry-run", "commit 1 completion"],
        )
        mock_run_branch_migrate.assert_not_called()

    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_migrate")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_branch_dry_run")
    @patch("buildscripts.copybara.sync_repo_with_copybara.reset_preview_dir")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_copybara_tokens")
    @patch("buildscripts.copybara.sync_repo_with_copybara.rewrite_copybara_config")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_pending_source_commits")
    def test_run_branch_commit_sync_skips_noop_commit_and_continues(
        self,
        mock_list_pending_source_commits,
        mock_rewrite_copybara_config,
        mock_get_copybara_tokens,
        mock_reset_preview_dir,
        mock_run_branch_dry_run,
        mock_run_branch_migrate,
    ):
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="headsha",
            config_sha="local",
            workflow_name="prod_v8.2",
            config_file=Path("/tmp/copy.bara.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo", "copybara"),
            expansions={"version_id": "patch123"},
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="https://example.com/source.git",
                    repo_name="10gen/mongo",
                    branch="v8.2",
                    ref="headsha",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="https://example.com/destination.git",
                    repo_name="mongodb/mongo",
                    branch="v8.2",
                ),
            ),
        )
        mock_list_pending_source_commits.return_value = [
            make_source_commit("commit1"),
            make_source_commit("commit2"),
        ]
        mock_get_copybara_tokens.return_value = {"source": "fresh-token"}
        mock_run_branch_dry_run.side_effect = [
            sync_repo_with_copybara.BranchDryRunResult(branch="v8.2", noop=True),
            sync_repo_with_copybara.BranchDryRunResult(branch="v8.2"),
        ]

        result = sync_repo_with_copybara.run_branch_commit_sync(sync, {"source": "initial-token"})

        self.assertEqual(
            result,
            sync_repo_with_copybara.BranchCommitSyncResult(
                branch="v8.2",
                discovered_commits=2,
                migrated_commits=1,
                noop_commits=1,
            ),
        )
        mock_run_branch_migrate.assert_called_once()
        self.assertEqual(mock_run_branch_migrate.call_args.args[0].source_ref, "commit2")
        mock_get_copybara_tokens.assert_not_called()
        self.assertEqual(mock_reset_preview_dir.call_count, 4)

    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_branch_head")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_fetch_remote_copybara_config_bundle_reads_latest_master_bundle(
        self, mock_run_command, mock_get_remote_branch_head
    ):
        mock_get_remote_branch_head.return_value = "configsha123"
        local_runner_contents = Path(sync_repo_with_copybara.__file__).resolve().read_text()
        local_path_rules_contents = (
            Path(sync_repo_with_copybara.__file__).resolve().with_name("path_rules.py").read_text()
        )
        local_release_tag_template_contents = (
            Path(sync_repo_with_copybara.__file__)
            .resolve()
            .with_name(sync_repo_with_copybara.COPYBARA_RELEASE_TAG_HELPERS_TEMPLATE_PATH.name)
            .read_text()
        )
        template_text = (
            "rendered_from_template = True\n"
            "common_files_to_include = [\n"
            "{{COMMON_FILES_TO_INCLUDE}}\n"
            "]\n"
            "\n"
            "common_files_to_exclude = [\n"
            "{{COMMON_FILES_TO_EXCLUDE}}\n"
            "]\n"
        )

        def run_command_side_effect(command, **kwargs):
            if command.startswith("git init "):
                return ""
            if "fetch --depth 1" in command:
                return ""
            if expected_copybara_config_rev_parse_fragment() in command:
                return "configsha123\n"
            if sync_repo_with_copybara.COPYBARA_SYNC_RUNNER_PATH.as_posix() in command:
                return local_runner_contents
            if sync_repo_with_copybara.COPYBARA_PATH_RULES_HELPER_PATH.as_posix() in command:
                return local_path_rules_contents
            if (
                sync_repo_with_copybara.COPYBARA_RELEASE_TAG_HELPERS_TEMPLATE_PATH.as_posix()
                in command
            ):
                return local_release_tag_template_contents
            if "ls-tree -r --name-only" in command:
                return "\n".join(
                    [
                        sync_repo_with_copybara.COPYBARA_BASE_CONFIG_PATH.as_posix(),
                        "buildscripts/copybara/master.sky",
                        "buildscripts/copybara/v8_2.sky",
                    ]
                )
            if sync_repo_with_copybara.COPYBARA_BASE_CONFIG_PATH.as_posix() in command:
                return (
                    'source_url = "https://github.com/10gen/mongo.git"\n'
                    'prod_url = "https://github.com/mongodb/mongo.git"\n'
                    'test_url = "https://github.com/10gen/mongo-copybara.git"\n'
                    'test_branch_prefix = "copybara_test_branch"\n'
                    "source_refs = {}\n"
                    "\n"
                    "def make_workflow(workflow_name, destination_url, source_ref, destination_ref, branch_excluded_files):\n"
                    "    pass\n"
                    "\n"
                    "def sync_branch(branch_name, branch_excluded_files = []):\n"
                    '    make_workflow("prod_" + branch_name, prod_url, branch_name, branch_name, branch_excluded_files)\n'
                    "    make_workflow(\n"
                    '        "test_" + branch_name,\n'
                    "        test_url,\n"
                    "        branch_name,\n"
                    '        test_branch_prefix + "_" + branch_name,\n'
                    "        branch_excluded_files,\n"
                    "    )\n"
                )
            if sync_repo_with_copybara.COPYBARA_PATH_RULES_PATH.as_posix() in command:
                return json.dumps(
                    {
                        "common_files_to_include": ["**"],
                        "common_files_to_exclude": ["src/mongo/db/modules/**"],
                    }
                )
            if sync_repo_with_copybara.COPYBARA_PATH_RULES_TEMPLATE_PATH.as_posix() in command:
                return template_text
            if (
                sync_repo_with_copybara.COPYBARA_GENERATED_EVERGREEN_CONFIG_PATH.as_posix()
                in command
            ):
                return "generated Copybara Evergreen YAML\n"
            if "buildscripts/copybara/master.sky" in command:
                return 'sync_branch("master")\n'
            if "buildscripts/copybara/v8_2.sky" in command:
                return 'sync_branch("v8.2")\n'
            raise AssertionError(f"Unexpected command: {command}")

        mock_run_command.side_effect = run_command_side_effect

        with tempfile.TemporaryDirectory() as tmpdir:
            bundle = sync_repo_with_copybara.fetch_remote_copybara_config_bundle(
                tmpdir,
                "https://x-access-token:token@github.com/10gen/mongo.git",
            )

            self.assertEqual(bundle.config_sha, "configsha123")
            self.assertEqual(
                bundle.base_config_path.read_text().splitlines()[0],
                'source_url = "https://github.com/10gen/mongo.git"',
            )
            self.assertEqual(
                bundle.path_rules_path.read_text(),
                json.dumps(
                    {
                        "common_files_to_include": ["**"],
                        "common_files_to_exclude": ["src/mongo/db/modules/**"],
                    }
                ),
            )
            self.assertIn(
                "rendered_from_template = True",
                bundle.path_rules_module_path.read_text(),
            )
            generated_evergreen_config_path = (
                bundle.bundle_dir / sync_repo_with_copybara.COPYBARA_GENERATED_EVERGREEN_CONFIG_PATH
            )
            self.assertEqual(
                generated_evergreen_config_path.read_text(),
                "generated Copybara Evergreen YAML\n",
            )
            self.assertIn('"src/mongo/db/modules/**"', bundle.path_rules_module_path.read_text())
            self.assertIn("master", bundle.branch_to_fragment)
            self.assertIn("v8.2", bundle.branch_to_fragment)

    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_branch_head")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_fetch_remote_copybara_config_bundle_fails_for_stale_checked_out_runner(
        self, mock_run_command, mock_get_remote_branch_head
    ):
        mock_get_remote_branch_head.return_value = "configsha123"

        def run_command_side_effect(command, **kwargs):
            if command.startswith("git init "):
                return ""
            if "fetch --depth 1" in command:
                return ""
            if expected_copybara_config_rev_parse_fragment() in command:
                return "configsha123\n"
            if sync_repo_with_copybara.COPYBARA_SYNC_RUNNER_PATH.as_posix() in command:
                return "# stale runner from old build\n"
            raise AssertionError(f"Unexpected command: {command}")

        mock_run_command.side_effect = run_command_side_effect

        with tempfile.TemporaryDirectory() as tmpdir:
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                with self.assertRaises(SystemExit):
                    sync_repo_with_copybara.fetch_remote_copybara_config_bundle(
                        tmpdir,
                        "https://x-access-token:token@github.com/10gen/mongo.git",
                    )

        log_output = stdout.getvalue()
        self.assertIn("latest master-owned", log_output)
        self.assertIn(sync_repo_with_copybara.COPYBARA_SYNC_RUNNER_PATH.as_posix(), log_output)
        self.assertIn("Start a new master build", log_output)

    def test_fetch_remote_copybara_config_bundle_can_skip_checked_out_helper_check(self):
        def run_command_side_effect(command, **kwargs):
            if command.startswith("git init "):
                return ""
            if "fetch --depth 1" in command:
                return ""
            if expected_copybara_config_rev_parse_fragment() in command:
                return "configsha123\n"
            raise AssertionError(f"Unexpected command: {command}")

        def read_git_file_side_effect(fetch_repo_dir, git_ref, repo_path):
            repo_path = Path(repo_path)
            if repo_path == sync_repo_with_copybara.COPYBARA_BASE_CONFIG_PATH:
                return (
                    'source_url = "https://github.com/10gen/mongo.git"\n'
                    'prod_url = "https://github.com/mongodb/mongo.git"\n'
                    'test_url = "https://github.com/10gen/mongo-copybara.git"\n'
                    'test_branch_prefix = "copybara_test_branch"\n'
                    "source_refs = {}\n"
                )
            if repo_path == sync_repo_with_copybara.COPYBARA_PATH_RULES_PATH:
                return json.dumps(
                    {
                        "common_files_to_include": ["**"],
                        "common_files_to_exclude": ["src/mongo/db/modules/**"],
                    }
                )
            if repo_path == sync_repo_with_copybara.COPYBARA_PATH_RULES_TEMPLATE_PATH:
                return "{{COMMON_FILES_TO_INCLUDE}}\n{{COMMON_FILES_TO_EXCLUDE}}\n"
            if repo_path == sync_repo_with_copybara.COPYBARA_GENERATED_EVERGREEN_CONFIG_PATH:
                return "generated Copybara Evergreen YAML\n"
            if repo_path == Path("buildscripts/copybara/master.sky"):
                return 'sync_branch("master")\n'
            raise AssertionError(f"Unexpected repo path: {repo_path}")

        with tempfile.TemporaryDirectory() as tmpdir:
            fragment_path = (
                Path(tmpdir)
                / "tmp_copybara"
                / "config_bundle"
                / "configsha123"
                / "buildscripts"
                / "copybara"
                / "master.sky"
            )
            with (
                patch.object(sync_repo_with_copybara, "get_remote_branch_head") as mock_head,
                patch.object(sync_repo_with_copybara, "run_command") as mock_run_command,
                patch.object(
                    sync_repo_with_copybara, "ensure_current_copybara_python_helpers_match_master"
                ) as mock_ensure_helpers,
                patch.object(sync_repo_with_copybara, "read_git_file") as mock_read_git_file,
                patch.object(sync_repo_with_copybara, "list_copybara_fragment_paths") as mock_list,
                patch.object(sync_repo_with_copybara, "write_generated_copybara_path_rules_module"),
                patch.object(
                    sync_repo_with_copybara, "discover_copybara_branches"
                ) as mock_discover,
            ):
                mock_head.return_value = "configsha123"
                mock_run_command.side_effect = run_command_side_effect
                mock_read_git_file.side_effect = read_git_file_side_effect
                mock_list.return_value = ["buildscripts/copybara/master.sky"]
                mock_discover.return_value = {"master": fragment_path}

                bundle = sync_repo_with_copybara.fetch_remote_copybara_config_bundle(
                    tmpdir,
                    "https://x-access-token:token@github.com/10gen/mongo.git",
                    enforce_current_python_helpers=False,
                )

        self.assertEqual(bundle.config_sha, "configsha123")
        self.assertEqual(bundle.branch_to_fragment, {"master": fragment_path})
        mock_ensure_helpers.assert_not_called()

    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_branch_head")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_fetch_remote_copybara_config_bundle_fails_for_stale_checked_out_path_rules(
        self, mock_run_command, mock_get_remote_branch_head
    ):
        mock_get_remote_branch_head.return_value = "configsha123"
        local_runner_contents = Path(sync_repo_with_copybara.__file__).resolve().read_text()

        def run_command_side_effect(command, **kwargs):
            if command.startswith("git init "):
                return ""
            if "fetch --depth 1" in command:
                return ""
            if expected_copybara_config_rev_parse_fragment() in command:
                return "configsha123\n"
            if sync_repo_with_copybara.COPYBARA_SYNC_RUNNER_PATH.as_posix() in command:
                return local_runner_contents
            if sync_repo_with_copybara.COPYBARA_PATH_RULES_HELPER_PATH.as_posix() in command:
                return "# stale path rules helper from old build\n"
            raise AssertionError(f"Unexpected command: {command}")

        mock_run_command.side_effect = run_command_side_effect

        with tempfile.TemporaryDirectory() as tmpdir:
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                with self.assertRaises(SystemExit):
                    sync_repo_with_copybara.fetch_remote_copybara_config_bundle(
                        tmpdir,
                        "https://x-access-token:token@github.com/10gen/mongo.git",
                    )

        log_output = stdout.getvalue()
        self.assertIn("latest master-owned", log_output)
        self.assertIn(
            sync_repo_with_copybara.COPYBARA_PATH_RULES_HELPER_PATH.as_posix(), log_output
        )
        self.assertIn("path rules helper", log_output)
        self.assertIn("Start a new master build", log_output)

    @patch("buildscripts.copybara.sync_repo_with_copybara.get_remote_branch_head")
    @patch("buildscripts.copybara.sync_repo_with_copybara.run_command")
    def test_fetch_remote_copybara_config_bundle_fails_for_stale_release_tag_template(
        self, mock_run_command, mock_get_remote_branch_head
    ):
        mock_get_remote_branch_head.return_value = "configsha123"
        local_runner_contents = Path(sync_repo_with_copybara.__file__).resolve().read_text()
        local_path_rules_contents = (
            Path(sync_repo_with_copybara.__file__).resolve().with_name("path_rules.py").read_text()
        )

        def run_command_side_effect(command, **kwargs):
            if command.startswith("git init "):
                return ""
            if "fetch --depth 1" in command:
                return ""
            if expected_copybara_config_rev_parse_fragment() in command:
                return "configsha123\n"
            if sync_repo_with_copybara.COPYBARA_SYNC_RUNNER_PATH.as_posix() in command:
                return local_runner_contents
            if sync_repo_with_copybara.COPYBARA_PATH_RULES_HELPER_PATH.as_posix() in command:
                return local_path_rules_contents
            if (
                sync_repo_with_copybara.COPYBARA_RELEASE_TAG_HELPERS_TEMPLATE_PATH.as_posix()
                in command
            ):
                return "# stale release tag helper template from old build\n"
            raise AssertionError(f"Unexpected command: {command}")

        mock_run_command.side_effect = run_command_side_effect

        with tempfile.TemporaryDirectory() as tmpdir:
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                with self.assertRaises(SystemExit):
                    sync_repo_with_copybara.fetch_remote_copybara_config_bundle(
                        tmpdir,
                        "https://x-access-token:token@github.com/10gen/mongo.git",
                    )

        log_output = stdout.getvalue()
        self.assertIn("latest master-owned", log_output)
        self.assertIn(
            sync_repo_with_copybara.COPYBARA_RELEASE_TAG_HELPERS_TEMPLATE_PATH.as_posix(),
            log_output,
        )
        self.assertIn("release tag helper template", log_output)
        self.assertIn("Start a new master build", log_output)

    def test_get_local_copybara_config_bundle_renders_checked_out_path_rules_module(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            base_config_path = get_repo_base_copybara_config_path(root)
            write_base_copybara_config(base_config_path)
            path_rules_path = get_repo_copybara_path_rules_path(root)
            write_copybara_path_rules(
                path_rules_path,
                common_includes=DEFAULT_TEST_COPYBARA_PATH_RULES_INCLUDES,
                common_excludes=DEFAULT_TEST_COPYBARA_PATH_RULES_EXCLUDES,
            )
            path_rules_template_path = get_repo_copybara_path_rules_template_path(root)
            write_copybara_path_rules_template(path_rules_template_path)
            path_rules_module_path = get_repo_copybara_path_rules_module_path(root)
            fragment_dir = root / "buildscripts" / "copybara"
            fragment_dir.mkdir(parents=True, exist_ok=True)
            (fragment_dir / "master.sky").write_text('sync_branch("master")\n')
            (fragment_dir / "v8_2.sky").write_text('sync_branch("v8.2")\n')

            bundle = sync_repo_with_copybara.get_local_copybara_config_bundle(tmpdir)

            self.assertEqual(bundle.config_sha, "local")
            self.assertEqual(bundle.base_config_path, base_config_path)
            self.assertEqual(bundle.path_rules_path, path_rules_path)
            self.assertEqual(bundle.path_rules_module_path, path_rules_module_path)
            self.assertEqual(
                path_rules_module_path.read_text(),
                render_copybara_path_rules_module_from_files(
                    path_rules_template_path,
                    path_rules_path,
                ),
            )
            self.assertEqual(bundle.branch_to_fragment["master"], fragment_dir / "master.sky")
            self.assertEqual(bundle.branch_to_fragment["v8.2"], fragment_dir / "v8_2.sky")


class TestGenerateCopybaraEvergreen(unittest.TestCase):
    @staticmethod
    def write_fragment(root: Path, filename: str, contents: str) -> None:
        fragment_path = root / sync_repo_with_copybara.COPYBARA_CONFIG_DIRECTORY / filename
        fragment_path.parent.mkdir(parents=True, exist_ok=True)
        fragment_path.write_text(contents)

    def test_render_expected_copybara_evergreen_uses_branch_and_tag_fragments(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            self.write_fragment(
                root,
                "master.sky",
                'sync_branch("master", evergreen_activate = True)\n',
            )
            self.write_fragment(
                root,
                "v8_2.sky",
                'sync_branch("v8.2")\n'
                'sync_branch("v8.2.7-hotfix")\n'
                'sync_tag("r8.2.7-hotfix", evergreen_activate = True)\n',
            )

            generated = generate_evergreen.render_expected_copybara_evergreen(root)

            self.assertIn(
                "# This file is generated by buildscripts/copybara/generate_evergreen.py.",
                generated,
            )
            self.assertIn("  - name: sync_copybara_master", generated)
            self.assertIn("  - name: sync_copybara_v8_2", generated)
            self.assertIn("  - name: sync_copybara_v8_2_7_hotfix", generated)
            self.assertIn('          copybara_branches: "v8.2.7-hotfix"', generated)
            self.assertIn("  - name: sync_copybara_r8_2_7_hotfix", generated)
            self.assertIn('          copybara_branches: "r8.2.7-hotfix"', generated)
            self.assertIn("  - name: test_copybara_sync", generated)
            self.assertIn("    patch_only: true", generated)
            self.assertIn('      - func: "test copybara sync"', generated)
            self.assertIn("  - name: test_copybara_master", generated)
            self.assertIn("  - name: test_copybara_v8_2", generated)
            self.assertIn("  - name: test_copybara_v8_2_7_hotfix", generated)
            self.assertIn("  - name: test_copybara_r8_2_7_hotfix", generated)
            self.assertIn('      - func: "run copybara test sync task"', generated)
            self.assertIn(
                "  - name: test_copybara_v8_2\n"
                '    tags: ["assigned_to_jira_team_devprod_test_infrastructure", "auxiliary"]\n'
                "    patchable: true",
                generated,
            )
            self.assertNotIn("sync_copybara_task_template", generated)
            self.assertLess(
                generated.index("sync_copybara_master"),
                generated.index("sync_copybara_v8_2"),
            )
            self.assertLess(
                generated.index("sync_copybara_v8_2_7_hotfix"),
                generated.index("sync_copybara_r8_2_7_hotfix"),
            )
            self.assertIn(
                "      - name: sync_copybara_master\n        priority: 50\n        activate: true",
                generated,
            )
            self.assertIn(
                "      - name: sync_copybara_v8_2\n        priority: 50\n        activate: false",
                generated,
            )

            sync_variant = generated[
                generated.index("  - name: copybara-sync-between-repos") : generated.index(
                    "  - name: copybara-test-sync-between-repos"
                )
            ]
            self.assertNotIn("test_copybara_v8_2", sync_variant)

            test_variant = generated[
                generated.index("  - name: copybara-test-sync-between-repos") :
            ]
            self.assertIn(
                "  - name: copybara-test-sync-between-repos\n"
                '    display_name: "* Copybara Test Sync Between Repos"\n'
                '    tags: ["suggested"]\n'
                "    activate: false\n"
                "    patchable: true",
                test_variant,
            )
            self.assertIn(
                "      - name: test_copybara_sync\n        priority: 50\n        activate: false",
                test_variant,
            )
            self.assertIn(
                "      - name: test_copybara_v8_2\n        priority: 50\n        activate: false",
                test_variant,
            )
            self.assertIn(
                "      - name: sync_copybara_r8_2_7_hotfix\n"
                "        priority: 50\n"
                "        activate: true",
                generated,
            )
            self.assertIn(
                "      - name: test_copybara_r8_2_7_hotfix\n"
                "        priority: 50\n"
                "        activate: false",
                test_variant,
            )

    def test_check_generated_copybara_evergreen_detects_stale_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            self.write_fragment(root, "master.sky", 'sync_branch("master")\n')
            generated_path = root / generate_evergreen.COPYBARA_EVERGREEN_GENERATED_CONFIG_PATH
            generated_path.parent.mkdir(parents=True, exist_ok=True)
            generated_path.write_text("stale\n")

            stdout = io.StringIO()
            with redirect_stdout(stdout):
                is_current = generate_evergreen.check_generated_copybara_evergreen(root)

            self.assertFalse(is_current)
            self.assertIn("Generated Copybara Evergreen config is stale", stdout.getvalue())

    def test_check_generated_copybara_evergreen_accepts_current_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            self.write_fragment(root, "master.sky", 'sync_branch("master")\n')
            generated_path = root / generate_evergreen.COPYBARA_EVERGREEN_GENERATED_CONFIG_PATH
            generated_path.parent.mkdir(parents=True, exist_ok=True)
            generated_path.write_text(generate_evergreen.render_expected_copybara_evergreen(root))

            self.assertTrue(generate_evergreen.check_generated_copybara_evergreen(root))

    @patch("buildscripts.copybara.generate_evergreen.check_generated_copybara_evergreen")
    def test_sync_precheck_accepts_current_generated_evergreen(self, mock_check_generated):
        mock_check_generated.return_value = True

        stdout = io.StringIO()
        with redirect_stdout(stdout):
            sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current("/repo")

        mock_check_generated.assert_called_once_with(Path("/repo"))
        self.assertIn("Generated Copybara Evergreen config is current", stdout.getvalue())

    @patch("buildscripts.copybara.generate_evergreen.check_generated_copybara_evergreen")
    def test_sync_precheck_rejects_stale_generated_evergreen(self, mock_check_generated):
        mock_check_generated.return_value = False

        with self.assertRaises(SystemExit) as context:
            sync_repo_with_copybara.ensure_generated_copybara_evergreen_is_current("/repo")

        self.assertEqual(context.exception.code, 1)
        mock_check_generated.assert_called_once_with(Path("/repo"))


class TestValidateSyncConfig(unittest.TestCase):
    @patch("buildscripts.copybara.sync_repo_with_copybara.check_branch_top_level_paths_are_labeled")
    @patch("buildscripts.copybara.sync_repo_with_copybara.list_top_level_paths_for_remote_ref")
    def test_uses_pinned_source_ref_for_top_level_validation(
        self,
        mock_list_top_level_paths_for_remote_ref,
        mock_check_branch_top_level_paths_are_labeled,
    ):
        mock_list_top_level_paths_for_remote_ref.return_value = {"README.md", "src"}
        sync = sync_repo_with_copybara.PreparedBranchSync(
            branch="v8.2",
            source_ref="deadbeef123",
            config_sha="configsha123",
            workflow_name="prod_v8.2",
            config_file=Path("/tmp/generated.sky"),
            preview_dir=Path("/tmp/preview"),
            docker_command=("echo", "copybara"),
            copybara_config=sync_repo_with_copybara.CopybaraConfig(
                source=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="https://example.com/source.git",
                    repo_name=sync_repo_with_copybara.SOURCE_REPO_NAME,
                    branch="v8.2",
                    ref="deadbeef123",
                ),
                destination=sync_repo_with_copybara.CopybaraRepoConfig(
                    git_url="https://example.com/destination.git",
                    repo_name=sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_NAME,
                    branch="v8.2",
                ),
            ),
        )

        sync_repo_with_copybara.validate_sync_config(sync)

        mock_list_top_level_paths_for_remote_ref.assert_called_once_with(
            "https://example.com/source.git",
            "deadbeef123",
        )
        mock_check_branch_top_level_paths_are_labeled.assert_called_once_with(
            str(sync.config_file),
            "v8.2",
            {"README.md", "src"},
        )


class TestEnsureCopybaraSourceRefSupport(unittest.TestCase):
    """Verify that ensure_copybara_source_ref_support correctly injects or preserves source_refs."""

    def _make_config_without_source_refs(self) -> str:
        return textwrap.dedent("""\
            source_url = "https://github.com/10gen/mongo.git"
            test_branch_prefix = "copybara_test_branch"

            def sync_branch(branch_name):
                make_workflow("prod_" + branch_name, prod_url, branch_name, branch_name)
                make_workflow(
                    "test_" + branch_name,
                    test_url,
                    branch_name,
                    test_branch_prefix + "_" + branch_name,
                )
            """)

    def _make_config_with_source_refs(self) -> str:
        return textwrap.dedent("""\
            source_url = "https://github.com/10gen/mongo.git"
            test_branch_prefix = "copybara_test_branch"
            source_refs = {}

            def sync_branch(branch_name):
                source_ref = source_refs.get(branch_name, branch_name)
                make_workflow(
                    "prod_" + branch_name,
                    prod_url,
                    source_ref,
                    branch_name,
                )
                make_workflow(
                    "test_" + branch_name,
                    test_url,
                    source_ref,
                    test_branch_prefix + "_" + branch_name,
                )
            """)

    def _make_legacy_config_with_source_refs(self) -> str:
        return textwrap.dedent("""\
            source_url = "https://github.com/10gen/mongo.git"
            test_branch_prefix = "copybara_test_branch"
            source_refs = {}

            def sync_branch(branch_name, branch_excluded_files = [], branch_public_files = ["**"]):
                source_ref = source_refs.get(branch_name, branch_name)
                make_workflow(
                    "prod_" + branch_name,
                    prod_url,
                    source_ref,
                    branch_name,
                    branch_excluded_files,
                    branch_public_files,
                )
                make_workflow(
                    "test_" + branch_name,
                    test_url,
                    source_ref,
                    test_branch_prefix + "_" + branch_name,
                    branch_excluded_files,
                    branch_public_files,
                )
            """)

    def test_injects_source_refs_when_missing(self):
        contents = self._make_config_without_source_refs()
        result = sync_repo_with_copybara.ensure_copybara_source_ref_support(
            contents, Path("test.sky")
        )
        self.assertIn("source_refs = {}", result)
        self.assertIn("source_ref = source_refs.get(branch_name, branch_name)", result)
        self.assertIn("def sync_tag(tag_name, evergreen_activate = False):", result)
        self.assertIn(
            'make_workflow("prod_" + branch_name, prod_url, source_ref, branch_name)',
            result,
        )
        self.assertIn(
            'make_workflow("prod_" + tag_name, prod_url, source_ref, branch_name)',
            result,
        )
        self.assertNotIn("for char in version_part:", result)
        self.assertIn("for char_index in range(len(version_part)):", result)
        self.assertIn("char = version_part[char_index]", result)

    def test_preserves_existing_source_refs(self):
        contents = self._make_config_with_source_refs()
        result = sync_repo_with_copybara.ensure_copybara_source_ref_support(
            contents, Path("test.sky")
        )
        self.assertIn("source_refs = {}", result)
        self.assertIn("source_ref = source_refs.get(branch_name, branch_name)", result)
        self.assertIn("def sync_branch(branch_name, evergreen_activate = False):", result)

    def test_preserves_legacy_source_refs_with_public_files(self):
        contents = self._make_legacy_config_with_source_refs()
        result = sync_repo_with_copybara.ensure_copybara_source_ref_support(
            contents, Path("test.sky")
        )
        self.assertIn("source_refs = {}", result)
        self.assertIn("source_ref = source_refs.get(branch_name, branch_name)", result)
        self.assertIn('branch_public_files = ["**"],', result)
        self.assertIn("evergreen_activate = False", result)
        self.assertIn("branch_public_files,\n    )", result)

    def test_exits_when_sync_branch_not_found(self):
        contents = textwrap.dedent("""\
            source_url = "https://github.com/10gen/mongo.git"
            test_branch_prefix = "copybara_test_branch"
            source_refs = {}
            """)
        with self.assertRaises(SystemExit):
            sync_repo_with_copybara.ensure_copybara_source_ref_support(contents, Path("test.sky"))

    def test_exits_when_test_branch_prefix_not_found(self):
        contents = textwrap.dedent("""\
            source_url = "https://github.com/10gen/mongo.git"

            def sync_branch(branch_name, branch_excluded_files = []):
                pass
            """)
        with self.assertRaises(SystemExit):
            sync_repo_with_copybara.ensure_copybara_source_ref_support(contents, Path("test.sky"))


class TestValidatePreviewExclusions(unittest.TestCase):
    """Verify dry-run output validation catches forbidden files."""

    def _make_sync_with_preview(
        self,
        files: list[str],
        branch: str = "master",
        branch_excluded_patterns: list[str] | None = None,
    ) -> sync_repo_with_copybara.PreparedBranchSync:
        tmpdir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmpdir)
        root = Path(tmpdir)

        config_path = root / "copy.bara.sky"
        write_base_copybara_config(config_path, common_includes=["README.md"])
        branch_excluded_patterns = branch_excluded_patterns or []
        if branch_excluded_patterns:
            exclude_entries = "\n".join(f'    "{pattern}",' for pattern in branch_excluded_patterns)
            config_path.write_text(
                config_path.read_text()
                + "\nbranch_files_to_exclude = [\n"
                + f"{exclude_entries}\n"
                + "]\n"
                + f'sync_branch("{branch}", branch_files_to_exclude)\n'
            )
        else:
            config_path.write_text(config_path.read_text() + f'\nsync_branch("{branch}")\n')

        preview_dir = root / "preview" / "checkout"
        preview_dir.mkdir(parents=True)
        for file_path in files:
            full_path = preview_dir / file_path
            full_path.parent.mkdir(parents=True, exist_ok=True)
            full_path.write_text("content")

        return sync_repo_with_copybara.PreparedBranchSync(
            branch=branch,
            source_ref="abc123",
            config_sha="sha123",
            workflow_name=f"prod_{branch}",
            config_file=config_path,
            preview_dir=root / "preview",
            docker_command=("echo",),
        )

    def test_passes_when_no_excluded_files_present(self):
        sync = self._make_sync_with_preview(
            [
                "src/mongo/db/catalog/collection.cpp",
                "README.md",
                "jstests/core/basic.js",
            ]
        )
        sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_modules_directory_present(self):
        sync = self._make_sync_with_preview(
            [
                "src/mongo/db/modules/enterprise/something.cpp",
            ]
        )
        with self.assertRaises(sync_repo_with_copybara.BranchSyncError) as ctx:
            sync_repo_with_copybara.validate_preview_exclusions(sync)
        self.assertIn("preview validation", ctx.exception.stage)

    def test_fails_when_agents_md_present(self):
        sync = self._make_sync_with_preview(["AGENTS.md"])
        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_private_third_party_present(self):
        sync = self._make_sync_with_preview(
            [
                "src/third_party/private/secret.h",
            ]
        )
        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_cursor_directory_present(self):
        sync = self._make_sync_with_preview(
            [
                ".cursor/rules/my_rule.md",
            ]
        )
        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_github_codeowners_present(self):
        sync = self._make_sync_with_preview(
            [
                ".github/CODEOWNERS",
            ]
        )
        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_monguard_present(self):
        sync = self._make_sync_with_preview(
            [
                "monguard/config.yaml",
            ]
        )
        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_branch_specific_excluded_file_present(self):
        sync = self._make_sync_with_preview(
            ["docs/private-notes/secret.md"],
            branch="v8.2",
            branch_excluded_patterns=["docs/private-notes/**"],
        )
        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_private_commercial_header_present(self):
        sync = self._make_sync_with_preview([])
        public_header_path = sync.preview_dir / "checkout" / "src/mongo/util/public_header.h"
        public_header_path.parent.mkdir(parents=True, exist_ok=True)
        public_header_path.write_text(
            textwrap.dedent("""\
            /**
             *    Copyright (C) 2025-present MongoDB, Inc. and subject to applicable commercial license.
             */
            """)
        )

        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_excluded_broken_symlink_present(self):
        if sys.platform == "win32":
            self.skipTest("symlink permissions vary on Windows")

        sync = self._make_sync_with_preview([])
        excluded_symlink = sync.preview_dir / "checkout" / "AGENTS.md"
        os.symlink("missing-target", excluded_symlink)

        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)

    def test_fails_when_excluded_symlinked_directory_present(self):
        if sys.platform == "win32":
            self.skipTest("symlink permissions vary on Windows")

        sync = self._make_sync_with_preview([])
        excluded_symlink_dir = sync.preview_dir / "checkout" / "src/mongo/db/modules"
        excluded_symlink_dir.parent.mkdir(parents=True, exist_ok=True)
        os.symlink("missing-target", excluded_symlink_dir)

        with self.assertRaises(sync_repo_with_copybara.BranchSyncError):
            sync_repo_with_copybara.validate_preview_exclusions(sync)


class TestShellQuote(unittest.TestCase):
    """Verify shell quoting used by string commands."""

    def test_windows_shell_quote_uses_cmd_compatible_quotes(self):
        with patch.object(sync_repo_with_copybara.os, "name", "nt"):
            self.assertEqual(
                '"C:\\data\\mci\\tmp\\repo"',
                sync_repo_with_copybara.shell_quote(r"C:\data\mci\tmp\repo"),
            )
            self.assertEqual(
                '"refs/tags/r8.2.7^{}"',
                sync_repo_with_copybara.shell_quote("refs/tags/r8.2.7^{}"),
            )

    def test_posix_shell_quote_uses_shlex(self):
        with patch.object(sync_repo_with_copybara.os, "name", "posix"):
            self.assertEqual(
                "'/tmp/path with spaces/repo'",
                sync_repo_with_copybara.shell_quote("/tmp/path with spaces/repo"),
            )


class TestRedactSecrets(unittest.TestCase):
    """Verify token redaction in log output."""

    def test_redacts_known_tokens(self):
        original_secrets = list(sync_repo_with_copybara.REDACTED_STRINGS)
        try:
            sync_repo_with_copybara.REDACTED_STRINGS.clear()
            sync_repo_with_copybara.REDACTED_STRINGS.append("ghp_SuperSecretToken123")

            result = sync_repo_with_copybara.redact_secrets(
                "Using token ghp_SuperSecretToken123 for auth"
            )
            self.assertNotIn("ghp_SuperSecretToken123", result)
            self.assertIn("<REDACTED>", result)
        finally:
            sync_repo_with_copybara.REDACTED_STRINGS.clear()
            sync_repo_with_copybara.REDACTED_STRINGS.extend(original_secrets)

    def test_redacts_github_url_credentials(self):
        url = "https://x-access-token:ghs_abc123xyz@github.com/10gen/mongo.git"
        result = sync_repo_with_copybara.redact_secrets(url)
        self.assertNotIn("ghs_abc123xyz", result)
        self.assertIn("https://x-access-token:<REDACTED>@github.com", result)

    def test_redacts_unknown_github_url_credentials(self):
        """Credentials not in REDACTED_STRINGS are still caught by the regex fallback."""
        url = "https://x-access-token:unknown_ambient_token@github.com/mongodb/mongo.git"
        result = sync_repo_with_copybara.redact_secrets(url)
        self.assertNotIn("unknown_ambient_token", result)

    def test_preserves_non_sensitive_text(self):
        text = "Running copybara for branch master"
        result = sync_repo_with_copybara.redact_secrets(text)
        self.assertEqual(text, result)

    def test_handles_empty_redacted_strings_list(self):
        original_secrets = list(sync_repo_with_copybara.REDACTED_STRINGS)
        try:
            sync_repo_with_copybara.REDACTED_STRINGS.clear()
            result = sync_repo_with_copybara.redact_secrets("no secrets here")
            self.assertEqual("no secrets here", result)
        finally:
            sync_repo_with_copybara.REDACTED_STRINGS.clear()
            sync_repo_with_copybara.REDACTED_STRINGS.extend(original_secrets)

    def test_read_git_file_returns_unredacted_source_text(self):
        source_text = (
            'url = repo_url.replace("https://github.com", '
            'f"https://x-access-token:{token}@github.com", 1)\n'
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            repo_dir = Path(tmpdir)
            git_env = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull}
            subprocess.run(
                ["git", "init"],
                cwd=repo_dir,
                check=True,
                capture_output=True,
                env=git_env,
            )
            subprocess.run(
                ["git", "config", "--local", "user.email", "test@example.com"],
                cwd=repo_dir,
                check=True,
                capture_output=True,
                env=git_env,
            )
            subprocess.run(
                ["git", "config", "--local", "user.name", "Test User"],
                cwd=repo_dir,
                check=True,
                capture_output=True,
                env=git_env,
            )
            source_path = repo_dir / "helper.py"
            source_path.write_text(source_text)
            subprocess.run(
                ["git", "add", "helper.py"],
                cwd=repo_dir,
                check=True,
                capture_output=True,
                env=git_env,
            )
            subprocess.run(
                ["git", "commit", "-m", "add helper"],
                cwd=repo_dir,
                check=True,
                capture_output=True,
                env=git_env,
            )

            stdout = io.StringIO()
            os.chdir(Path(__file__).resolve().parents[2])
            with redirect_stdout(stdout):
                result = sync_repo_with_copybara.read_git_file(repo_dir, "HEAD", "helper.py")

        self.assertEqual(source_text, result)
        self.assertIn("https://x-access-token:<REDACTED>@github.com", stdout.getvalue())
        self.assertNotIn("https://x-access-token:{token}@github.com", stdout.getvalue())


class TestExtractSkyExcludedPatternsRejectsDuplicates(unittest.TestCase):
    """Verify that duplicate common_files_to_exclude definitions are rejected."""

    def test_rejects_multiple_common_files_to_exclude_definitions(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            sky_path.write_text(
                textwrap.dedent("""\
                common_files_to_exclude = [
                    "src/mongo/db/modules/**",
                ]

                common_files_to_exclude = [
                    "harmless_file.txt",
                ]
                """)
            )

            with self.assertRaises(SystemExit):
                sync_repo_with_copybara.extract_sky_excluded_patterns(str(sky_path))

    def test_accepts_single_definition(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(sky_path)
            patterns = sync_repo_with_copybara.extract_sky_excluded_patterns(str(sky_path))
            self.assertIsInstance(patterns, set)
            self.assertTrue(len(patterns) > 0)

    def test_ignores_commented_out_definitions(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            sky_path = Path(tmpdir) / "copy.bara.sky"
            sky_path.write_text(
                textwrap.dedent("""\
                # common_files_to_exclude = ["should_be_ignored/**"]
                common_files_to_exclude = [
                    "src/mongo/db/modules/**",
                ]
                """)
            )

            patterns = sync_repo_with_copybara.extract_sky_excluded_patterns(str(sky_path))
            self.assertNotIn("should_be_ignored/**", patterns)
            self.assertIn("src/mongo/db/modules/**", patterns)


class TestMatchesExcludedPattern(unittest.TestCase):
    """Verify path matching logic for excluded patterns."""

    def test_directory_pattern_matches_files_in_subtree(self):
        self.assertTrue(
            sync_repo_with_copybara.matches_excluded_pattern(
                "src/mongo/db/modules/enterprise/foo.cpp", "src/mongo/db/modules/"
            )
        )

    def test_directory_pattern_matches_directory_itself(self):
        self.assertTrue(
            sync_repo_with_copybara.matches_excluded_pattern(
                "src/mongo/db/modules", "src/mongo/db/modules/"
            )
        )

    def test_directory_pattern_with_glob_suffix(self):
        self.assertTrue(
            sync_repo_with_copybara.matches_excluded_pattern(
                "src/third_party/private/secret.h", "src/third_party/private/**"
            )
        )

    def test_directory_pattern_does_not_match_unrelated_path(self):
        self.assertFalse(
            sync_repo_with_copybara.matches_excluded_pattern(
                "src/mongo/db/storage/wiredtiger.cpp", "src/mongo/db/modules/"
            )
        )

    def test_exact_file_pattern_matches_exact_path(self):
        self.assertTrue(sync_repo_with_copybara.matches_excluded_pattern("AGENTS.md", "AGENTS.md"))

    def test_exact_file_pattern_does_not_match_different_file(self):
        self.assertFalse(sync_repo_with_copybara.matches_excluded_pattern("README.md", "AGENTS.md"))

    def test_exact_file_pattern_does_not_match_subdirectory_file(self):
        self.assertFalse(
            sync_repo_with_copybara.matches_excluded_pattern("docs/AGENTS.md", "AGENTS.md")
        )

    def test_directory_pattern_does_not_match_prefix_overlap(self):
        """monguard/ should not match monguard_extra/."""
        self.assertFalse(
            sync_repo_with_copybara.matches_excluded_pattern("monguard_extra/file.txt", "monguard/")
        )


class TestCanonicalizeExcludedPattern(unittest.TestCase):
    """Verify pattern normalization for preview exclusion matching."""

    def test_trailing_slash_becomes_directory_pattern(self):
        self.assertEqual(
            sync_repo_with_copybara.canonicalize_excluded_pattern("monguard/"),
            "monguard/",
        )

    def test_glob_suffix_becomes_directory_pattern(self):
        self.assertEqual(
            sync_repo_with_copybara.canonicalize_excluded_pattern("monguard/**"),
            "monguard/",
        )

    def test_exact_file_stays_exact(self):
        self.assertEqual(
            sync_repo_with_copybara.canonicalize_excluded_pattern("AGENTS.md"),
            "AGENTS.md",
        )

    def test_rejects_wildcard_patterns(self):
        with self.assertRaises(SystemExit):
            sync_repo_with_copybara.canonicalize_excluded_pattern("*.py")

    def test_rejects_empty_pattern(self):
        with self.assertRaises(SystemExit):
            sync_repo_with_copybara.canonicalize_excluded_pattern("")

    def test_rejects_slash_only_pattern(self):
        with self.assertRaises(SystemExit):
            sync_repo_with_copybara.canonicalize_excluded_pattern("/")


class TestAssembleCopybaraConfig(unittest.TestCase):
    """Verify that base config and fragments are correctly concatenated."""

    def test_combines_base_and_single_fragment(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            base = root / "base.sky"
            base.write_text('# base config\nsource_url = "foo"\n')

            fragment = root / "master.sky"
            fragment.write_text('sync_branch("master")\n')

            output = root / "assembled.sky"
            sync_repo_with_copybara.assemble_copybara_config(base, [fragment], output)

            assembled = output.read_text()
            self.assertIn("# base config", assembled)
            self.assertIn('sync_branch("master")', assembled)
            self.assertIn(f"# BEGIN {fragment.as_posix()}", assembled)
            self.assertIn(f"# END {fragment.as_posix()}", assembled)

    def test_combines_base_and_multiple_fragments(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            base = root / "base.sky"
            base.write_text("# base\n")

            frag_a = root / "a.sky"
            frag_a.write_text('sync_branch("a")\n')
            frag_b = root / "b.sky"
            frag_b.write_text('sync_branch("b")\n')

            output = root / "out.sky"
            sync_repo_with_copybara.assemble_copybara_config(base, [frag_a, frag_b], output)

            assembled = output.read_text()
            self.assertIn('sync_branch("a")', assembled)
            self.assertIn('sync_branch("b")', assembled)
            idx_a = assembled.index('sync_branch("a")')
            idx_b = assembled.index('sync_branch("b")')
            self.assertLess(idx_a, idx_b)

    def test_output_ends_with_newline(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            base = root / "base.sky"
            base.write_text("# base\n")
            fragment = root / "f.sky"
            fragment.write_text('sync_branch("x")\n')
            output = root / "out.sky"
            sync_repo_with_copybara.assemble_copybara_config(base, [fragment], output)
            self.assertTrue(output.read_text().endswith("\n"))


class TestParseBranchList(unittest.TestCase):
    """Verify edge-case handling for comma-separated branch parsing."""

    def test_returns_empty_for_none(self):
        self.assertEqual(sync_repo_with_copybara.parse_branch_list(None), [])

    def test_returns_empty_for_blank_string(self):
        self.assertEqual(sync_repo_with_copybara.parse_branch_list(""), [])

    def test_single_branch(self):
        self.assertEqual(
            sync_repo_with_copybara.parse_branch_list("master"),
            ["master"],
        )

    def test_deduplicates_preserving_order(self):
        self.assertEqual(
            sync_repo_with_copybara.parse_branch_list("v8.2, master, v8.2"),
            ["v8.2", "master"],
        )

    def test_strips_whitespace(self):
        self.assertEqual(
            sync_repo_with_copybara.parse_branch_list("  master , v8.0 , v8.2 "),
            ["master", "v8.0", "v8.2"],
        )

    def test_skips_empty_entries_from_trailing_comma(self):
        self.assertEqual(
            sync_repo_with_copybara.parse_branch_list("master,v8.0,"),
            ["master", "v8.0"],
        )

    def test_skips_empty_entries_from_double_comma(self):
        self.assertEqual(
            sync_repo_with_copybara.parse_branch_list("master,,v8.0"),
            ["master", "v8.0"],
        )


class TestRealCopybaraSkyConfiguration(unittest.TestCase):
    """Integration tests for the checked-in Copybara config files."""

    REAL_COPYBARA_ROOT = Path(__file__).resolve().parents[2]
    REAL_SKY_PATH = REAL_COPYBARA_ROOT / sync_repo_with_copybara.COPYBARA_BASE_CONFIG_PATH
    REAL_TEMPLATE_PATH = (
        REAL_COPYBARA_ROOT / "buildscripts" / "copybara" / "copybara_path_rules.bara.sky.template"
    )
    REAL_PATH_RULES_PATH = REAL_COPYBARA_ROOT / sync_repo_with_copybara.COPYBARA_PATH_RULES_PATH
    REAL_PATH_RULES_MODULE_PATH = (
        REAL_COPYBARA_ROOT / sync_repo_with_copybara.COPYBARA_PATH_RULES_MODULE_PATH
    )
    REAL_RELEASE_TAG_HELPERS_TEMPLATE_PATH = (
        REAL_COPYBARA_ROOT / sync_repo_with_copybara.COPYBARA_RELEASE_TAG_HELPERS_TEMPLATE_PATH
    )
    REAL_GENERATED_EVERGREEN_PATH = (
        REAL_COPYBARA_ROOT / generate_evergreen.COPYBARA_EVERGREEN_GENERATED_CONFIG_PATH
    )
    REAL_EVERGREEN_COMPONENT_PATH = (
        REAL_COPYBARA_ROOT / "etc" / "evergreen_yml_components" / "copybara" / "copybara.yml"
    )

    @unittest.skipUnless(
        REAL_PATH_RULES_MODULE_PATH.is_file()
        and REAL_TEMPLATE_PATH.is_file()
        and REAL_PATH_RULES_PATH.is_file(),
        "checked-in Copybara source-of-truth files not found at expected paths",
    )
    def test_real_path_rules_module_matches_rendered_source_of_truth(self):
        rendered = render_copybara_path_rules_module_from_files(
            self.REAL_TEMPLATE_PATH,
            self.REAL_PATH_RULES_PATH,
        )
        self.assertEqual(self.REAL_PATH_RULES_MODULE_PATH.read_text(), rendered)

    @unittest.skipUnless(
        REAL_SKY_PATH.is_file(),
        "checked-in copy.bara.sky not found at expected path",
    )
    def test_real_base_config_loads_generated_path_rules_module(self):
        contents = self.REAL_SKY_PATH.read_text()
        self.assertIn('load("copybara_path_rules"', contents)
        self.assertIn('"common_files_to_include"', contents)
        self.assertIn('"common_files_to_exclude"', contents)

    @unittest.skipUnless(
        REAL_SKY_PATH.is_file(),
        "checked-in copy.bara.sky not found at expected path",
    )
    def test_real_base_config_does_not_iterate_release_tag_string(self):
        contents = self.REAL_SKY_PATH.read_text()
        self.assertNotIn("for char in version_part:", contents)
        self.assertIn("for char_index in range(len(version_part)):", contents)
        self.assertIn("char = version_part[char_index]", contents)

    @unittest.skipUnless(
        REAL_SKY_PATH.is_file() and REAL_RELEASE_TAG_HELPERS_TEMPLATE_PATH.is_file(),
        "checked-in copy.bara.sky or release tag helper template not found at expected path",
    )
    def test_real_base_config_uses_release_tag_helper_template_source(self):
        contents = self.REAL_SKY_PATH.read_text()
        template = self.REAL_RELEASE_TAG_HELPERS_TEMPLATE_PATH.read_text().strip()
        self.assertIn(template, contents)

    @unittest.skipUnless(
        REAL_PATH_RULES_PATH.is_file(),
        "checked-in Copybara path rules JSON not found at expected path",
    )
    def test_real_path_rules_json_lists_are_sorted(self):
        payload = json.loads(self.REAL_PATH_RULES_PATH.read_text())

        for field_name in ("common_files_to_include", "common_files_to_exclude"):
            with self.subTest(field_name=field_name):
                self.assertEqual(
                    payload[field_name],
                    sorted(payload[field_name]),
                    f"{field_name} in copybara_path_rules.json must remain sorted.",
                )

    @unittest.skipUnless(
        REAL_SKY_PATH.is_file() and REAL_PATH_RULES_PATH.is_file(),
        "checked-in Copybara config files not found at expected paths",
    )
    def test_real_path_rules_label_tracked_and_synthetic_top_level_paths(self):
        try:
            output = subprocess.check_output(
                ["git", "-C", str(self.REAL_COPYBARA_ROOT), "ls-files", "-z"]
            )
        except (FileNotFoundError, subprocess.CalledProcessError) as err:
            self.skipTest(f"could not list tracked files: {err}")

        top_level_paths = {Path(path.decode()).parts[0] for path in output.split(b"\0") if path}
        top_level_paths.add(".copybara_release_fragments")
        top_level_paths.update({"copybara.sky", "copybara.staging.sky"})

        with tempfile.TemporaryDirectory() as tmpdir:
            config_path = Path(tmpdir) / "copy.bara.sky"
            master_fragment_path = (
                self.REAL_COPYBARA_ROOT / "buildscripts" / "copybara" / "master.sky"
            )
            config_path.write_text(
                self.REAL_SKY_PATH.read_text() + "\n" + master_fragment_path.read_text()
            )
            shutil.copy2(
                self.REAL_PATH_RULES_PATH,
                config_path.with_name(sync_repo_with_copybara.PATH_RULES_FILENAME),
            )

            sync_repo_with_copybara.check_branch_top_level_paths_are_labeled(
                str(config_path), "master", top_level_paths
            )

    @unittest.skipUnless(
        REAL_GENERATED_EVERGREEN_PATH.is_file(),
        "checked-in generated Copybara Evergreen config not found at expected path",
    )
    def test_real_generated_evergreen_matches_copybara_fragments(self):
        self.assertEqual(
            self.REAL_GENERATED_EVERGREEN_PATH.read_text(),
            generate_evergreen.render_expected_copybara_evergreen(self.REAL_COPYBARA_ROOT),
        )

    @unittest.skipUnless(
        REAL_EVERGREEN_COMPONENT_PATH.is_file() and REAL_GENERATED_EVERGREEN_PATH.is_file(),
        "checked-in Copybara Evergreen components not found at expected paths",
    )
    def test_real_copybara_test_task_exercises_sync_tag_path(self):
        component_contents = self.REAL_EVERGREEN_COMPONENT_PATH.read_text()
        generated_contents = self.REAL_GENERATED_EVERGREEN_PATH.read_text()
        self.assertIn("--branches=${copybara_branches|master}", component_contents)
        self.assertIn("--test-sync-tag", component_contents)
        self.assertIn("--prod-destination=test", component_contents)
        self.assertIn("copybara_branches: ${copybara_branches|master}", generated_contents)


class TestEvergreenProjectGuard(unittest.TestCase):
    def test_passes_for_expected_master_project(self):
        sync_repo_with_copybara.ensure_expected_evergreen_project(
            {"project": sync_repo_with_copybara.EXPECTED_EVERGREEN_PROJECT}
        )

    def test_fails_for_missing_project(self):
        with self.assertRaises(SystemExit):
            sync_repo_with_copybara.ensure_expected_evergreen_project({})

    def test_fails_for_non_master_project(self):
        with self.assertRaises(SystemExit):
            sync_repo_with_copybara.ensure_expected_evergreen_project(
                {"project": "mongodb-mongo-master-nightly"}
            )


class TestHotfixTaskActivation(unittest.TestCase):
    def test_get_hotfix_branches_for_release(self):
        hotfix_branches = sync_repo_with_copybara.get_hotfix_branches_for_release(
            "v8.2",
            ["master", "v8.2", "v8.2-hotfix", "v8.2.6-hotfix", "v8.0.10-hotfix"],
        )

        self.assertEqual(hotfix_branches, ["v8.2-hotfix", "v8.2.6-hotfix"])

    @patch("buildscripts.copybara.sync_repo_with_copybara.retry_operation")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_evergreen_api")
    @patch("buildscripts.copybara.sync_repo_with_copybara.check_destination_branch_exists")
    @patch("buildscripts.copybara.sync_repo_with_copybara.branch_exists_remote")
    def test_activate_new_hotfix_tasks_activates_unsynced_hotfix_task(
        self,
        mock_branch_exists_remote,
        mock_check_destination_branch_exists,
        mock_get_api,
        mock_retry_operation,
    ):
        mock_branch_exists_remote.return_value = True
        mock_check_destination_branch_exists.return_value = False

        inactive_task = MagicMock()
        inactive_task.display_name = "sync_copybara_v8_2_6_hotfix"
        inactive_task.activated = False
        inactive_task.task_id = "task_1"

        mock_api = mock_get_api.return_value
        mock_api.tasks_by_build.return_value = [inactive_task]

        with tempfile.TemporaryDirectory() as tmpdir:
            config_file = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                config_file,
                prod_url=sync_repo_with_copybara.TEST_REPO_URL,
            )

            sync_repo_with_copybara.activate_new_hotfix_tasks(
                selected_branches=["v8.2"],
                configured_branches=["master", "v8.2", "v8.2.6-hotfix"],
                expansions={"build_id": "build_1"},
                tokens_map={
                    sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                    sync_repo_with_copybara.TEST_REPO_URL: "test-token",
                },
                config_file=config_file,
            )

        mock_retry_operation.assert_called_once_with(
            mock_api.configure_task,
            "task_1",
            activated=True,
            tries=3,
            delay_seconds=5,
            backoff_factor=2,
        )
        self.assertEqual(
            mock_check_destination_branch_exists.call_args.args[0].destination.repo_name,
            sync_repo_with_copybara.TEST_REPO_NAME,
        )

    @patch("buildscripts.copybara.sync_repo_with_copybara.retry_operation")
    @patch("buildscripts.copybara.sync_repo_with_copybara.get_evergreen_api")
    @patch("buildscripts.copybara.sync_repo_with_copybara.check_destination_branch_exists")
    @patch("buildscripts.copybara.sync_repo_with_copybara.branch_exists_remote")
    def test_activate_new_hotfix_tasks_skips_existing_public_branch(
        self,
        mock_branch_exists_remote,
        mock_check_destination_branch_exists,
        mock_get_api,
        mock_retry_operation,
    ):
        mock_branch_exists_remote.return_value = True
        mock_check_destination_branch_exists.return_value = True
        mock_get_api.return_value.tasks_by_build.return_value = []

        with tempfile.TemporaryDirectory() as tmpdir:
            config_file = Path(tmpdir) / "copy.bara.sky"
            write_base_copybara_config(
                config_file,
                prod_url=sync_repo_with_copybara.TEST_REPO_URL,
            )

            sync_repo_with_copybara.activate_new_hotfix_tasks(
                selected_branches=["v8.2"],
                configured_branches=["master", "v8.2", "v8.2.6-hotfix"],
                expansions={"build_id": "build_1"},
                tokens_map={
                    sync_repo_with_copybara.SOURCE_REPO_URL: "source-token",
                    sync_repo_with_copybara.PUBLIC_GITHUB_APP_REPO_URL: "prod-token",
                    sync_repo_with_copybara.TEST_REPO_URL: "test-token",
                },
                config_file=config_file,
            )

        mock_retry_operation.assert_not_called()


if __name__ == "__main__":
    unittest.main()
