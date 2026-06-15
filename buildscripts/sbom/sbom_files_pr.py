#!/usr/bin/env python3
"""
Script that opens a PR using a bot to update SBOM-related files.
"""

import argparse
import json
import os
import re
import sys
import time

from github import (
    GithubException,
    GithubIntegration,
    GitRef,
    InputGitTreeElement,
    PullRequest,
    Repository,
)

SBOM_FILES = ["sbom.json", "sbom.private.json", "README.third_party.md"]


def get_repository(github_owner, github_repo, app_id, _private_key) -> Repository.Repository:
    """
    Gets the mongo github repository
    """
    app = GithubIntegration(str(int(app_id)), _private_key)
    installation = app.get_repo_installation(github_owner, github_repo)
    g = installation.get_github_for_installation()
    return g.get_repo(f"{github_owner}/{github_repo}")


def get_pull_request(branch_gitref: GitRef.GitRef) -> PullRequest.PullRequest | None:
    """
    Gets the pull request for the branch ref, if it exists
    """
    pulls = branch_gitref
    print("get_pull_request:")
    for pull in pulls:
        print(" pull: ", pull)
    if pulls.totalCount > 0:
        pull = pulls[0]
        print(f"Found open PR #{pull.number} '{pull.title}'")
        return pull
    else:
        return None


def create_branch(repository, base_branch, new_branch) -> None:
    """
    Create a new branch or get existing branch.
    """
    try:
        print(f"Attempting to create branch '{new_branch}' with base branch '{base_branch}'.")
        ref = f"refs/heads/{new_branch}"
        base_repo_branch = repository.get_branch(base_branch)
        sha = base_repo_branch.commit.sha
        repository.create_git_ref(ref=ref, sha=sha)
        print(f"Created branch '{new_branch}', ref: {ref}, sha: {sha}")
    except GithubException as e:
        if e.status == 422:
            print(f"Branch {new_branch} already exists, ref: {ref}")
        else:
            raise


def read_text_file(path: str) -> str:
    """Read a text file and return as string"""
    try:
        with open(path, "r", encoding="utf-8") as file:
            content = file.read()
        return content
    except FileNotFoundError:
        print(f"ERROR: The file '{path}' was not found.")
        return f"ERROR: The file '{path}' was not found."
    except (OSError, UnicodeDecodeError) as e:
        print(f"An error occurred: {e}")
        return f"ERROR: An error occurred while reading '{path}': {e}"


