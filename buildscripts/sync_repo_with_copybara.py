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
from pathlib import Path
from typing import NamedTuple, Optional

from github import GithubIntegration

from buildscripts.util.read_config import read_config_file
from evergreen.api import RetryingEvergreenApi

# this will be populated by the github jwt tokens (1 hour lifetimes)
REDACTED_STRINGS = []

# This is the list of file globs to check for 
# after the dryrun has created the destination output tree
EXCLUDED_PATTERNS = [  
    "src/mongo/db/modules/",
    "buildscripts/modules/",
]

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
    def from_copybara_sky_file(cls, workflow: str, branch: str, file_path: str) -> CopybaraConfig:
        with open(file_path, "r") as file:
            content = file.read()
            # Delete comments
            content = re.sub(r"#.*", "", content)

            source_url_match = re.search(r'sourceUrl = "(.+?)"', content)
            if source_url_match is None:
                return cls.empty()

            if workflow == "prod":
                destination_url_match = re.search(r'prodUrl = "(.+?)"', content)
                if destination_url_match is None:
                    return cls.empty()
            else:
                destination_url_match = re.search(r'testUrl = "(.+?)"', content)
                if destination_url_match is None:
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
                    branch=branch,
                ),
                destination=CopybaraRepoConfig(
                    git_url=destination_git_url,
                    repo_name=destination_repo_name_match.group(1),
                    branch=branch,
                ),
            )

    def is_complete(self) -> bool:
        return self.source is not None and self.destination is not None


def run_command(command):  
    print(command)  
    try:  
        process = subprocess.Popen(  
            command,  
            shell=True,  
            stdout=subprocess.PIPE,  
            stderr=subprocess.STDOUT,  # Merge stderr into stdout  
            text=True,  
            bufsize=1  
        )  
  
        output_lines = []  
        for line in process.stdout:  
            for redact in filter(None, REDACTED_STRINGS):  # avoid None replacements  
                line = line.replace(redact, "<REDACTED>")  
            print(line, end="")  
            output_lines.append(line)  
  
        full_output = ''.join(output_lines)  
        process.wait()  
  
        if process.returncode != 0:  
            # Attach output so except block can read it  
            raise subprocess.CalledProcessError(  
                process.returncode, command, output=full_output  
            )  
  
        return full_output  
  
    except subprocess.CalledProcessError:  
        # Let main handle it  
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


