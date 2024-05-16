"""Module for syncing a repo with Copybara and setting up configurations."""

from __future__ import annotations

import argparse
import subprocess
import os
import sys
import re
import shutil
from datetime import datetime

from typing import NamedTuple, Optional
from github import GithubIntegration

from buildscripts.util.read_config import read_config_file
from evergreen.api import RetryingEvergreenApi


class CopybaraBranchNames(NamedTuple):
    """Copybara sync branch names."""

    source: Optional[str]
    destination: Optional[str]

    @classmethod
    def empty(cls) -> CopybaraBranchNames:
        return cls(
            source=None,
            destination=None,
        )

    @classmethod
    def from_copybara_sky_file(cls, file_path: str) -> CopybaraBranchNames:
        source_branch_name_pattern = re.compile(r'ref = "(.+?)"')
        destination_branch_name_pattern = re.compile(r'push = "(.+?)"')

        with open(file_path, "r") as file:
            content = file.read()
            source_branch_name_match = source_branch_name_pattern.search(content)
            destination_branch_name_match = destination_branch_name_pattern.search(content)

            # Check if both required patterns were found and return them
            if source_branch_name_match and destination_branch_name_match:
                return cls(
                    source=source_branch_name_match.group(1),
                    destination=destination_branch_name_match.group(1),
                )

        # Return empty if any pattern is not found
        return cls.empty()

    def is_complete(self) -> bool:
        return self.source and self.destination


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


def check_destination_branch_exists(repo_url: str, branch_name: str) -> bool:
    """
    Check if a specific branch exists in a remote git repository.

    Args:
    - repo_url (str): The URL of the remote git repository.
    - branch_name (str): The name of the branch to check for existence.

    Returns
    - bool: `True` if the branch exists in the remote repository, `False` otherwise.
    """

    command = f"git ls-remote {repo_url} {branch_name}"
    output = run_command(command)
    return branch_name in output


