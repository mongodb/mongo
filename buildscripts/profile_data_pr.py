#!/usr/bin/env python3
"""
Script that opens a PR using a bot to update profile data links for PGO, CSPGO, and BOLT.
This updates profiling_data.bzl and is reliant on the formatting of it to not change.

Two invocation modes:
  1. PGO + BOLT (original): 3 positional URLs (bolt, clang_pgo, gcc_pgo). Expects exactly
     one of clang_pgo / gcc_pgo to be populated so only one is updated at a time.
  2. CSPGO (--cspgo_url): updates only the CSPGO URL + checksum. Orthogonal to the PGO/BOLT
     flow;

The PGO + BOLT mode runs once per architecture (--arch). All invocations for the same base
revision share a bot branch (the branch name is derived from the revision), each updating
only its own architecture's BOLT entries, so concurrent arm64 and x86_64 upload tasks
converge on a single PR. PGO entries are shared across architectures and only updated by
the aarch64 invocation.
"""

import argparse
import hashlib
import os
import re
import sys
import tempfile

import requests
from github.GithubException import GithubException
from github.GithubIntegration import GithubIntegration
from jira import JIRAError

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from buildscripts.client.jiraclient import JiraAuth, JiraClient

OWNER_NAME = "10gen"
REPO_NAME = "mongo"
PROFILE_DATA_FILE_PATH = "bazel/repository_rules/profiling_data.bzl"
JIRA_SERVER = "https://jira.mongodb.org"
PROFILE_DATA_OWNING_TEAM = "Product Performance"
ARCH_TO_BOLT_SUFFIX = {
    "aarch64": "ARM64",
    "x86_64": "X86_64",
}


def get_mongo_repository(app_id, private_key):
    """
    Gets the mongo github repository
    """
    app = GithubIntegration(str(int(app_id)), private_key)
    installation = app.get_repo_installation(OWNER_NAME, REPO_NAME)
    g = installation.get_github_for_installation()
    return g.get_repo(f"{OWNER_NAME}/{REPO_NAME}")


def compute_sha256(file_path: str) -> str:
    """
    Compute the sha256 hash of a file
    """
    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256.update(block)
    return sha256.hexdigest()


def download_file(url: str, output_location: str) -> bool:
    """
    Download a file to a specific output_location and return if the file existed remotely
    """
    try:
        response = requests.get(url)
        response.raise_for_status()
        with open(output_location, "wb") as file:
            file.write(response.content)
        return True
    except requests.exceptions.RequestException:
        return False


def replace_quoted_text_in_tagged_line(text: str, tag: str, new_text: str) -> str:
    """
    Replace the text between quotes in a line that starts with a specific tag
    eg. FOO = "replace_this" -> FOO = "new_text"

    The match is anchored to the whole tag at the start of a line so that tags which are
    prefixes of other tags (eg. ..._URL vs ..._URL_X86_64) cannot collide, and empty
    current values are replaced correctly.
    """
    pattern = rf'^({re.escape(tag)}\s*=\s*")[^"]*(")'
    replaced, count = re.subn(
        pattern, lambda match: match.group(1) + new_text + match.group(2), text, flags=re.M
    )
    if count == 0:
        print(f"Tag: {tag} did not exist in the file.", file=sys.stderr)
        sys.exit(1)
    return replaced


def update_bolt_info(file_content: str, new_url: str, new_checksum: str, arch_suffix: str) -> str:
    """
    Updates the bolt url and checksum lines for one architecture in a file
    """
    bolt_url_tag = f"DEFAULT_BOLT_DATA_URL_{arch_suffix}"
    bolt_checksum_tag = f"DEFAULT_BOLT_DATA_CHECKSUM_{arch_suffix}"
    updated_text = replace_quoted_text_in_tagged_line(file_content, bolt_url_tag, new_url)
    return replace_quoted_text_in_tagged_line(updated_text, bolt_checksum_tag, new_checksum)