def send_failure_message_to_slack(expansions, error_message):
    """
    Send a failure message to a specific Slack channel when the Copybara task fails.

    :param expansions: Dictionary containing various expansion data.
    """
    truncated_error_message = error_message[0:200]
    task_id = expansions.get("task_id", None)
    error_msg = "\n".join(
        [
            "Evergreen task '* Copybara Sync Between Repos' failed",
            "See troubleshooting doc <http://go/copybara-troubleshoot|here>.",
            f"See task: <https://spruce.mongodb.com/task/{task_id}|here>.",
            f"Error message: {truncated_error_message}"
            + ("... (truncated)" if len(error_message) > 200 else ""),
        ]
    )

    evg_api = RetryingEvergreenApi.get_api(config_file=".evergreen.yml")
    evg_api.send_slack_message(
        target="#devprod-build-automation",
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

def handle_failure(expansions, error_message, output_logs):
    acceptable_error_messages = [
        # Indicates the two repositories are identical
        "No new changes to import for resolved ref",
        # Indicates differences exist but no changes affect the destination, for example: exclusion rules
        "Iterative workflow produced no changes in the destination for resolved ref",
        # Indicates the commits have already been synced over with another copybara task
        "Updates were rejected because the remote contains work that you do",
    ]

    if not any(
        acceptable_message in output_logs for acceptable_message in acceptable_error_messages
    ):
        send_failure_message_to_slack(expansions, error_message)

def create_branch_from_matching_commit(copybara_config: CopybaraConfig) -> None:
    """
    Create a new branch in the copybara destination repository based on a matching commit found in
    source repository and destination repository.

    Args:
        copybara_config (CopybaraConfig): Copybara configuration.
    """

    # Save original directory
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

def is_current_repo_origin(expected_repo: str) -> bool:  
    """Check if the current repo's origin matches 'owner/repo'."""  
    try:  
        url = run_command("git config --get remote.origin.url").strip()  
    except subprocess.CalledProcessError:  
        return False  
    m = re.search(r"([^/:]+/[^/:]+)\.git$", url)  
    return bool(m and m.group(1) == expected_repo)  
  

def sky_file_has_version_id(config_file: str, version_id: str) -> bool:  
    contents = Path(config_file).read_text()  
    return str(version_id) in contents  

def branch_exists_remote(remote_url: str, branch_name: str) -> bool:  
    """Return True if branch exists on the remote."""  
    try:  
        output = run_command(f"git ls-remote --heads {remote_url} {branch_name}")  
        return bool(output.strip())  
    except subprocess.CalledProcessError:  
        return False  
  
def delete_remote_branch(remote_url: str, branch_name: str):  
    """Delete branch from remote if it exists."""  
    if branch_exists_remote(remote_url, branch_name):  
        print(f"Deleting remote branch {branch_name} from {remote_url}")  
        run_command(f"git push {remote_url} --delete {branch_name}")  
  
def push_test_branches(copybara_config, expansions):  
    """Push test branch with Evergreen patch changes to source, and clean revision to destination."""  
    # Safety checks  
    if copybara_config.source.branch != copybara_config.destination.branch:  
        print(f"ERROR: test branches must match: source={copybara_config.source.branch} dest={copybara_config.destination.branch}")  
        sys.exit(1)  
    if not copybara_config.source.branch.startswith("copybara_test_branch") \
        or not copybara_config.destination.branch.startswith("copybara_test_branch"):  
        print(f"ERROR: can not push non copybara test branch: {copybara_config.source.branch}")  
        sys.exit(1)  
    if not is_current_repo_origin("10gen/mongo"):          
        print("Refusing to push copybara_test_branch to non 10gen/mongo repo")  
        sys.exit(1)  
  
    # First, delete stale remote branches if present  
    delete_remote_branch(copybara_config.source.git_url, copybara_config.source.branch)  
    delete_remote_branch(copybara_config.destination.git_url, copybara_config.destination.branch)  
  
    # --- Push patched branch to DEST repo (local base Evergreen state) --- 
    run_command(f"git remote add dest_repo {copybara_config.destination.git_url}")   
    run_command(f"git checkout -B {copybara_config.destination.branch}")
    run_command(f"git push dest_repo {copybara_config.destination.branch}")

    # --- Push patched branch to SOURCE repo (local patched Evergreen state) --- 
    run_command(f'git commit -am "Evergreen patch for version_id {expansions["version_id"]}"')    
    run_command(f"git remote add source_repo {copybara_config.source.git_url}")  
    run_command(f"git push source_repo {copybara_config.source.branch}")   

def main():
    global REDACTED_STRINGS
    """Clone the Copybara repo, build its Docker image, and set up and run migrations."""
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--expansions-file",
        "-e",
        default="../expansions.yml",
        help="Location of expansions file generated by evergreen.",
    )

    parser.add_argument(
        "--workflow",
        default="test",
        choices = ["prod", "test"],
        help="The copybara workflow to use (test is a dryrun)",
    )

    args = parser.parse_args()

    # Check if the copybara directory already exists
    if os.path.exists("copybara"):
        print("Copybara directory already exists.")
    else:
        run_command("git clone https://github.com/10gen/copybara.git")

    # Navigate to the Copybara directory and build the Copybara Docker image
    run_command("cd copybara && docker build --rm -t copybara_container .")

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

    REDACTED_STRINGS += [token_mongodb_mongo, token_10gen_mongo]

    tokens_map = {
        "https://github.com/mongodb/mongo.git": token_mongodb_mongo,
        "https://github.com/10gen/mongo.git": token_10gen_mongo,
        "https://github.com/10gen/mongo-copybara.git": token_10gen_mongo,
    }

    # Create the mongodb-bot.gitconfig file as necessary.
    create_mongodb_bot_gitconfig()

    current_dir = os.getcwd()
    config_file = f"{current_dir}/copy.bara.sky"

    if args.workflow == "test":
        test_args = ["--init-history", f"--last-rev={expansions['revision']}"]
        branch = f"copybara_test_branch_{expansions['version_id']}"
        test_branch_str = 'testBranch = "copybara_test_branch"'
    elif args.workflow == "prod":
        if expansions['is_patch'] == "true":
            print("ERROR: prod workflow should not be run in patch builds!")
            sys.exit(1)
        test_args = []
        branch = "master"
    else:
        raise Exception(f"invalid workflow {args.workflow}")

    # Overwrite repo urls in copybara config in-place
    with fileinput.FileInput(config_file, inplace=True) as file:  
        for line in file:  
            token = None  
    
            # Replace GitHub URL with token-authenticated URL  
            for repo, value in tokens_map.items():  
                if repo in line:  
                    token = value  
                    break  # no need to check other repos  
    
            if token:  
                print(  
                    line.replace(  
                        "https://github.com",  
                        f"https://x-access-token:{token}@github.com",  
                    ),  
                    end="",  
                )  
    
            # Update testBranch in .sky file if running test workflow  
            elif args.workflow == "test" and test_branch_str in line:  
                print(  
                    line.replace(  
                        test_branch_str,  
                        test_branch_str[:-1] + f"_{expansions['version_id']}\"\n",
                    ),  
                    end="",  
                )  
    
            else:  
                print(line, end="") 

    if args.workflow == "test":  
        if not sky_file_has_version_id(config_file, expansions["version_id"]):  
            print(  
                f"Copybara test branch in {config_file} does not contain version_id {expansions['version_id']}"  
            )  
            sys.exit(1)

    copybara_config = CopybaraConfig.from_copybara_sky_file(args.workflow, branch, config_file)

    if args.workflow == "test":
        push_test_branches(copybara_config, expansions)

    # Create destination branch if it does not exist
    if not copybara_config.is_complete():
        print("ERROR!!!")
        print(
            f"ERROR!!! Source or destination configuration could not be parsed from the {config_file}."
        )
        print("ERROR!!!")
        sys.exit(1)
    else:
        if args.workflow == "prod":
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

    os.makedirs("tmp_copybara")

    docker_cmd = [  
        "docker", "run", "--rm",  
        "-v", f"{os.path.expanduser('~/.ssh')}:/root/.ssh",  
        "-v", f"{os.path.expanduser('~/mongodb-bot.gitconfig')}:/root/.gitconfig",  
        "-v", f"{config_file}:/usr/src/app/copy.bara.sky",
        "-v", f"{os.getcwd()}/tmp_copybara:/tmp/copybara-preview",
        "copybara_container",
        "migrate", "/usr/src/app/copy.bara.sky", args.workflow,  
        "-v", "--output-root=/tmp/copybara-preview",
    ]

    try:
        run_command(" ".join(docker_cmd + ["--dry-run"] + test_args))

        
        found_forbidden = False  
        preview_dir = Path("tmp_copybara")  
  
        
        for file_path in preview_dir.rglob("*"):  
            if file_path.is_file():  
                for pattern in EXCLUDED_PATTERNS: 
                    if pattern in str(file_path):  
                        print(f"ERROR: Found excluded path: {file_path}")  
                        found_forbidden = True  
        
        if found_forbidden:  
            sys.exit(1)  
    except subprocess.CalledProcessError as err:
        if args.workflow == "prod":
            error_message = f"Copybara failed with error: {err.returncode}" 
            handle_failure(expansions, error_message, err.output)

    # dry run successful, time to push
    try:  
        run_command(" ".join(docker_cmd + test_args))  
    except subprocess.CalledProcessError as err:  
        if args.workflow == "prod":  
            error_message = f"Copybara failed with error: {err.returncode}"  
            handle_failure(expansions, error_message, err.output)


if __name__ == "__main__":
    main()
