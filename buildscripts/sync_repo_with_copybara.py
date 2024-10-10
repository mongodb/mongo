"""Module for syncing a repo with Copybara and setting up configurations."""

from __future__ import annotations

import argparse
import fileinput
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from typing import NamedTuple, Optional

from github import GithubIntegration

from buildscripts.util.read_config import read_config_file
from evergreen.api import RetryingEvergreenApi


class CopybaraRepoConfig(NamedTuple):
    """Copybara source and destination repo sync configuration."""

    git_url: Optional[str] = None
    repo_name: Optional[str] = None
    branch: Optional[str] = None


class CopybaraConfig(NamedTuple):
    """Copybara sync configuration."""

    source: Optional[CopybaraRepoConfig] = None
    destination: Optional[CopybaraRepoConfig] = None

    @classmethod
    def empty(cls) -> CopybaraConfig:
        return cls(
            source=None,
            destination=None,
        )

    @classmethod
    def from_copybara_sky_file(cls, file_path: str) -> CopybaraConfig:
        with open(file_path, "r") as file:
            content = file.read()
            # Delete comments
            content = re.sub(r"#.*", "", content)

            source_url_match = re.search(r'sourceUrl = "(.+?)"', content)
            if source_url_match is None:
                return cls.empty()

            source_branch_name_match = re.search(r'ref = "(.+?)"', content)
            if source_branch_name_match is None:
                return cls.empty()

            destination_url_match = re.search(r'destinationUrl = "(.+?)"', content)
            if destination_url_match is None:
                return cls.empty()

            destination_branch_name_match = re.search(r'push = "(.+?)"', content)
            if destination_branch_name_match is None:
                return cls.empty()

            repo_name_regex = re.compile(r"([^:/]+/[^:/]+)\.git")

            source_git_url = source_url_match.group(1)
            source_repo_name_match = repo_name_regex.search(source_git_url)
            if source_repo_name_match is None:
                return cls.empty()

            destination_git_url = destination_url_match.group(1)
            destination_repo_name_match = repo_name_regex.search(destination_git_url)
            if destination_repo_name_match is None:
                return cls.empty()

            return cls(
                source=CopybaraRepoConfig(
                    git_url=source_git_url,
                    repo_name=source_repo_name_match.group(1),
                    branch=source_branch_name_match.group(1),
                ),
                destination=CopybaraRepoConfig(
                    git_url=destination_git_url,
                    repo_name=destination_repo_name_match.group(1),
                    branch=destination_branch_name_match.group(1),
                ),
            )

    def is_complete(self) -> bool:
        return self.source is not None and self.destination is not None


def run_command(command):
    """
    Execute a shell command and return its standard output (`stdout`).

    Args:
        command (str): The shell command to be executed.

    Returns
        str: The standard output of the executed command.

    Raises
        subprocess.CalledProcessError: If the command execution fails.

    """
    try:
        return subprocess.run(
            command, shell=True, check=True, text=True, capture_output=True
        ).stdout
    except subprocess.CalledProcessError as e:
        print(f"Error while executing: '{command}'.\n{e}\nStandard Error: {e.stderr}")
        raise


def create_mongodb_bot_gitconfig():
    """Create the mongodb-bot.gitconfig file with the desired content."""

    content = """
    [user]
        name = MongoDB Bot
        email = mongo-bot@mongodb.com
    """

    gitconfig_path = os.path.expanduser("~/mongodb-bot.gitconfig")

    with open(gitconfig_path, "w") as file:
        file.write(content)

    print("mongodb-bot.gitconfig file created.")


def get_installation_access_token(
    app_id: int, private_key: str, installation_id: int
) -> Optional[str]:  # noqa: D407,D413
    """
    Obtain an installation access token using JWT.

    Args:
    - app_id (int): The application ID for GitHub App.
    - private_key (str): The private key associated with the GitHub App.
    - installation_id (int): The installation ID of the GitHub App for a particular account.

    Returns
    - Optional[str]: The installation access token. Returns `None` if there's an error obtaining the token.

    """
    integration = GithubIntegration(app_id, private_key)
    auth = integration.get_access_token(installation_id)

    if auth:
        return auth.token
    else:
        print("Error obtaining installation token")
        return None


def send_failure_message_to_slack(expansions):
    """
    Send a failure message to a specific Slack channel when the Copybara task fails.

    :param expansions: Dictionary containing various expansion data.
    """
    current_version_id = expansions.get("version_id", None)
    error_msg = (
        "Evergreen task '* Copybara Sync Between Repos' failed\n"
        "See troubleshooting doc <http://go/copybara-troubleshoot|here>.\n"
        f"See task log here: <https://spruce.mongodb.com/version/{current_version_id}|here>."
    )

    evg_api = RetryingEvergreenApi.get_api(config_file=".evergreen.yml")
    evg_api.send_slack_message(
        target="#sdp-triager",
        msg=error_msg,
    )


