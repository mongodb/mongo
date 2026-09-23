#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Validate that the commit message is ok."""

import logging
import pathlib
import re
import subprocess
import sys

import pathspec
import requests
import structlog
import typer
from git import Commit, Repo
from typing_extensions import Annotated

from buildscripts.bazel_rules_mongo.utils.evergreen_git import get_changed_files
from buildscripts.jira_cve_links import unresolved_cves_by_ticket

LOGGER = structlog.get_logger(__name__)

STATUS_OK = 0
STATUS_ERROR = 1

repo_root = pathlib.Path(
    subprocess.run(
        "git rev-parse --show-toplevel", shell=True, text=True, capture_output=True
    ).stdout.strip()
)

pr_template = ""
with open(repo_root / ".github" / "pull_request_template.md", "r") as r:
    pr_template = r.read().strip()

BANNED_STRINGS = ["https://spruce.mongodb.com", "https://evergreen.mongodb.com", pr_template]

VALID_SUMMARY = re.compile(
    r'(?:Revert ")?(?:([A-Z]+)-([A-Z0-9]+)|Import wiredtiger|Bump \S+ from \S+ to \S+)'
)

# A Jira key anywhere in a message, for the linked-CVE check rather than summary validation.
# Matched case-insensitively and normalized to upper case, so "server-123" counts. The
# numeric suffix skips placeholders, and the boundaries keep "utf-8" or "SERVER-12x" out.
JIRA_KEY_RE = re.compile(r"(?<![A-Za-z0-9-])([A-Z]+)-([1-9][0-9]*)(?![0-9A-Za-z-])", re.IGNORECASE)

# The allowed jira projects and their corresponding allowed file paths.
# allowed file paths are in gitignore format
ALLOWED_JIRA_PROJECTS = {
    "SERVER": ["**/*"],  # SERVER commits can modify any file
    "GUARD": ["monguard/**/*"],  # GUARD commits can only modify files in the monguard directory
}

RETRY_INSTRUCTIONS = "If you are seeing this on a PR, after changing the required field, you will need to restart the failed validate_commit_message task in Evergreen before being able to submit your PR."


def is_valid_commit(commit: Commit, changed_files: list[str] = [], requester: str = "") -> bool:
    # Valid values look like:
    # 1. SERVER-\d+
    # 2. Revert "SERVER-\d+
    # 3. Import wiredtiger
    # 4. Revert "Import wiredtiger
    match = VALID_SUMMARY.match(commit.summary)
    if not match:
        LOGGER.error(
            f"""PR summary is not valid; it must match the regular expression: {VALID_SUMMARY}
Current summary: {commit.summary}
Please update the PR title and description to match the expected format.
{RETRY_INSTRUCTIONS}
The decision to add this check was made in SERVER-101443, please feel free to leave comments/feedback on that ticket.""",
        )
        return False

    # get the jira project from the regex, this will be none for wiredtiger imports
    jira_project = match.group(1)
    ticket_suffix = match.group(2)
    if jira_project:
        if requester == "github_pr":
            # For github_pr requests, the suffix can be alphanumeric (3-6 characters)
            # This is to allow SERVER-XXXX tickets that will be auto-replaced after PR creation.
            if not (3 <= len(ticket_suffix) <= 6):
                LOGGER.error(
                    f"""PR summary ticket suffix must be 3-6 characters, got {len(ticket_suffix)}: {ticket_suffix}
{RETRY_INSTRUCTIONS}"""
                )
                return False
        else:
            # For non-github_pr requests, the suffix must be purely numeric.
            if not ticket_suffix.isdigit():
                LOGGER.error(
                    f"""PR summary ticket suffix must be numeric, got: {ticket_suffix}
{RETRY_INSTRUCTIONS}"""
                )
                return False

        # Check that the jira project is in the allowed list
        if jira_project not in ALLOWED_JIRA_PROJECTS:
            LOGGER.error(
                f"""PR summary contains an invalid Jira project {jira_project}; it must be one of: {list(ALLOWED_JIRA_PROJECTS.keys())}
{RETRY_INSTRUCTIONS}"""
            )
            return False

        allowed_file_paths = ALLOWED_JIRA_PROJECTS[jira_project]
        spec = pathspec.PathSpec.from_lines("gitwildmatch", allowed_file_paths)

        for file in changed_files:
            if not spec.match_file(file):
                LOGGER.error(
                    f"""PR summary indicates Jira project {jira_project} but the PR modifies file {file} which is not allowed for that Jira project.
{RETRY_INSTRUCTIONS}"""
                )
                return False

    # Remove all whitespace from comparisons. GitHub line-wraps commit messages, which adds
    # newline characters that otherwise would not match verbatim such banned strings.
    stripped_message = "".join(commit.message.split())
    for banned_string in BANNED_STRINGS:
        if "".join(banned_string.split()) in stripped_message:
            LOGGER.error(
                f"""PR title/description contains a banned string (ignoring whitespace).
Please update the PR title and description to not contain the following banned string:
banned string: "{banned_string}"
commit message: "{commit.message}"
{RETRY_INSTRUCTIONS}
The decision to add this check was made in SERVER-101443, please feel free to leave comments/feedback on that ticket."""
            )
            return False

    return True


