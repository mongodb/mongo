#!/usr/bin/env python3
"""Script that opens a PR using a bot to update SBOM-related files."""

import argparse
import os
import re
import time

from github import (
    GithubException,
    GithubIntegration,
    Github,
    Repository,
)

SBOM_FILES = ["sbom.json", "README.third_party.md"]

# pylint: disable=C0103


def get_repository(github_owner, github_repo, app_id, _private_key) -> Repository.Repository:
    """Gets the mongo github repository."""
    app = GithubIntegration(int(app_id), _private_key)
    installation = app.get_repo_installation(github_owner, github_repo)
    installation_auth = app.get_access_token(installation.id, permissions=None)
    gh = Github(installation_auth.token)
    _repo = gh.get_repo(f"{github_owner}/{github_repo}")
    return _repo


def create_branch(base_branch, new_branch, _repo) -> None:
    """Create a new branch or get existing branch."""
    print(f"Attempting to create branch '{new_branch}' with base branch '{base_branch}'.")
    ref = f"refs/heads/{new_branch}"
    try:
        base_repo_branch = _repo.get_branch(base_branch)
        sha = base_repo_branch.commit.sha
        _repo.create_git_ref(ref=ref, sha=sha)
        print(f"Created branch '{new_branch}', ref: {ref}, sha: {sha}")
    except GithubException as ex:
        if ex.status == 422:
            print(f"Branch {new_branch} already exists, ref: {ref}")
        else:
            raise


def read_text_file(file_path_str: str) -> str:
    """Read a text file and return as string."""
    content = ""
    try:
        with open(file_path_str, "r", encoding="utf-8") as _file:
            content = _file.read()
    except FileNotFoundError:
        print(f"ERROR: The file '{file_path_str}' was not found.")
        return f"ERROR: The file '{file_path_str}' was not found."
    except Exception as ex:
        print(f"An error occurred: {ex}")
    return content


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
    print("repo: ", repo)

    HAS_UPDATE = False

    for file_path in SBOM_FILES:
        original_file = repo.get_contents(file_path, ref=f"refs/heads/{args.base_branch}")
        assert not isinstance(original_file, list)
        print("original_file: ", original_file)
        original_content = original_file.decoded_content.decode()
        try:
            with open(file_path, "r", encoding="utf-8") as file:
                new_content = file.read()
        except FileNotFoundError:
            print("Error: file '%s' not found.", file_path)
            new_content = original_content

        # Compare content with removed Endor Labs version to avoid triggering a new SBOM on only that change
        PATTERN = r'{"name":"EndorLabsInc","version":".*"}'
        REPL = r'{"name":"EndorLabsInc","version":""}'
        original_content_compare = re.sub(PATTERN, REPL, "".join(original_content.split()))
        new_content_compare = re.sub(PATTERN, REPL, "".join(new_content.split()))

        if original_content_compare != new_content_compare:
            create_branch(args.base_branch, args.new_branch, repo)
            original_file_new_branch = repo.get_contents(file_path,
                                                         ref=f"refs/heads/{args.new_branch}")
            print("original_file_new_branch: ", original_file_new_branch)

            print("New file is different from original file.")
            print("repo.update_file:")
            print(f"  message: Updating '{file_path}'")
            print("  path: ", file_path)
            assert not isinstance(original_file_new_branch, list)
            print("  sha: ", original_file_new_branch.sha)
            print("  content:")
            print(new_content[:128])
            print("...[truncated]...")
            print(new_content[-128:])
            print("  branch: ", args.new_branch)
            time.sleep(10)  # Wait to reduce chance of 409 errors
            update_file_result = repo.update_file(
                message=f"Updating '{file_path}'",
                path=file_path,
                sha=original_file_new_branch.sha,
                content=new_content,
                branch=args.new_branch,
            )
            print("update_file_result: ", update_file_result)
            commit = update_file_result.get("commit")
            print("commit: ", commit)

            HAS_UPDATE = True

    if HAS_UPDATE:
        # Get open PR or create new PR
        pull_requests = repo.get_pulls(state="open", head=f"{args.github_owner}:{args.new_branch}",
                                       base=args.base_branch)
        if pull_requests.totalCount:
            pull_request = pull_requests[0]
            print("pull_request: ", pull_request)
        else:
            pr_body = "Automated PR updating SBOM and related files."
            print("Creating PR:")
            print(f" title={args.pr_title}")
            print(f" head={args.new_branch}")
            print(f" base={args.base_branch}")
            print(f" body={pr_body}")

            pull_request = repo.create_pull(
                title=args.pr_title,
                head=args.new_branch,
                base=args.base_branch,
                body=pr_body,
            )
            print("pull_request: ", pull_request)

        if args.saved_warnings:
            pr_comment = "The following warnings were output by the SBOM generation script:\n"
            if os.path.isfile(args.saved_warnings):
                pr_comment += read_text_file(args.saved_warnings)
            comment = pull_request.create_issue_comment(pr_comment)
            print("Added PR comment: ", comment)
    else:
        print(f"Files '{SBOM_FILES}' have not changed. Skipping PR.")