def update_clang_pgo_info(file_content: str, new_url: str, new_checksum: str) -> str:
    """
    Updates the clang pgo url and checksum lines in a file
    """
    clang_pgo_url_tag = "DEFAULT_CLANG_PGO_DATA_URL"
    clang_pgo_checksum_tag = "DEFAULT_CLANG_PGO_DATA_CHECKSUM"
    updated_text = replace_quoted_text_in_tagged_line(file_content, clang_pgo_url_tag, new_url)
    return replace_quoted_text_in_tagged_line(updated_text, clang_pgo_checksum_tag, new_checksum)


def update_gcc_pgo_info(file_content: str, new_url: str, new_checksum: str) -> str:
    """
    Updates the gcc pgo url and checksum lines in a file
    """
    gcc_pgo_url_tag = "DEFAULT_GCC_PGO_DATA_URL"
    gcc_pgo_checksum_tag = "DEFAULT_GCC_PGO_DATA_CHECKSUM"
    updated_text = replace_quoted_text_in_tagged_line(file_content, gcc_pgo_url_tag, new_url)
    return replace_quoted_text_in_tagged_line(updated_text, gcc_pgo_checksum_tag, new_checksum)


def update_clang_cspgo_info(file_content: str, new_url: str, new_checksum: str) -> str:
    """
    Updates the clang cspgo url and checksum lines in a file
    """
    clang_cspgo_url_tag = "DEFAULT_CLANG_CSPGO_DATA_URL"
    clang_cspgo_checksum_tag = "DEFAULT_CLANG_CSPGO_DATA_CHECKSUM"
    updated_text = replace_quoted_text_in_tagged_line(file_content, clang_cspgo_url_tag, new_url)
    return replace_quoted_text_in_tagged_line(updated_text, clang_cspgo_checksum_tag, new_checksum)


def create_backport_ticket(version: str):
    jira = JiraClient(JIRA_SERVER, JiraAuth(), dry_run=False)
    jira = jira._jira
    server_issue_dict = {
        "project": {"key": "SERVER"},
        "issuetype": {"name": "Task"},
        "summary": "Update PGO profiles",
        "description": "Updated PGO profile numbers for performance.",
        "customfield_12751": [{"value": PROFILE_DATA_OWNING_TEAM}],
    }
    backport_issue_dict = {
        "project": {"key": "BACKPORT"},
        "issuetype": {"name": "Backport"},
        "summary": f"[{version}] Update PGO profiles",
        # Branch
        "customfield_14166": {"value": version},
        # Backport Justification
        "customfield_25156": "Updated PGO profile numbers for performance.",
    }
    for attempt in range(3):
        try:
            server_issue = jira.create_issue(fields=server_issue_dict)
            backport_issue = jira.create_issue(fields=backport_issue_dict)
            # For some reason you cant assign a team on creation for backport tickets
            backport_issue.update({"customfield_12751": [{"value": PROFILE_DATA_OWNING_TEAM}]})
            jira.create_issue_link(
                type="backported by", inwardIssue=server_issue.key, outwardIssue=backport_issue.key
            )
            break
        except JIRAError as err:
            print(err)
            return None
    return server_issue


def ensure_branch(repo, target_branch: str, new_branch: str):
    """
    Ensure the shared bot branch exists, tolerating concurrent creation by another
    architecture's upload task.
    """
    try:
        repo.get_branch(branch=new_branch)
        return
    except GithubException as e:
        if e.status != 404:
            raise
    try:
        print(f"Branch doesn't exist, creating branch {new_branch}.")
        target_repo_branch = repo.get_branch(target_branch)
        repo.create_git_ref(ref=f"refs/heads/{new_branch}", sha=target_repo_branch.commit.sha)
    except GithubException as e:
        # 422: another task created the ref between our check and our create.
        if e.status != 422:
            raise
        print(f"Branch {new_branch} was created concurrently, using it.")