def referenced_jira_keys_for_cve_check(commits: list[Commit]) -> dict[str, list[str]]:
    """
    Collect every Jira ticket the change references, not just the one it is filed under.

    Deliberately wider than VALID_SUMMARY, which only looks at the first line because that
    is what the commit is *for*; a linked CVE is worth catching wherever the ticket is
    mentioned, since a fix can reference a second ticket in its description.

    Because the repository squashes, a commit message here already carries the pull request
    title and description: the merge-queue commit is the squashed headline plus body, and the
    pull request path asks GitHub for the text the merge would produce. So scanning messages
    covers the title, the description and the commit messages without any extra API call.

    Two filters keep this from asking Jira about things that are not tickets. Only projects
    in ALLOWED_JIRA_PROJECTS are considered, so a mention of a BF or a CVE key is ignored,
    and suffixes must be numeric, which skips the SERVER-XXXX placeholder a pull request may
    still carry.

    :param commits: Commits already validated by is_valid_commit.
    :return: Jira key -> where it was referenced, in first-seen order.
    """
    found: dict[str, list[str]] = {}
    for commit in commits:
        summary, _, description = commit.message.partition("\n")
        for text, part in ((summary, "summary"), (description, "description")):
            for match in JIRA_KEY_RE.finditer(text):
                jira_project = match.group(1).upper()
                if jira_project not in ALLOWED_JIRA_PROJECTS:
                    continue
                origins = found.setdefault(f"{jira_project}-{match.group(2)}", [])
                origin = f'{part} of "{summary}"'
                if origin not in origins:
                    origins.append(origin)
    return found


def format_cve_failure(jira_key: str, origins: list[str], unresolved: list[str]) -> str:
    """
    Explain one blocking ticket to the author.

    The scan is wider than the ticket the change is filed under, so the key may be an
    incidental mention. Each remediation covers one case: the change is the fix and must
    wait, the reference is unrelated and should go, or the CVE is already out.

    Only Jira keys appear in the output, never summaries or descriptions of the CVE itself.

    :param jira_key: The referenced ticket that blocked the merge.
    :param origins: Where the ticket was referenced.
    :param unresolved: Unresolved linked CVE keys.
    :return: Multi-line message.
    """
    cves = ", ".join(unresolved)
    referenced_in = ", ".join(origins)
    return f"""{jira_key} is linked to unresolved CVE(s): {cves}
Referenced in: {referenced_in}

This safeguard is meant to prevent premature disclosure of security issues on the public repo.

 - If this fix is related to the CVE(s), do not merge to master before publication.
 - If this fix is unrelated to the CVE(s), remove {jira_key} from the {referenced_in}.
 - If the CVE(s) are published, follow the proper process to resolve {cves} before merging.
{RETRY_INSTRUCTIONS}"""


def get_non_merge_queue_squashed_commits(
    github_org: str,
    github_repo: str,
    pr_number: int,
    github_token: str,
) -> list[Commit]:
    assert github_org
    assert github_repo
    assert pr_number >= 0
    assert github_token

    pr_merge_info_query = {
        "query": f"""{{
            repository(owner: "{github_org}", name: "{github_repo}") {{
                pullRequest(number: {pr_number}) {{
                    viewerMergeHeadlineText(mergeType: SQUASH)
                    viewerMergeBodyText(mergeType: SQUASH)
                }}
            }}
         }}"""
    }
    headers = {"Authorization": f"token {github_token}"}

    LOGGER.info("Sending request", request=pr_merge_info_query)
    req = requests.post(
        url="https://api.github.com/graphql",
        json=pr_merge_info_query,
        headers=headers,
        timeout=60,  # 60s
    )
    resp = req.json()
    # Response will look like
    # {'data': {'repository': {'pullRequest':
    # {
    #   'viewerMergeHeadlineText': 'SERVER-1234 Add a ton of great support (#32823)',
    #   'viewerMergeBodyText': 'This PR adds back support for a lot of things\nMany great things!'
    # }}}}
    LOGGER.info("Squashed content", content=resp)
    pr_info = resp["data"]["repository"]["pullRequest"]

    fake_repo = Repo()
    return [
        Commit(
            message="\n".join([pr_info["viewerMergeHeadlineText"], pr_info["viewerMergeBodyText"]]),
            # required fields, but faked out - these aren't helpful in user-facing logs
            repo=fake_repo,
            binsha=b"00000000000000000000",
        )
    ]