def check_destination_branch_exists(copybara_config: CopybaraConfig) -> bool:
    """
    Check if a specific branch exists in the destination git repository.

    Args:
    - copybara_config (CopybaraConfig): Copybara configuration.

    Returns
    - bool: `True` if the branch exists in the destination repository, `False` otherwise.
    """

    command = (
        f"git ls-remote {copybara_config.destination.git_url} {copybara_config.destination.branch}"
    )
    output = run_command(command)
    return copybara_config.destination.branch in output


def find_matching_commit(dir_source_repo: str, dir_destination_repo: str) -> Optional[str]:
    """
    Finds a matching commit in the destination repository based on the commit hash from the source repository.

    Args:
    - dir_source_repo: The directory of the source repository.
    - dir_destination_repo: The directory of the destination repository.

    Returns
    The hash of the matching commit if found; otherwise, prints a message and returns None.
    """

    # Navigate to the source repository
    os.chdir(dir_source_repo)

    # Find the latest commit hash.
    source_hash = run_command('git log --pretty=format:"%H" -1')

    # Attempt to find a matching commit in the destination repository.
    commit = run_command(
        f'git --git-dir={dir_destination_repo}/.git log -1 --pretty=format:"%H" --grep "GitOrigin-RevId: {source_hash}"'
    )

    first_commit = run_command("git rev-list --max-parents=0 HEAD")

    # Loop until a matching commit is found or the first commit is reached.
    while len(commit.splitlines()) != 1:
        current_commit = run_command('git log --pretty=format:"%H" -1')

        if current_commit.strip() == first_commit.strip():
            print(
                "No matching commit found, and have reverted to the first commit of the repository."
            )
            return None

        # Revert to the previous commit in the source repository and try again.
        run_command("git checkout HEAD~1")
        source_hash = run_command('git log --pretty=format:"%H" -1')

        # Attempt to find a matching commit again in the destination repository.
        commit = run_command(
            f'git --git-dir={dir_destination_repo}/.git log -1 --pretty=format:"%H" --grep "GitOrigin-RevId: {source_hash}"'
        )
    return commit


def has_only_destination_repo_remote(repo_name: str):
    """
    Check if the current directory's Git repository only contains the destination repository remote URL.

    Returns
        bool: True if the repository only contains the destination repository remote URL, False otherwise.
    """
    git_config_path = os.path.join(".git", "config")
    with open(git_config_path, "r") as f:
        config_content = f.read()

        # Define a regular expression pattern to match the '{owner}/{repo}.git'
        url_pattern = r"url\s*=\s*(.*?\.git\s*)"
        matches = re.findall(url_pattern, config_content)

        if len(matches) == 1 and matches[0].strip().endswith(f"{repo_name}.git"):
            return True
    print(
        f"The current directory's Git repository contains not only the '{repo_name}.git' remote URL."
    )
    return False


def push_branch_to_destination_repo(
    destination_repo_dir: str, copybara_config: CopybaraConfig, branching_off_commit: str
):
    """
    Pushes a new branch to the remote repository after ensuring it branches off the public repository.

    Args:
        destination_repo_dir (str): Path to the cloned destination repository.
        copybara_config (CopybaraConfig): Copybara configuration.
        branching_off_commit (str): The commit hash of the matching commit in the destination repository.

    Raises
        Exception: If the new branch is not branching off the destination repository.
    """

    os.chdir(destination_repo_dir)

    # Check the current repo has only destination repository remote.
    if not has_only_destination_repo_remote(copybara_config.destination.repo_name):
        raise Exception(f"{destination_repo_dir} git repo has not only the destination repo remote")

    # Confirm the top commit is matching the found commit before pushing
    new_branch_top_commit = run_command('git log --pretty=format:"%H" -1')
    if not new_branch_top_commit == branching_off_commit:
        raise Exception(
            "The new branch top commit does not match the branching_off_commit. Aborting push."
        )

    # Confirming whether the commit exists in the destination repository to ensure
    # we are not pushing anything that isn't already in the destination repository.
    # run_command will raise an exception if the commit is not found in the destination branch.
    run_command(f"git branch -r --contains {new_branch_top_commit}")

    # Push the new branch to the destination repository
    run_command(
        f"git push {copybara_config.destination.git_url} {copybara_config.destination.branch}"
    )