def commit_file_update(repo, new_branch: str, apply_edits, max_attempts: int = 3):
    """
    Commit the result of apply_edits(content) to new_branch via the contents API.

    The contents API is a compare-and-swap on the file's blob sha, so a concurrent commit
    from another architecture's task surfaces as a 409. The architectures edit disjoint
    tagged lines, so refetching and reapplying our edits on top always merges cleanly.
    """
    for attempt in range(max_attempts):
        branch_file = repo.get_contents(PROFILE_DATA_FILE_PATH, ref=f"refs/heads/{new_branch}")
        new_content = apply_edits(branch_file.decoded_content.decode())
        try:
            repo.update_file(
                path=PROFILE_DATA_FILE_PATH,
                content=new_content,
                branch=new_branch,
                message="Updating profile files.",
                sha=branch_file.sha,
            )
            return
        except GithubException as e:
            if e.status != 409 or attempt == max_attempts - 1:
                raise
            print(f"Concurrent update to {new_branch} detected, retrying.")


def ensure_pr(repo, target_branch: str, new_branch: str):
    """
    Ensure an open PR exists for the bot branch.

    create_pull is the atomic guard against concurrent tasks: it fails with 422 when an
    open PR for this head already exists, so exactly one task creates the PR. The
    get_pulls pre-check just avoids filing duplicate backport tickets in the common
    (non-racing) case.
    """
    if (
        repo.get_pulls(
            state="open", base=target_branch, head=f"{OWNER_NAME}:{new_branch}"
        ).totalCount
        > 0
    ):
        print(f"An open PR for {new_branch} already exists, not creating another.")
        return

    jira_ticket = "SERVER-110427"
    # This is a versioned backport branch if it stats with v
    if target_branch != "master" and target_branch[0] == "v":
        # get v8.0 from either v8.0 or v8.0-staging
        version = target_branch.split("-")[0]
        new_ticket = create_backport_ticket(version)
        if new_ticket:
            jira_ticket = new_ticket.key
        else:
            jira_ticket = "[Jira Ticket Creation Broken]"

    try:
        repo.create_pull(
            base=target_branch,
            head=new_branch,
            title=f"{jira_ticket} Update profiling data",
            body="Automated PR updating the profiling data.",
        )
    except GithubException as e:
        message = str(e.data)
        if e.status == 422 and "pull request already exists" in message:
            print(f"PR for {new_branch} was created concurrently, nothing to do.")
        elif e.status == 422 and "No commits between" in message:
            print(
                f"{new_branch} has no diff against {target_branch}; profile data is already up to date."
            )
        else:
            raise


def create_profile_data_pr(repo, args, target_branch, new_branch):
    """
    Apply this architecture's profile updates to the shared bot branch and ensure a PR
    exists for it. Concurrent invocations for other architectures share the branch and
    the PR; each edits only its own architecture's lines.
    """
    arch_suffix = ARCH_TO_BOLT_SUFFIX[args.arch]
    temp_dir = tempfile.mkdtemp()
    bolt_file = os.path.join(temp_dir, "bolt.fdata")

    # This is not an error because the script can run when no files were meant to be updated.
    if not download_file(args.bolt_url, bolt_file):
        print(f"Bolt file did not exist at {args.bolt_url}. Not creating PR.")
        sys.exit(0)
    bolt_checksum = compute_sha256(bolt_file)

    # PGO profiles are IR-level and shared across architectures; only the aarch64
    # invocation updates them. Other architectures consume the same profile, so
    # re-uploading it under their own URL would just churn the shared entries.
    clang_pgo_update = None
    gcc_pgo_update = None
    if args.arch == "aarch64":
        clang_pgo_file = os.path.join(temp_dir, "clang_pgo.profdata")
        gcc_pgo_file = os.path.join(temp_dir, "gcc_pgo.tgz")
        clang_pgo_file_exists = download_file(args.clang_pgo_url, clang_pgo_file)
        gcc_pgo_file_exists = download_file(args.gcc_pgo_url, gcc_pgo_file)

        if clang_pgo_file_exists and gcc_pgo_file_exists:
            print(
                f"Both clang and gcc had pgo files that existed. Clang: {args.clang_pgo_url} GCC: {args.gcc_pgo_url}. Only one should be updated at a time. Not creating PR."
            )
            sys.exit(1)

        if not clang_pgo_file_exists and not gcc_pgo_file_exists:
            print(
                f"Neither clang nor gcc had pgo files that existed at either {args.clang_pgo_url} or {args.gcc_pgo_url}. Not creating PR."
            )
            sys.exit(0)

        if clang_pgo_file_exists:
            clang_pgo_update = (args.clang_pgo_url, compute_sha256(clang_pgo_file))
        else:
            gcc_pgo_update = (args.gcc_pgo_url, compute_sha256(gcc_pgo_file))

    def apply_edits(content: str) -> str:
        content = update_bolt_info(content, args.bolt_url, bolt_checksum, arch_suffix)
        if clang_pgo_update:
            content = update_clang_pgo_info(content, *clang_pgo_update)
        if gcc_pgo_update:
            content = update_gcc_pgo_info(content, *gcc_pgo_update)
        return content

    ensure_branch(repo, target_branch, new_branch)
    commit_file_update(repo, new_branch, apply_edits)
    ensure_pr(repo, target_branch, new_branch)