def find_matching_commit(dir_10gen_mongo: str, dir_mongodb_mongo: str) -> str:
    """
    Finds a matching commit in the dir_mongodb_mongo repository based on the private hash from the dir_10gen_mongo repository.

    Args:
    - dir_10gen_mongo: The directory of the 10gen-mongo repository.
    - dir_mongodb_mongo: The directory of the mongodb-mongo repository.

    Returns
    The hash of the matching commit if found; otherwise, prints a message and returns None.
    """

    # Navigate to the 10gen-mongo repository
    os.chdir(dir_10gen_mongo)

    # Find the latest commit hash.
    private_hash = run_command('git log --pretty=format:"%H" -1')

    # Attempt to find a matching commit in the mongodb-mongo repository.
    commit = run_command(
        f'git --git-dir={dir_mongodb_mongo}/.git log -1 --pretty=format:"%H" --grep "GitOrigin-RevId: {private_hash}"'
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

        # Revert to the previous commit in the 10gen-mongo repository and try again.
        run_command("git checkout HEAD~1")
        private_hash = run_command('git log --pretty=format:"%H" -1')

        # Attempt to find a matching commit again in the mongodb-mongo repository.
        commit = run_command(
            f'git --git-dir={dir_mongodb_mongo}/.git log -1 --pretty=format:"%H" --grep "GitOrigin-RevId: {private_hash}"'
        )
    return commit


def is_only_mongodb_mongo_repo():
    """
    Check if the current directory's Git repository only contains the 'mongodb/mongo.git' remote URL.

    Returns
        bool: True if the repository only contains the 'mongodb/mongo.git' remote URL, False otherwise.
    """
    git_config_path = os.path.join(".git", "config")
    with open(git_config_path, "r") as f:
        config_content = f.read()

        # Define a regular expression pattern to match the mongodb/mongo.git
        url_pattern = r"url\s*=\s*(.*?\.git\s*)"
        matches = re.findall(url_pattern, config_content)

        if len(matches) == 1 and matches[0].strip().endswith("mongodb/mongo.git"):
            return True
    print(
        "The current directory's Git repository does not only contain the 'mongodb/mongo.git' remote URL."
    )
    return False


def push_branch_to_public_repo(
    mongodb_mongo_dir: str, repo_url: str, destination_branch_name: str, branching_off_commit: str
):
    """
    Pushes a new branch to the remote repository after ensuring it branches off the public repository.

    Args:
        mongodb_mongo_dir (str): Path to the cloned 'mongodb-mongo' repository.
        repo_url (str): The URL of the remote repository where the new branch will be pushed.
        destination_branch_name (str): The name for the new branch to be pushed to the remote repository.
        commit (str): The commit hash of the matching commit in the 'mongodb-mongo' repository.

    Raises
        Exception: If the new branch is not branching off the public repository.
    """

    os.chdir(mongodb_mongo_dir)

    # Check the current repo is only mongodb/mongo repo.
    if not is_only_mongodb_mongo_repo():
        raise Exception("Not only mongodb repo")

    # Confirm the top commit is matching the found commit before pushing
    new_branch_top_commit = run_command('git log --pretty=format:"%H" -1')
    if not new_branch_top_commit == branching_off_commit:
        raise Exception(
            "The new branch top commit does not match the branching_off_commit. Aborting push."
        )

    # Confirming whether the commit exists in the remote public repository (mongodb/mongo/master) to ensure we are not pushing anything that isn't already in the public repository.
    # run_command will raise an exception if the commit is not found in the remote branch.
    run_command(f"git branch -r --contains {new_branch_top_commit}")

    # Push the new branch to the remote repository
    run_command(f"git push {repo_url} {destination_branch_name}")


def create_branch_from_matching_commit(
    repo_url: str, source_branch_name: str, destination_branch_name: str
) -> None:
    """
    Creates a new branch in the mongodb/mongo repository based on a matching commit found in 10gen/mongo/branches and mongodb/mongo/master.

    Args:
        repo_url (str): The URL of the remote repository where the new branch will be pushed.
        source_branch_name (str): The name of the branch in the '10gen-mongo' repository.
        destination_branch_name (str): The name for the new branch to be created in the 'mongodb-mongo' repository.
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

        # Clone the specified branch of the 10gen/mongo and master of mongodb/mongo repo
        cloned_10gen_mongo_dir = os.path.join(working_dir, "10gen-mongo")
        cloned_mongodb_mongo_dir = os.path.join(working_dir, "mongodb-mongo")

        run_command(
            f"git clone -b {source_branch_name} git@github.com:10gen/mongo.git {cloned_10gen_mongo_dir}"
        )
        run_command(f"git clone git@github.com:mongodb/mongo.git {cloned_mongodb_mongo_dir}")

        # Find matching commits to branching off
        commit = find_matching_commit(cloned_10gen_mongo_dir, cloned_mongodb_mongo_dir)
        if commit is not None:
            # Delete the cloned_10gen_mongo_dir folder
            shutil.rmtree(cloned_10gen_mongo_dir)
            if os.path.exists(cloned_10gen_mongo_dir):
                raise Exception(cloned_10gen_mongo_dir + ": did not get removed")

            # Once a matching commit is found, create a new branch based on it.
            os.chdir(cloned_mongodb_mongo_dir)
            run_command(f"git checkout -b {destination_branch_name} {commit}")

            # Push the new branch to the remote repository
            push_branch_to_public_repo(
                cloned_mongodb_mongo_dir, repo_url, destination_branch_name, commit
            )
        else:
            print(
                f"Could not find matching commits between mongodb/mongo/master and 10gen/mongo/{destination_branch_name} to branching off"
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

    access_token_copybara_syncer = get_installation_access_token(
        expansions["app_id_copybara_syncer"],
        expansions["private_key_copybara_syncer"],
        expansions["installation_id_copybara_syncer"],
    )

    # Create the mongodb-bot.gitconfig file as necessary.
    create_mongodb_bot_gitconfig()

    current_dir = os.getcwd()
    config_file = f"{current_dir}/copybara.sky"
    git_destination_url_with_token = (
        f"https://x-access-token:{access_token_copybara_syncer}@github.com/mongodb/mongo.git"
    )

    # create destination branch if not exists
    copybara_branch_names = CopybaraBranchNames.from_copybara_sky_file(config_file)
    if not copybara_branch_names.is_complete():
        print("ERROR!!!")
        print(
            f"ERROR!!! Source or destination branch name could not be parsed from the {config_file}."
        )
        print("ERROR!!!")
        sys.exit(1)
    else:
        if not check_destination_branch_exists(
            git_destination_url_with_token, copybara_branch_names.destination
        ):
            create_branch_from_matching_commit(
                git_destination_url_with_token,
                copybara_branch_names.source,
                copybara_branch_names.destination,
            )
            print(
                f"New branch named {copybara_branch_names.destination} has been created for the mongodb/mongo repo"
            )
        else:
            print(
                f"The branch named {copybara_branch_names.destination} already exists in the mongodb/mongo repo."
            )

    # Set up the Docker command and execute it
    docker_cmd = [
        "docker run",
        "-v ~/.ssh:/root/.ssh",
        "-v ~/mongodb-bot.gitconfig:/root/.gitconfig",
        f'-v "{config_file}":/usr/src/app/copy.bara.sky',
        "-e COPYBARA_CONFIG='copy.bara.sky'",
        "-e COPYBARA_SUBCOMMAND='migrate'",
        f"-e COPYBARA_OPTIONS='-v --git-destination-url={git_destination_url_with_token}'",
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
