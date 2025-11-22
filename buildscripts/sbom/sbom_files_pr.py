#!/usr/bin/env python3
"""Script that opens a PR using a bot to update SBOM-related files."""

import argparse
import os
import time

from github import Github, Repository
from github.GithubException import GithubException
from github.GithubIntegration import GithubIntegration

SBOM_FILES = ["sbom.json", "README.third_party.md"]


def get_repository(github_owner, github_repo, app_id, _private_key):
    """Gets the mongo github repository."""
    app = GithubIntegration(int(app_id), _private_key)
    installation = app.get_repo_installation(github_owner, github_repo)
    installation_auth = app.get_access_token(installation.id, permissions=None)
    gh = Github(installation_auth.token)
    _repo = gh.get_repo(f"{github_owner}/{github_repo}")
    return _repo


def create_branch(base_branch, new_branch) -> None:
    """Creates a new branch."""
    try:
        assert repo is not None
        print(f"Attempting to create branch '{new_branch}' with base branch '{base_branch}'.")
        base_repo_branch = repo.get_branch(base_branch)
        ref = f"refs/heads/{new_branch}"
        repo.create_git_ref(ref=ref, sha=base_repo_branch.commit.sha)
        print("Created branch.")
    except GithubException as ex:
        if ex.status == 422:
            print("Branch already exists. Continuing...")
        else:
            raise


def read_text_file(_file_path: str) -> str:
    """Read a text file and return as string."""
    try:
        with open(_file_path, "r", encoding="utf-8") as _file:
            content = _file.read()
        return content
    except FileNotFoundError:
        print(f"ERROR: The file '{_file_path}' was not found.")
        return f"ERROR: The file '{_file_path}' was not found."
    except Exception as err:
        print(f"An error occurred: {err}")
        return f"An error occurred: {err}"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description=
        "This script checks for changes to SBOM and related files and creats a PR if files have been updated.",
    )
    parser.add_argument("--github-owner", help="GitHub org/owner (e.g., 10gen).", type=str)
    parser.add_argument("--github-repo", help="GitHub repository name (e.g., mongo).", type=str)
    parser.add_argument("--base-branch", help="base branch to merge into.", type=str)
    parser.add_argument("--new-branch", help="New branch for the PR.", type=str)
    parser.add_argument("--pr-title", help="Title for the PR.", type=str)
    parser.add_argument("--saved-warnings", help="Path to file to include as text in PR message.",
                        type=str)
    parser.add_argument(
        "--app-id",
        help="GitHub App ID used for authentication.",
        type=str,
        default=os.getenv("MONGO_PR_BOT_APP_ID"),
    )
    parser.add_argument(
        "--private-key",
        help="Key to use for GitHub App authentication.",
        type=str,
        default=os.getenv("MONGO_PR_BOT_PRIVATE_KEY"),
    )
    args = parser.parse_args()

    if not args.app_id or not args.private_key:
        parser.error(
            "Must define --app-id or env MONGO_PR_BOT_APP_ID and --private-key or env MONGO_PR_BOT_PRIVATE_KEY."
        )

    # Replace spaces with newline, if applicable
    private_key = (args.private_key[:31] + args.private_key[31:-29].replace(" ", "\n") +
                   args.private_key[-29:])

    repo = get_repository(args.github_owner, args.github_repo, args.app_id, private_key)
    assert isinstance(repo, Repository.Repository)
    assert repo is not None
    print("Repo: ", repo)

    PR_NEEDED = False

    for file_path in SBOM_FILES:
        original_file = repo.get_contents(file_path, ref=f"refs/heads/{args.base_branch}")
        assert not isinstance(original_file, list)
        original_content = original_file.decoded_content.decode()
        try:
            with open(file_path, "r", encoding="utf-8") as file:
                NEW_CONTENT = file.read()
        except FileNotFoundError:
            print("Error: file '%s' not found.", file_path)
            NEW_CONTENT = ""
        # compare strings without whitespace
        if "".join(NEW_CONTENT.split()) != "".join(original_content.split()):
            create_branch(args.base_branch, args.new_branch)
            original_file_new_branch = repo.get_contents(file_path,
                                                         ref=f"refs/heads/{args.new_branch}")

            print("New file is different from original file.")
            print("repo.update_file:")
            print(f"  message: Updating '{file_path}'")
            print(f"  path: '{file_path}'")
            assert not isinstance(original_file_new_branch, list)
            print(f"  sha: {original_file_new_branch.sha}")
            print(f"  content: '{NEW_CONTENT:.256}'")
            print(f"  branch: {args.new_branch}")
            time.sleep(10)  # Wait to reduce chance of 409 errors
            update_file_result = repo.update_file(
                message=f"Updating '{file_path}'",
                path=file_path,
                sha=original_file_new_branch.sha,
                content=NEW_CONTENT,
                branch=args.new_branch,
            )
            print("Results:")
            print("  commit: ", update_file_result["commit"])

            PR_NEEDED = True

    if PR_NEEDED:
        PR_BODY = "Automated PR updating SBOM and related files."
        if args.saved_warnings:
            PR_BODY += "\n\nThe following warnings were output by the SBOM generation script:\n"
            PR_BODY += read_text_file(args.saved_warnings)

        print("Creating PR:")
        print(f"base={args.base_branch}")
        print(f"head={args.new_branch}")
        print(f"title={args.pr_title}")
        print(f"body={PR_BODY}")

        repo.create_pull(
            base=args.base_branch,
            head=args.new_branch,
            title=args.pr_title,
            body=PR_BODY,
        )
    else:
        print(f"Files '{SBOM_FILES}' have not changed. Skipping PR.")
