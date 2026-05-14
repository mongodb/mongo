#!/usr/bin/env python3
# Copyright (C) 2026-present MongoDB, Inc.
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
"""Validate that an Evergreen patch build link is included on the PR."""

import re
import sys

import requests
import structlog
import typer
from typing_extensions import Annotated

LOGGER = structlog.get_logger(__name__)

STATUS_OK = 0
STATUS_ERROR = 1

EVERGREEN_PATCH_URL_RE = re.compile(
    r"https?://(?:spruce|evergreen)\.mongodb\.com/(?:version|patch)/[A-Za-z0-9_-]+",
    re.IGNORECASE,
)

FAILURE_MESSAGE = """
No Evergreen patch build link found on this PR.

A required patch build is required before merging (https://wiki.corp.mongodb.com/spaces/KERNEL/pages/126668501/Required+Patch+Builds+Policy). To fix this:

1. Run an Evergreen patch for this PR:
       evergreen patch -p mongodb-mongo-master -a required
2. Copy the patch link (looks like https://spruce.mongodb.com/version/<id>).
3. Post the link as a comment on this PR (do not put it in the description —
   Spruce/Evergreen URLs in the description are banned by validate_commit_message).
4. Restart this task in Spruce (click Restart on the failing task).
"""


def _gh_get(url: str, github_token: str) -> requests.Response:
    headers = {
        "Authorization": f"token {github_token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    resp = requests.get(url, headers=headers, timeout=60)
    resp.raise_for_status()
    return resp


def get_pr_comments(
    github_org: str, github_repo: str, pr_number: int, github_token: str
) -> list[dict]:
    """Return all issue comments on the PR, following pagination."""
    comments: list[dict] = []
    url = (
        f"https://api.github.com/repos/{github_org}/{github_repo}"
        f"/issues/{pr_number}/comments?per_page=100"
    )
    while url:
        resp = _gh_get(url, github_token)
        comments.extend(resp.json())
        url = resp.links.get("next", {}).get("url")
    return comments


def has_patch_link(text: str) -> bool:
    if not text:
        return False
    return bool(EVERGREEN_PATCH_URL_RE.search(text))


def find_patch_link(comments: list[dict]) -> bool:
    """Look for an Evergreen patch link in any PR comment.

    Intentionally does NOT check the PR description: Spruce/Evergreen URLs are banned
    there by validate_commit_message (they end up in the squashed commit message body).
    """
    return any(has_patch_link(comment.get("body") or "") for comment in comments)


def main(
    github_org: Annotated[
        str, typer.Option(envvar="GITHUB_ORG", help="GitHub organization (e.g. 10gen)")
    ] = "",
    github_repo: Annotated[
        str, typer.Option(envvar="GITHUB_REPO", help="GitHub repo name (e.g. mongo)")
    ] = "",
    pr_number: Annotated[int, typer.Option(envvar="PR_NUMBER", help="PR number")] = -1,
    github_token: Annotated[
        str,
        typer.Option(
            envvar="GITHUB_TOKEN",
            help="GitHub token with pull_requests: read permission",
        ),
    ] = "",
    requester: Annotated[str, typer.Option(envvar="REQUESTER", help="Evergreen requester")] = "",
):
    """Fail if the PR is missing an Evergreen patch build link."""
    if requester != "github_pr":
        LOGGER.info("Skipping: only runs for github_pr", requester=requester)
        return

    if not (github_org and github_repo and github_token and pr_number > 0):
        LOGGER.error(
            "Missing required inputs",
            github_org=bool(github_org),
            github_repo=bool(github_repo),
            github_token=bool(github_token),
            pr_number=pr_number,
        )
        raise typer.Exit(code=STATUS_ERROR)

    comments = get_pr_comments(github_org, github_repo, pr_number, github_token)

    if find_patch_link(comments):
        LOGGER.info("Found Evergreen patch link on PR", pr_number=pr_number)
        return

    LOGGER.error(FAILURE_MESSAGE, pr_number=pr_number)
    raise typer.Exit(code=STATUS_ERROR)


app = typer.Typer(pretty_exceptions_show_locals=False)
app.command()(main)

if __name__ == "__main__":
    sys.exit(app())
