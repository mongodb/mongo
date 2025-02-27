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
            "Commit did not contain a valid summary",
            commit_hexsha=commit.hexsha,
            commit_summary=commit.summary,
        )
        return False

    # Remove all whitespace from comparisons. GitHub line-wraps commit messages, which adds
    # newline characters that otherwise would not match verbatim such banned strings.
    stripped_message = "".join(commit.message.split())
    for banned_string in BANNED_STRINGS:
        if "".join(banned_string.split()) in stripped_message:
            LOGGER.error(
                "Commit contains banned string (ignoring whitespace)",
                banned_string=banned_string,
                commit_hexsha=commit.hexsha,
                commit_message=commit.message,
            )
            return False

    return True


def main(
    branch_name: Annotated[
        str,
        typer.Option(envvar="BRANCH_NAME", help="Name of the branch to compare against HEAD"),
    ],
    is_commit_queue: Annotated[
        str,
        typer.Option(
            envvar="IS_COMMIT_QUEUE",
            help="If this is being run in the commit/merge queue. Set to anything to be considered part of the commit/merge queue.",
        ),
    ] = "",
):
    """
    Validate the commit message.

    It validates the latest message when no arguments are provided.
    """

    if not is_commit_queue:
        LOGGER.info("Exiting early since this is not running in the commit/merge queue")
        raise typer.Exit(code=STATUS_OK)

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

    for commit_hash in commit_hashs:
        commit = repo.commit(commit_hash)
        if not is_valid_commit(commit):
            LOGGER.error("Found an invalid commit", commit=commit)
            raise typer.Exit(code=STATUS_ERROR)

    return


if __name__ == "__main__":
    typer.run(main)