def get_merge_queue_commits(branch_name: str) -> list[Commit]:
    assert branch_name

    diff_commits = subprocess.run(
        ["git", "log", '--pretty=format:"%H"', f"{branch_name}...HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    # Comes back like "hash1"\n"hash2"\n...
    commit_hashs: list[str] = diff_commits.stdout.replace('"', "").splitlines()
    LOGGER.info("Diff commit hashes", commit_hashs=commit_hashs)
    repo = Repo(repo_root)

    return [repo.commit(commit_hash) for commit_hash in commit_hashs]


def main(
    github_org: Annotated[
        str,
        typer.Option(envvar="GITHUB_ORG", help="Name of the github organization (e.g. 10gen)"),
    ] = "",
    github_repo: Annotated[
        str,
        typer.Option(envvar="GITHUB_REPO", help="Name of the repo (e.g. mongo)"),
    ] = "",
    branch_name: Annotated[
        str,
        typer.Option(envvar="BRANCH_NAME", help="Name of the branch to compare against HEAD"),
    ] = "",
    pr_number: Annotated[
        int,
        typer.Option(envvar="PR_NUMBER", help="PR Number to compare with"),
    ] = -1,
    github_token: Annotated[
        str,
        typer.Option(envvar="GITHUB_TOKEN", help="Github token with pr read access"),
    ] = "",
    requester: Annotated[
        str,
        typer.Option(
            envvar="REQUESTER",
            help="What is requested this task. Defined https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Project-Configuration-Files#expansions.",
        ),
    ] = "",
    jira_auth_pat: Annotated[
        str,
        typer.Option(
            envvar="JIRA_AUTH_PAT",
            help="Jira token for the CI account. The linked-CVE check is skipped without it.",
        ),
    ] = "",
):
    """
    Validate the commit message.

    It validates the latest message when no arguments are provided.
    """
    # jira_cve_links logs through stdlib logging so it stays dependency-free. Without this
    # its records would be dropped, since stdlib logging defaults to WARNING.
    logging.basicConfig(level=logging.INFO, stream=sys.stdout, format="%(message)s")

    commits: list[Commit] = []
    if requester == "github_merge_queue":
        commits = get_merge_queue_commits(branch_name)
    elif requester == "github_pr":
        commits = get_non_merge_queue_squashed_commits(
            github_org, github_repo, pr_number, github_token
        )
    else:
        LOGGER.error("Running with an invalid requester", requester=requester)
        raise typer.Exit(code=STATUS_ERROR)

    changed_files = get_changed_files("../expansions.yml", diff_filter=None)
    for commit in commits:
        if not is_valid_commit(commit, changed_files, requester):
            LOGGER.error("Invalid commit, unable to merge")
            raise typer.Exit(code=STATUS_ERROR)

    # Only reached once every summary is well formed, so the ticket keys below are known
    # good. A malformed summary is the author's first problem; no point asking Jira about it.
    if not jira_auth_pat:
        LOGGER.warning("JIRA_AUTH_PAT is not set; skipping the linked CVE check")
        return

    jira_keys = referenced_jira_keys_for_cve_check(commits)
    if not jira_keys:
        LOGGER.info("No Jira keys to check for linked CVEs")
        return

    blocking = unresolved_cves_by_ticket(list(jira_keys), jira_auth_pat)
    if not blocking:
        LOGGER.info("No unresolved linked CVEs", checked=list(jira_keys))
        return

    for jira_key, unresolved in blocking.items():
        LOGGER.error(format_cve_failure(jira_key, jira_keys[jira_key], unresolved))
    LOGGER.error("Unresolved linked CVE, unable to merge")
    raise typer.Exit(code=STATUS_ERROR)


app = typer.Typer(pretty_exceptions_show_locals=False)
app.command()(main)

if __name__ == "__main__":
    app()
