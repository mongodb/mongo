import argparse
import sys
from typing import Optional
from github import Github, GithubIntegration, GithubException

from buildscripts.util.read_config import read_config_file
from simple_report import put_report, make_report


def get_installation_access_token(app_id: int, private_key: str,
                                  installation_id: int) -> Optional[str]:  # noqa: D406
    """
    Obtain an installation access token using JWT.

    Args:
    - app_id: The application ID for GitHub App.
    - private_key: The private key associated with the GitHub App.
    - installation_id: The installation ID of the GitHub App for a particular account.

    Returns:
    - Optional[str]: The installation access token. Returns `None` if there's an error obtaining the token.
    """
    integration = GithubIntegration(app_id, private_key)
    auth = integration.get_access_token(installation_id)
    if auth:
        return auth.token
    else:
        print("Error obtaining installation token")
        return None


def get_users_who_forked_mongo_repo(owner: str, repo: str, token: str) -> list[str]:  # noqa: D406
    """
    Retrieve list of users who have forked a particular repository.

    Args:
    - owner: The owner of the repository.
    - repo: The name of the repository.
    - token: The GitHub authentication token.

    Returns:
    - list[str]: A list of usernames who have forked the given repository.
    """
    github_client = Github(token)
    repository = github_client.get_repo(f"{owner}/{repo}")

    # If a repo is archived it is read only so it is safe to be allowed
    return [fork.owner.login for fork in repository.get_forks() if not fork.archived]


def is_10gen_member(user: str, org: str, token: str) -> bool:  # noqa: D406
    """
    Check if a user is a member of a particular organization.

    Args:
    - user: The username to check.
    - org: The organization name to check against.
    - token: The GitHub authentication token.

    Returns:
    - bool: True if the user is a member of the organization, otherwise False.
    """
    try:
        github_client = Github(token)
        organization = github_client.get_organization(org)
        user_obj = github_client.get_user(user)
        return organization.has_in_members(user_obj)
    except GithubException:
        return False


def main():
    # Set up argument parsing
    parser = argparse.ArgumentParser(description='Monitor forks of MongoDB repo by 10gen members.')
    parser.add_argument("-l", "--log-file", type=str, default="mongo_fork_from_10gen",
                        help="Log file for storing output.")
    parser.add_argument("--expansions-file", "-e", default="../expansions.yml",
                        help="Expansions file to read GitHub app credentials from.")
    args = parser.parse_args()

    # Read configurations
    expansions = read_config_file(args.expansions_file)

    # Obtain installation access tokens using app credentials
    access_token_mongodb_forks = get_installation_access_token(
        expansions["app_id_mongodb_forks"], expansions["private_key_mongodb_forks"],
        expansions["installation_id_mongodb_forks"])
    access_token_10gen_member = get_installation_access_token(
        expansions["app_id_10gen_member"], expansions["private_key_10gen_member"],
        expansions["installation_id_10gen_member"])

    if not access_token_mongodb_forks or not access_token_10gen_member:
        print("Error obtaining the installation tokens.")
        return

    # Retrieve list of users who forked mongodb/mongo repo
    forked_users = get_users_who_forked_mongo_repo('mongodb', 'mongo', access_token_mongodb_forks)
    print(f"Recent forks info: {forked_users}")

    # Filter out users who are members of the specified organization
    members_from_10gen = [
        user for user in forked_users if is_10gen_member(user, '10gen', access_token_10gen_member)
    ]
    # Sort so list is easier to see diffs of time over time
    members_from_10gen.sort()

    # Generate report message
    if members_from_10gen:
        users_list = [f"+ {user}" for user in members_from_10gen]
        users_list_message = (
            "For each of these names, please make a BF and assign it to that user.\n\n"
            "Users who recently forked mongodb/mongo and are members of 10gen:\n" +
            '\n'.join(users_list))
        print(users_list_message)
    else:
        users_list_message = "No users who recently forked mongodb/mongo are members of 10gen."

    # Make report
    exit_code = 1 if members_from_10gen else 0
    put_report(make_report(args.log_file, users_list_message, exit_code))

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
