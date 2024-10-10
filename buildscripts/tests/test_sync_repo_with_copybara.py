import os
import sys
import tempfile
import traceback
import unittest

from buildscripts import sync_repo_with_copybara


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

    def test_branch_exists(self):
        """Perform a test to check that the branch exists in a repository."""
        test_name = "branch_exists_test"
        copybara_config = sync_repo_with_copybara.CopybaraConfig(
            source=None,
            destination=sync_repo_with_copybara.CopybaraRepoConfig(
                git_url="https://github.com/mongodb/mongo.git",
                branch="v7.3",
            ),
        )
        result = sync_repo_with_copybara.check_destination_branch_exists(copybara_config)
        self.assertTrue(result, f"{test_name}: SUCCESS!")

    def test_branch_not_exists(self):
        """Perform a test to check that the branch does not exist in a repository."""
        test_name = "branch_not_exists_test"
        copybara_config = sync_repo_with_copybara.CopybaraConfig(
            source=None,
            destination=sync_repo_with_copybara.CopybaraRepoConfig(
                git_url="https://github.com/mongodb/mongo.git",
                branch="..invalid-therefore-impossible-to-create-branch-name",
            ),
        )
        result = sync_repo_with_copybara.check_destination_branch_exists(copybara_config)
        self.assertFalse(result, f"{test_name}: SUCCESS!")

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


if __name__ == "__main__":
    unittest.main()
