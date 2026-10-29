#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Validate a pull request's commit series so rebase merges get squash-level coverage.

This task always assumes the pull request will be rebase merged, whether it
ends up squashed or not, and validates what that would land.

The commit series is read from the pull request on GitHub, not from git
history: this task runs as a patch build, where the checkout is just the base
revision with the patch applied, so Evergreen has no knowledge of the pull
request's commits. The same is true of the pull request text, which rebase
merge does not land in any commit message.

Squash merge collapses a pull request into a single commit, so validating the
squashed headline covers the title, the description and the commit messages at
once. Rebase merge lands each commit verbatim, so the same coverage requires
looking at every commit plus the pull request text:

 * every commit message must satisfy the same rules as a squashed commit
   (see validate_commit_message.is_valid_commit),
 * every commit description must name the ticket the branch was developed
   against with a "feature branch ticket: SERVER-XXXX" line, and every commit
   must name the same ticket, so the series can be reverted as a unit,
 * the pull request title and description are checked for banned strings and
   scanned for referenced Jira keys,
 * the linked-CVE gate runs over all of the above.

Under an actual squash merge these checks degrade gracefully: the commit
series collapses to the one squashed commit, whose body is the pull request
title and description, so the feature-branch-ticket line must appear there and
the same-ticket check holds trivially.
"""

import logging
import re
import sys

import requests
import structlog
import typer
from git import Commit, Repo
from typing_extensions import Annotated

from buildscripts.bazel_rules_mongo.utils.evergreen_git import get_changed_files
from buildscripts.jira_cve_links import unresolved_cves_by_ticket
from buildscripts.validate_commit_message import (
    RETRY_INSTRUCTIONS,
    find_banned_string,
    format_cve_failure,
    is_valid_commit,
    referenced_jira_keys_for_cve_check,
)

LOGGER = structlog.get_logger(__name__)

STATUS_OK = 0
STATUS_ERROR = 1

# GitHub serves the pull request commit connection in pages of at most 100.
PAGE_SIZE = 100
# Refuse to validate a commit series longer than this.
MAX_COMMITS = 1000

# Every commit in a rebased PR must carry the ticket the branch was developed
# against, and all commits must agree on it, so the series can be identified
# (and reverted) as a unit.
FEATURE_BRANCH_TICKET_RE = re.compile(
    r"feature branch ticket:\s*(SERVER-[1-9][0-9]*)", re.IGNORECASE
)


def feature_branch_tickets(commit: Commit) -> list[str]:
    """Return the tickets named on "feature branch ticket:" lines, first-seen order."""
    _, _, description = commit.message.partition("\n")
    return [match.group(1).upper() for match in FEATURE_BRANCH_TICKET_RE.finditer(description)]


def check_feature_branch_tickets(commits: list[Commit]) -> str | None:
    """Check every commit names one shared feature branch ticket.

    :return: None when valid, otherwise a user-facing error message.
    """
    missing = [commit for commit in commits if not feature_branch_tickets(commit)]
    if missing:
        return f"""Every commit in a rebased pull request must contain a "feature branch ticket: SERVER-XXXX" line in its description, naming the ticket the branch was developed against.
{len(missing)} commit(s) are missing it, e.g. the summary "{missing[0].summary}".
{RETRY_INSTRUCTIONS}"""

    tickets = {ticket for commit in commits for ticket in feature_branch_tickets(commit)}
    if len(tickets) > 1:
        return f"""All commits in a rebased pull request must name the same feature branch ticket, but found {len(tickets)}: {", ".join(sorted(tickets))}.
