#!/usr/bin/env python3
# Copyright (C) 2019-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""Validate that the commit message is ok."""

import pathlib
import re
import subprocess

import requests
import structlog
import typer
from git import Commit, Repo
from typing_extensions import Annotated

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

VALID_SUMMARY = re.compile(r'(Revert ")?(SERVER-[0-9]+|Import wiredtiger)')


def is_valid_commit(commit: Commit) -> bool:
    # Valid values look like:
    # 1. SERVER-\d+
    # 2. Revert "SERVER-\d+
    # 3. Import wiredtiger
    # 4. Revert "Import wiredtiger
    if not VALID_SUMMARY.match(commit.summary):
        LOGGER.error(
            f"""PR summary is not valid; it must match the regular expression: {VALID_SUMMARY}
Current summary: {commit.summary}
Please update the PR title and description to match the expected format.
If you are seeing this on a PR, after changing the title/description, you will need to rerun this check before being able to submit your PR.
The decision to add this check was made in SERVER-101443, please feel free to leave comments/feedback on that ticket.""",
        )
        return False

    # Remove all whitespace from comparisons. GitHub line-wraps commit messages, which adds
    # newline characters that otherwise would not match verbatim such banned strings.
    stripped_message = "".join(commit.message.split())
    for banned_string in BANNED_STRINGS:
        if "".join(banned_string.split()) in stripped_message:
            LOGGER.error(
                """PR title/description contains a banned string (ignoring whitespace).
Please update the PR title and description to not contain the following banned string.
If you are seeing this on a PR, after changing the title/description, you will need to rerun this check before being able to submit your PR.
The decision to add this check was made in SERVER-101443, please feel free to leave comments/feedback on that ticket.""",
                banned_string=banned_string,
                commit_message=commit.message,
            )
            return False

    return True


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
):
    """
    Validate the commit message.

    It validates the latest message when no arguments are provided.
    """

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

    for commit in commits:
        if not is_valid_commit(commit):
            LOGGER.error("Invalid commit, unable to merge")
            raise typer.Exit(code=STATUS_ERROR)

    return


if __name__ == "__main__":
    typer.run(main)