def create_cspgo_pr(repo, cspgo_url: str, target_branch: str, new_branch: str):
    """
    Download the cspgo profdata, compute its checksum, and open a PR updating only
    the CSPGO url/checksum in profiling_data.bzl.
    """
    temp_dir = tempfile.mkdtemp()
    cspgo_file = os.path.join(temp_dir, "clang_cspgo.profdata")

    if not download_file(cspgo_url, cspgo_file):
        print(f"CSPGO file did not exist at {cspgo_url}. Not creating PR.")
        sys.exit(0)
    cspgo_checksum = compute_sha256(cspgo_file)

    def apply_edits(content: str) -> str:
        return update_clang_cspgo_info(content, cspgo_url, cspgo_checksum)

    ensure_branch(repo, target_branch, new_branch)
    commit_file_update(repo, new_branch, apply_edits)
    ensure_pr(repo, target_branch, new_branch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="This script uses bolt file url, clang pgo file url and gcc pgo file url to create a PR updating the links to these files. Pass --cspgo_url instead to update only the CSPGO entries."
    )
    parser.add_argument(
        "bolt_url", nargs="?", help="URL that BOLT data was uploaded to.", default=None
    )
    parser.add_argument(
        "clang_pgo_url", nargs="?", help="URL that clang pgo data was uploaded to.", default=None
    )
    parser.add_argument(
        "gcc_pgo_url", nargs="?", help="URL that gcc pgo data was uploaded to.", default=None
    )
    parser.add_argument("target_branch", help="The branch you want to create a PR into.")
    parser.add_argument("new_branch", help="The new branch to create a PR from.")
    parser.add_argument(
        "--cspgo_url",
        help="URL that clang cspgo data was uploaded to. When set, only the CSPGO entries are updated.",
        default=None,
    )
    parser.add_argument(
        "--arch",
        choices=sorted(ARCH_TO_BOLT_SUFFIX),
        default="aarch64",
        help="Architecture whose BOLT entries should be updated. PGO entries are shared across architectures and only updated when this is aarch64.",
    )
    parser.add_argument(
        "--app_id", help="App ID used for authentication.", default=os.getenv("MONGO_PR_BOT_APP_ID")
    )
    parser.add_argument(
        "--private_key",
        help="Key to use for authentication.",
        default=os.getenv("MONGO_PR_BOT_PRIVATE_KEY"),
    )
    args = parser.parse_args()
    if not args.app_id or not args.private_key:
        parser.error(
            "Must define --app-id or env MONGO_PR_BOT_APP_ID and --private-key or env MONGO_PR_BOT_PRIVATE_KEY."
        )
    if not args.cspgo_url and not (args.bolt_url and args.clang_pgo_url and args.gcc_pgo_url):
        parser.error(
            "Must provide either --cspgo_url for a CSPGO-only PR, or bolt_url/clang_pgo_url/gcc_pgo_url positional args for a PGO+BOLT PR."
        )
    # Replace spaces with newline, if applicable
    private_key = (
        args.private_key[:31] + args.private_key[31:-29].replace(" ", "\n") + args.private_key[-29:]
    )
    repo = get_mongo_repository(args.app_id, private_key)
    if args.cspgo_url:
        create_cspgo_pr(repo, args.cspgo_url, args.target_branch, args.new_branch)
    else:
        create_profile_data_pr(repo, args, args.target_branch, args.new_branch)