def summarize_sbom_changes(old_content: str, new_content: str) -> str:
    """Return a markdown bullet list summarising CycloneDX component changes."""
    try:
        old = json.loads(old_content) if old_content else {}
        new = json.loads(new_content)
    except (json.JSONDecodeError, ValueError):
        return "- _(could not parse SBOM JSON)_"

    def by_name(sbom: dict) -> dict:
        return {c["bom-ref"].split("@")[0]: c for c in sbom.get("components", [])}

    old_comps = by_name(old)
    new_comps = by_name(new)
    added = sorted(set(new_comps) - set(old_comps))
    removed = sorted(set(old_comps) - set(new_comps))
    changed = sorted(
        k
        for k in set(old_comps) & set(new_comps)
        if old_comps[k].get("version", "") != new_comps[k].get("version", "")
    )

    def label(comps: dict, keys: list) -> str:
        parts = []
        for k in keys:
            c = comps[k]
            n = c.get("name", k)
            v = c.get("version", "")
            parts.append(f"`{n} {v}`")
        return ", ".join(parts)

    lines = []
    if added:
        lines.append(f"- **Added ({len(added)}):** {label(new_comps, added)}")
    if removed:
        lines.append(f"- **Removed ({len(removed)}):** {label(old_comps, removed)}")
    if changed:
        diffs = [
            f"`{old_comps[k].get('name', k)}` "
            f"{old_comps[k].get('version', '')} → {new_comps[k].get('version', '')}"
            for k in changed
        ]
        lines.append(f"- **Version changed ({len(changed)}):** {', '.join(diffs)}")
    if not lines:
        lines.append("- No component changes")
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        description=(
            "This script checks for changes to SBOM and related files and creates a PR if "
            "files have been updated."
        ),
    )
    parser.add_argument("--github-owner", help="GitHub org/owner (e.g., 10gen).", type=str)
    parser.add_argument("--github-repo", help="GitHub repository name (e.g., mongo).", type=str)
    parser.add_argument(
        "--branch-filter",
        help="Create a PR only if base branch matches regex.",
        type=str,
        default=".*",
    )
    parser.add_argument("--base-branch", help="base branch to merge into.", type=str)
    parser.add_argument("--new-branch", help="New branch for the PR.", type=str)
    parser.add_argument("--pr-title", help="Title for the PR.", type=str)
    parser.add_argument(
        "--saved-warnings", help="Path to file to include as text in PR message.", type=str
    )
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
            "Must define --app-id or env MONGO_PR_BOT_APP_ID and --private-key or env "
            "MONGO_PR_BOT_PRIVATE_KEY."
        )

    # Check if base branch matches the branch filter regex
    if not re.fullmatch(args.branch_filter, args.base_branch):
        print(
            f"Base branch '{args.base_branch}' does not match branch filter '{args.branch_filter}'. Terminating as successful."
        )
        sys.exit(0)

    # Replace spaces with newline, if applicable
    private_key = (
        args.private_key[:31] + args.private_key[31:-29].replace(" ", "\n") + args.private_key[-29:]
    )

    repo = get_repository(args.github_owner, args.github_repo, args.app_id, private_key)
    print("repo: ", repo)

    # Strip the Endor Labs tool version when comparing SBOMs so a version-only bump
    # does not trigger a new PR commit.
    PATTERN = r'{"name":"(Endor Labs Inc|EndorLabsInc)","version":"[^"]*"}'
    REPL = r'{"name":"\1","version":""}'

    # Collect all changed files first so we can commit them in a single commit
    changed_files: list[tuple[str, str]] = []
    original_contents: dict[str, str] = {}  # base-branch content, used for the diff summary

    for file_path in SBOM_FILES:
        print(f"Checking file '{file_path}' on '{args.base_branch}' for changes...")
        # Try to get the existing file from the base branch; 404 means "new file"
        try:
            original_file = repo.get_contents(file_path, ref=f"refs/heads/{args.base_branch}")
            print("original_file: ", original_file)
            original_content = original_file.decoded_content.decode()
        except GithubException as e:
            if e.status in [403, 404]:
                print(f"'{file_path}' does not exist on {args.base_branch}; treating as new file")
                original_content = ""
            else:
                raise

        original_contents[file_path] = original_content

        try:
            with open(file_path, "r", encoding="utf-8") as file:
                new_content = file.read()
        except FileNotFoundError:
            print("Error: file '%s' not found.", file_path)
            continue

        original_content_compare = re.sub(PATTERN, REPL, "".join(original_content.split()))
        new_content_compare = re.sub(PATTERN, REPL, "".join(new_content.split()))

        if original_content_compare != new_content_compare:
            print(f"Detected change in '{file_path}'")
            changed_files.append((file_path, new_content))

    # files_to_commit is the subset of changed_files that also differ from the PR branch.
    # Keeping the two sets separate lets us always maintain the PR when there is a diff
    # vs. the base branch, while avoiding redundant commits on re-runs.
    files_to_commit: list[tuple[str, str]] = []

    if changed_files:
        # Ensure the branch exists (create if needed)
        create_branch(repo, args.base_branch, args.new_branch)

        # Small delay to reduce chance of 409s immediately after branch creation
        time.sleep(5)

        # Determine which files actually differ from what is already on the PR branch so
        # we do not push a no-op commit on every nightly re-run while the PR is open.
        for file_path, new_content in changed_files:
            try:
                pr_file = repo.get_contents(file_path, ref=f"refs/heads/{args.new_branch}")
                pr_content = pr_file.decoded_content.decode()
            except GithubException as e:
                if e.status in [403, 404]:
                    pr_content = ""
                else:
                    raise
            pr_content_compare = re.sub(PATTERN, REPL, "".join(pr_content.split()))
            new_content_compare = re.sub(PATTERN, REPL, "".join(new_content.split()))
            if pr_content_compare != new_content_compare:
                files_to_commit.append((file_path, new_content))

        if files_to_commit:
            # Base commit/tree on the current head of the PR branch
            branch_ref = repo.get_branch(args.new_branch)
            base_commit_sha = branch_ref.commit.sha
            base_commit = repo.get_git_commit(base_commit_sha)
            base_tree = repo.get_git_tree(base_commit_sha)

            # Build tree elements for all changed files in one go
            elements = [
                InputGitTreeElement(
                    path=path,
                    mode="100644",
                    type="blob",
                    content=content,
                )
                for path, content in files_to_commit
            ]

            new_tree = repo.create_git_tree(elements, base_tree)

            commit_message = "Update SBOM-related files: " + ", ".join(
                path for path, _ in files_to_commit
            )
            print("Creating single commit with message:", commit_message)

            new_commit = repo.create_git_commit(commit_message, new_tree, [base_commit])

            # Move branch ref to new commit (single commit containing all file updates)
            ref = repo.get_git_ref(f"heads/{args.new_branch}")
            ref.edit(new_commit.sha)
        else:
            print("PR branch already has the latest SBOM files. Skipping commit.")

    if changed_files:
        # Get open PR or create new PR
        pull_requests = repo.get_pulls(
            state="open", head=f"{args.github_owner}:{args.new_branch}", base=args.base_branch
        )
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
            pr_comment = "### SBOM Component Changes\n\n"
            for file_path, new_content in changed_files:
                if not file_path.endswith(".json"):
                    continue
                pr_comment += f"**{file_path}**\n"
                pr_comment += summarize_sbom_changes(
                    original_contents.get(file_path, ""), new_content
                )
                pr_comment += "\n\n"
            pr_comment += "---\n\n"
            pr_comment += "### The following warnings were output by the SBOM generation script:\n"
            if os.path.isfile(args.saved_warnings):
                pr_comment += read_text_file(args.saved_warnings)

            # Edit the bot's existing comment rather than adding a new one each run,
            # which would clutter long-lived PRs.
            BOT_LOGIN = "mongo-pr-bot[bot]"
            existing_comment = next(
                (
                    c
                    for c in pull_request.get_issue_comments()
                    if c.user.login == BOT_LOGIN
                    and "The following warnings were output by the SBOM generation script" in c.body
                ),
                None,
            )
            if existing_comment:
                existing_comment.edit(pr_comment)
                print("Updated existing PR comment: ", existing_comment)
            else:
                comment = pull_request.create_issue_comment(pr_comment)
                print("Added PR comment: ", comment)
    else:
        print(f"Files '{SBOM_FILES}' have not changed. Skipping PR.")