Rebase the branch so every commit belongs to the same ticket, or drop commits for other tickets.
{RETRY_INSTRUCTIONS}"""

    return None


def fetch_pr_text(github_org: str, github_repo: str, pr_number: int, github_token: str) -> str:
    """Fetch the pull request title and description, which rebase merge does not land."""
    query = {
        "query": f"""{{
            repository(owner: "{github_org}", name: "{github_repo}") {{
                pullRequest(number: {pr_number}) {{
                    title
                    body
                }}
            }}
         }}"""
    }
    headers = {"Authorization": f"token {github_token}"}

    LOGGER.info("Fetching pull request text")
    req = requests.post(
        url="https://api.github.com/graphql",
        json=query,
        headers=headers,
        timeout=60,  # 60s
    )
    resp = req.json()
    pr_info = resp["data"]["repository"]["pullRequest"]
    return "\n".join([pr_info["title"], pr_info["body"] or ""])


def get_pr_commits(
    github_org: str, github_repo: str, pr_number: int, github_token: str
) -> list[Commit]:
    """
    Read the pull request's commit series from GitHub.

    This task runs as a patch build, where the checkout has no git history to
    diff against - only the base revision with the patch applied - so the
    commits a rebase merge would land can only come from the pull request
    itself. GitHub serves the commit connection in pages, so the series is
    walked with the returned cursors, up to MAX_COMMITS commits.

    :return: One Commit per pull request commit, oldest first, with the
    message reassembled from the headline and body GitHub stores separately.
    """
    headers = {"Authorization": f"token {github_token}"}

    LOGGER.info("Fetching pull request commits", pr_number=pr_number)
    nodes = []
    cursor = None
    while True:
        query = {
            "query": f"""query($cursor: String) {{
                repository(owner: "{github_org}", name: "{github_repo}") {{
                    pullRequest(number: {pr_number}) {{
                        commits(first: {PAGE_SIZE}, after: $cursor) {{
                            totalCount
                            pageInfo {{
                                hasNextPage
                                endCursor
                            }}
                            nodes {{
                                commit {{
                                    oid
                                    messageHeadline
                                    messageBody
                                }}
                            }}
                        }}
                    }}
                }}
             }}""",
            "variables": {"cursor": cursor},
        }
        req = requests.post(
            url="https://api.github.com/graphql",
            json=query,
            headers=headers,
            timeout=60,  # 60s
        )
        resp = req.json()
        pr_data = (resp.get("data") or {}).get("repository", {}).get("pullRequest")
        if pr_data is None:
            LOGGER.error("Could not read the pull request from GitHub", response=resp)
            raise typer.Exit(code=STATUS_ERROR)

        commits_data = pr_data["commits"]
        if any(node is None for node in commits_data["nodes"]):
            # GitHub nulls out entries it cannot retrieve. Most often the token lacks
            # the permission to read commit objects ("Resource not accessible by
            # integration"); it can also mean a commit was force-pushed away while the
            # pages were being read. Nothing can be validated without seeing every
            # commit, so fail closed and point at the cause.
            LOGGER.error(
                "GitHub returned a commit that could not be read. If the errors below say "
                '"Resource not accessible by integration", the token used to read the pull '
                "request lacks permission to read commit objects and needs contents: read; "
                "otherwise the pull request's commit list may have changed while it was "
                "being read - retry the task",
                errors=resp.get("errors"),
            )
            raise typer.Exit(code=STATUS_ERROR)

        if commits_data["totalCount"] > MAX_COMMITS:
            LOGGER.error(
                "Pull requests with more than %s commits are not supported, found %s",
                MAX_COMMITS,
                commits_data["totalCount"],
            )
            raise typer.Exit(code=STATUS_ERROR)

        nodes.extend(commits_data["nodes"])
        if not commits_data["pageInfo"]["hasNextPage"] or len(nodes) >= commits_data["totalCount"]:
            break
        cursor = commits_data["pageInfo"]["endCursor"]

    fake_repo = Repo()
    commits = []
    for node in nodes:
        gh_commit = node["commit"]
        message = gh_commit["messageHeadline"]
        if gh_commit["messageBody"]:
            message = "\n".join([message, gh_commit["messageBody"]])
        LOGGER.info("Commit message", message=message)
        commits.append(
            Commit(
                message=message,
                repo=fake_repo,
                # required fields, but faked out - these aren't helpful in user-facing logs
                binsha=bytes.fromhex(gh_commit["oid"]),
            )
        )
    LOGGER.info("Fetched pull request commits", count=len(commits))
    return commits


def check_banned_strings(text: str) -> str | None:
    """Check the pull request text for banned strings, whitespace-insensitively."""
    banned_string = find_banned_string(text)
    if not banned_string:
        return None
    return f"""Pull request title/description contains a banned string (ignoring whitespace).
Please update the pull request title and description to not contain the following banned string:
banned string: "{banned_string}"
{RETRY_INSTRUCTIONS}"""


def main(
    github_org: Annotated[
        str,
        typer.Option(envvar="GITHUB_ORG", help="Name of the github organization (e.g. 10gen)"),
    ] = "",
    github_repo: Annotated[
        str,
        typer.Option(envvar="GITHUB_REPO", help="Name of the repo (e.g. mongo)"),
    ] = "",
    pr_number: Annotated[
        int,
        typer.Option(envvar="PR_NUMBER", help="PR Number to validate"),
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
    """Validate the commits a rebase merge would land."""
    logging.basicConfig(level=logging.INFO, stream=sys.stdout, format="%(message)s")

    if requester not in ("github_merge_queue", "github_pr", "patch"):
        LOGGER.error("Running with an invalid requester", requester=requester)
        raise typer.Exit(code=STATUS_ERROR)

    # Fail closed on missing identifying information: without it the pull
    # request's commits cannot be read, so nothing can be validated.
    if not github_org or not github_repo or not github_token or pr_number < 0:
        LOGGER.error(
            "github org/repo, pull request number and token are all required to read the "
            "pull request's commits from GitHub. This task validates the commits the pull "
            "request already has on GitHub, so it can only run against a pull request - "
            "select it as a pull request check rather than submitting a patch yourself"
        )
        raise typer.Exit(code=STATUS_ERROR)

    commits = get_pr_commits(github_org, github_repo, pr_number, github_token)
    if not commits:
        LOGGER.error("The pull request has no commits; nothing to validate")
        raise typer.Exit(code=STATUS_ERROR)

    changed_files = get_changed_files("../expansions.yml", diff_filter=None)
    for commit in commits:
        if not is_valid_commit(commit, changed_files, requester):
            LOGGER.error('Invalid commit "%s", unable to merge', commit.summary)
            raise typer.Exit(code=STATUS_ERROR)

    ticket_error = check_feature_branch_tickets(commits)
    if ticket_error:
        LOGGER.error(ticket_error)
        raise typer.Exit(code=STATUS_ERROR)

    # The PR text no longer travels in a commit message, so it is validated here
    # and folded into the CVE scan.
    pr_text = fetch_pr_text(github_org, github_repo, pr_number, github_token)
    banned_error = check_banned_strings(pr_text)
    if banned_error:
        LOGGER.error(banned_error)
        raise typer.Exit(code=STATUS_ERROR)

    if not jira_auth_pat:
        LOGGER.warning("JIRA_AUTH_PAT is not set; skipping the linked CVE check")
        return

    jira_keys = referenced_jira_keys_for_cve_check(commits)
    # Scan the PR text under a synthetic commit so keys referenced only in
    # the title/description are still checked.
    fake_repo = Repo()
    jira_keys.update(
        referenced_jira_keys_for_cve_check(
            [Commit(repo=fake_repo, binsha=b"00000000000000000000", message=pr_text)]
        )
    )
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
