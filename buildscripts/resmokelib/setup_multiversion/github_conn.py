"""Helper functions to interact with github."""
from github import Github, GithubException


class GithubConnError(Exception):
    """Errors in github_conn.py."""

    pass


def get_git_tag_and_commit(github_oauth_token, version):
    """Return git tag and commit hash by associating the version with git tag."""

    github = Github(github_oauth_token)
    repo = github.get_repo("mongodb/mongo")

    try:
        git_ref_list = list(repo.get_git_matching_refs(f"tags/r{version}"))
        # If git tag fully matches the version, it will be only one git_ref in the list,
        # otherwise picking up the latest git_ref
        git_ref = git_ref_list[-1]
        git_tag = repo.get_git_tag(git_ref.object.sha)
        git_commit = repo.get_commit(git_tag.object.sha)

    except (GithubException, IndexError):
        raise GithubConnError(f"Commit hash for a version {version} not found.")

    return git_tag.tag, git_commit.sha