def create_branch_from_matching_commit(copybara_config: CopybaraConfig) -> None:
    """
    Create a new branch in the copybara destination repository based on a matching commit found in
    source repository and destination repository.

    Args:
        copybara_config (CopybaraConfig): Copybara configuration.
    """

    # Save original dirtory
    original_dir = os.getcwd()

    try:
        # Create a unique directory based on the current timestamp.
        working_dir = os.path.join(
            original_dir, "make_branch_attempt_" + datetime.now().strftime("%Y_%m_%d_%H_%M_%S")
        )
        os.makedirs(working_dir, exist_ok=True)
        os.chdir(working_dir)

        # Clone the specified branch of the source repository and master of destination repository
        cloned_source_repo_dir = os.path.join(working_dir, "source-repo")
        cloned_destination_repo_dir = os.path.join(working_dir, "destination-repo")

        run_command(
            f"git clone -b {copybara_config.source.branch}"
            f" {copybara_config.source.git_url} {cloned_source_repo_dir}"
        )
        run_command(
            f"git clone {copybara_config.destination.git_url} {cloned_destination_repo_dir}"
        )

        # Find matching commits to branching off
        commit = find_matching_commit(cloned_source_repo_dir, cloned_destination_repo_dir)
        if commit is not None:
            # Delete the cloned_source_repo_dir folder
            shutil.rmtree(cloned_source_repo_dir)
            if os.path.exists(cloned_source_repo_dir):
                raise Exception(cloned_source_repo_dir + ": did not get removed")

            # Once a matching commit is found, create a new branch based on it.
            os.chdir(cloned_destination_repo_dir)
            run_command(f"git checkout -b {copybara_config.destination.branch} {commit}")

            # Push the new branch to the remote repository
            push_branch_to_destination_repo(cloned_destination_repo_dir, copybara_config, commit)
        else:
            print(
                f"Could not find matching commits between {copybara_config.destination.repo_name}/master"
                f" and {copybara_config.source.repo_name}/{copybara_config.source.branch} to branching off"
            )
            sys.exit(1)
    except Exception as err:
        print(f"An error occurred when creating destination branch: {err}")
        raise
    finally:
        # Change back to the original directory
        os.chdir(original_dir)


def main():
    """Clone the Copybara repo, build its Docker image, and set up and run migrations."""
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--expansions-file",
        "-e",
        default="../expansions.yml",
        help="Location of expansions file generated by evergreen.",
    )

    args = parser.parse_args()

    # Check if the copybara directory already exists
    if os.path.exists("copybara"):
        print("Copybara directory already exists.")
    else:
        run_command("git clone https://github.com/10gen/copybara.git")

    # Navigate to the Copybara directory and build the Copybara Docker image
    run_command("cd copybara && docker build --rm -t copybara .")

    # Read configurations
    expansions = read_config_file(args.expansions_file)

    token_mongodb_mongo = get_installation_access_token(
        expansions["app_id_copybara_syncer"],
        expansions["private_key_copybara_syncer"],
        expansions["installation_id_copybara_syncer"],
    )
    token_10gen_mongo = get_installation_access_token(
        expansions["app_id_copybara_syncer_10gen"],
        expansions["private_key_copybara_syncer_10gen"],
        expansions["installation_id_copybara_syncer_10gen"],
    )

    tokens_map = {
        "https://github.com/mongodb/mongo.git": token_mongodb_mongo,
        "https://github.com/10gen/mongo.git": token_10gen_mongo,
    }

    # Create the mongodb-bot.gitconfig file as necessary.
    create_mongodb_bot_gitconfig()

    current_dir = os.getcwd()
    config_file = f"{current_dir}/copy.bara.sky"

    # Overwrite repo urls in copybara config in-place
    with fileinput.FileInput(config_file, inplace=True) as file:
        for line in file:
            token = None
            for repo, value in tokens_map.items():
                if repo in line:
                    token = value

            if token:
                print(
                    line.replace(
                        "https://github.com",
                        f"https://x-access-token:{token}@github.com",
                    ),
                    end="",
                )
            else:
                print(line, end="")

    copybara_config = CopybaraConfig.from_copybara_sky_file(config_file)

    # Create destination branch if it does not exist
    if not copybara_config.is_complete():
        print("ERROR!!!")
        print(
            f"ERROR!!! Source or destination configuration could not be parsed from the {config_file}."
        )
        print("ERROR!!!")
        sys.exit(1)
    else:
        if not check_destination_branch_exists(copybara_config):
            create_branch_from_matching_commit(copybara_config)
            print(
                f"New branch named '{copybara_config.destination.branch}' has been created"
                f" for the '{copybara_config.destination.repo_name}' repo"
            )
        else:
            print(
                f"The branch named '{copybara_config.destination.branch}' already exists"
                f" in the '{copybara_config.destination.repo_name}' repo."
            )

    # Set up the Docker command and execute it
    docker_cmd = [
        "docker run",
        "-v ~/.ssh:/root/.ssh",
        "-v ~/mongodb-bot.gitconfig:/root/.gitconfig",
        f'-v "{config_file}":/usr/src/app/copy.bara.sky',
        "-e COPYBARA_CONFIG='copy.bara.sky'",
        "-e COPYBARA_SUBCOMMAND='migrate'",
        "-e COPYBARA_OPTIONS='-v'",
        "copybara copybara",
    ]

    try:
        run_command(" ".join(docker_cmd))
    except subprocess.CalledProcessError as err:
        error_message = str(err.stderr)
        acceptable_error_messages = [
            # Indicates the two repositories are identical
            "No new changes to import for resolved ref",
            # Indicates differences exist but no changes affect the destination, for example: exclusion rules
            "Iterative workflow produced no changes in the destination for resolved ref",
            # Indicates the commits have already been synced over with another copybara task
            "Updates were rejected because the remote contains work that you do",
        ]

        if any(
            acceptable_message in error_message for acceptable_message in acceptable_error_messages
        ):
            return

        # Send a failure message to #sdp-triager if the Copybara sync task fails.
        send_failure_message_to_slack(expansions)


if __name__ == "__main__":
    main()
