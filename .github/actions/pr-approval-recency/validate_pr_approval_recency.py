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
"""Validate that a PR's approvals still cover its most recent change (diff-aware)."""

import datetime
import logging
import os
import sys
from collections.abc import Mapping
from typing import TYPE_CHECKING

from github import Auth, Github
from github.GithubRetry import GithubRetry

if TYPE_CHECKING:
    from github.File import File
    from github.PullRequest import PullRequest
    from github.Repository import Repository

logging.basicConfig(level=logging.INFO, format="%(message)s")
LOGGER = logging.getLogger("validate_pr_approval_recency")

STATUS_OK = 0
STATUS_ERROR = 1

# Result categories emitted to Honeycomb.
RESULT_PASS = "pass"
RESULT_FAIL = "fail"
RESULT_ERROR = "error"

# Reason codes (one per exit path). Each maps deterministically to a result + exit code.
REASON_PENDING_REVIEW = "pending_review"
REASON_SHA_MATCH = "sha_match"
REASON_CONTENT_UNCHANGED = "content_unchanged"
REASON_CONTENT_CHANGED = "content_changed"
REASON_NO_APPROVALS = "no_approvals"
REASON_FILE_LIMIT = "file_limit_hit"
REASON_API_ERROR = "api_error"

REASON_TO_RESULT = {
    REASON_PENDING_REVIEW: RESULT_PASS,
    REASON_SHA_MATCH: RESULT_PASS,
    REASON_CONTENT_UNCHANGED: RESULT_PASS,
    REASON_CONTENT_CHANGED: RESULT_FAIL,
    REASON_NO_APPROVALS: RESULT_FAIL,
    REASON_FILE_LIMIT: RESULT_FAIL,
    REASON_API_ERROR: RESULT_ERROR,
}


def _status_for_reason(reason: str) -> int:
    """Exit code for a reason: 0 for pass reasons, 1 for fail/error reasons."""
    return STATUS_OK if REASON_TO_RESULT[reason] == RESULT_PASS else STATUS_ERROR


# GitHub's API is intermittently flaky; PyGithub's GithubRetry retries with backoff.
# With these values the total wait budget is roughly 5 minutes:
#   delays (s): 4 + 8 + 16 + 32 + 60 + 60 + 60 + 60 ≈ 300 s
MAX_RETRIES = 8
RETRY_BACKOFF_FACTOR = 2.0
RETRY_BACKOFF_MAX = 60  # cap individual sleeps so later retries don't stall for minutes

# GitHub's Compare API caps the changed-files list at 300; at/above this we can't fully
# verify the diff, so we fail closed rather than compare a truncated file set.
COMPARE_FILE_LIMIT = 300

Content = dict[str, list[str] | str]
LogfmtValue = str | bool | int
Metrics = dict[str, LogfmtValue]


def _content_lines(patch: str | None) -> list[str] | None:
    """Return the added/removed code lines of a per-file unified diff patch.

    Keeps lines beginning with '+' or '-' (the diff add/remove markers) and drops '@@'
    hunk headers and context lines. GitHub's per-file ``patch`` never contains the
    ``+++``/``---`` file headers, so those are NOT excluded -- doing so would wrongly drop
    real content lines like ``++i;`` (added) or ``--count;`` (removed). Returns None when
    there is no patch (binary/too-large), so the caller can key on the blob sha instead.
    """
    if patch is None:
        return None
    return [line for line in patch.splitlines() if line[:1] in ("+", "-")]


def _logfmt_value(value: LogfmtValue) -> str:
    """Render one value for logfmt output.

    Bools become ``true``/``false``. Anything containing whitespace, ``=`` or a quote
    (and the empty string) is double-quoted, with embedded backslashes/quotes escaped and
    newlines flattened to spaces, so free-text fields like ``pr_title`` parse as one field.
    """
    if isinstance(value, bool):
        return "true" if value else "false"
    text = str(value)
    needs_quoting = text == "" or any(c.isspace() or c in '="' for c in text)
    if not needs_quoting:
        return text
    escaped = text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ").replace("\r", " ")
    return f'"{escaped}"'


def _logfmt_line(fields: Mapping[str, LogfmtValue]) -> str:
    """Render an ordered mapping as a single logfmt line (insertion order preserved)."""
    return " ".join(f"{key}={_logfmt_value(value)}" for key, value in fields.items())


def write_logfmt(path: str, fields: Mapping[str, LogfmtValue]) -> None:
    """Append one logfmt line to ``path`` (the gha-buildevents file)."""
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(_logfmt_line(fields) + "\n")


def should_block_merge(
    review_decision: str | None,
    approved_content: Content | None,
    current_content: Content | None,
) -> bool:
    """Return True if the task should FAIL, per the diff-aware rule.

    Fails iff the PR is fully approved (reviewDecision == "APPROVED") AND the introduced
    code at the current head is not provably identical to the introduced code that was
    most recently approved. Not-fully-approved PRs are not gated here (branch protection
    handles that). Missing content (a truncated/unfetchable diff) fails closed.
    """
    if review_decision != "APPROVED":
        return False
    if approved_content is None or current_content is None:
        return True  # could not verify -> fail closed
    return approved_content != current_content


def _review_decision(github_client: Github, owner: str, name: str, pr_number: int) -> str | None:
    """Fetch the PR's reviewDecision via GraphQL (REST does not expose it).

    Returns "APPROVED" / "REVIEW_REQUIRED" / "CHANGES_REQUESTED" / None.
    """
    query = (
        '{ repository(owner: "%s", name: "%s") '
        "{ pullRequest(number: %d) { reviewDecision } } }" % (owner, name, pr_number)
    )
    _, response = github_client.requester.graphql_query(query, {})
    if response.get("errors"):
        raise RuntimeError(f"GitHub GraphQL error: {response['errors']}")
    repository = (response.get("data") or {}).get("repository")
    pull_request = repository.get("pullRequest") if repository else None
    if pull_request is None:
        raise RuntimeError("GraphQL returned no pullRequest (repo/PR not accessible).")
    return pull_request.get("reviewDecision")


def _introduced_content(files: list["File"]) -> Content:
    """Build {filename -> content} from a PyGithub Comparison's files.

    Text files map to their added/removed lines; binary/too-large files (no patch) map to
    ``blob:<sha>`` so an unchanged binary compares equal and a changed one differs.
    """
    content: Content = {}
    for changed in files:
        lines = _content_lines(changed.patch)
        content[changed.filename] = lines if lines is not None else f"blob:{changed.sha}"
    return content


def _post_approval_diff_stats(
    approved_content: Content,
    current_content: Content,
) -> tuple[int, int]:
    """Size of the change introduced since approval: (changed_files, changed_loc).

    A file counts as changed if its introduced content differs between the two snapshots
    (added, removed, or modified). ``changed_loc`` sums, per changed file, the symmetric
    difference of the ``+``/``-`` line lists -- the introduced lines not common to both.
    Binary/too-large files (``blob:<sha>`` strings) count toward files but contribute 0
    LOC, since no line-level data is available.
    """
    changed_files = 0
    changed_loc = 0
    for filename in set(approved_content) | set(current_content):
        approved = approved_content.get(filename)
        current = current_content.get(filename)
        if approved == current:
            continue
        changed_files += 1
        approved_lines = approved if isinstance(approved, list) else []
        current_lines = current if isinstance(current, list) else []
        changed_loc += len(set(approved_lines) ^ set(current_lines))
    return changed_files, changed_loc


def _latest_approval_commit(pull: "PullRequest") -> str | None:
    """The commit SHA of the most recent APPROVED review, or None if there are none.

    Ties on ``submitted_at`` (e.g. two approvals in the same second) are broken by review
    id, which increases monotonically, so the winner is deterministic across runs.
    """
    approvals = [r for r in pull.get_reviews() if r.state == "APPROVED" and r.submitted_at]
    if not approvals:
        return None
    return max(approvals, key=lambda r: (r.submitted_at, r.id)).commit_id


def _check_approved_pr(repo: "Repository", pull: "PullRequest", metrics: Metrics) -> str:
    """Gate logic for a fully-approved PR; takes PyGithub objects directly.

    Populates ``metrics`` in place and returns a reason code. Raises on API failure.
    """
    base_ref = pull.base.ref
    head_sha = pull.head.sha
    approval_sha = _latest_approval_commit(pull)
    metrics.update(
        {
            "pr_title": pull.title,
            "pr_author": pull.user.login,
            "pr_state": pull.state,
            "pr_created_at": pull.created_at.isoformat() if pull.created_at else "",
            "pr_updated_at": pull.updated_at.isoformat() if pull.updated_at else "",
            "pr_base_branch": base_ref,
            "pr_head_branch": pull.head.ref,
            "head_sha": head_sha,
            "approval_sha": approval_sha if approval_sha else "",
        }
    )
    LOGGER.info("base=%s head=%s most_recent_approval_commit=%s", base_ref, head_sha, approval_sha)

    if approval_sha is None:
        LOGGER.error("Approved but found no APPROVED review commit; failing closed.")
        return REASON_NO_APPROVALS
    if approval_sha == head_sha:
        LOGGER.info("Most recent approval is on the current head; code is unchanged.")
        return REASON_SHA_MATCH

    approved_files = _compare_files_within_limit(repo, base_ref, approval_sha)
    if approved_files is None:
        return REASON_FILE_LIMIT
    current_files = _compare_files_within_limit(repo, base_ref, head_sha)
    if current_files is None:
        return REASON_FILE_LIMIT

    approved_content = _introduced_content(approved_files)
    current_content = _introduced_content(current_files)

    if should_block_merge("APPROVED", approved_content, current_content):
        changed_files, changed_loc = _post_approval_diff_stats(approved_content, current_content)
        metrics["was_restack"] = False
        metrics["post_approval_changed_files"] = changed_files
        metrics["post_approval_changed_loc"] = changed_loc
        LOGGER.error(
            "This PR is fully approved, but the introduced code changed since the most "
            "recent approval (commit %s). The current code has not been reviewed; please "
            "get a fresh approval before merging. (A no-op rebase would NOT trigger this.)",
            approval_sha,
        )
        return REASON_CONTENT_CHANGED

    metrics["was_restack"] = True
    LOGGER.info("Diff-aware approval check passed: current code matches the approved code.")
    return REASON_CONTENT_UNCHANGED


def _run_gate(
    github_client: Github, owner: str, name: str, pr_number: int, metrics: Metrics
) -> str:
    """Fetch reviewDecision then delegate to ``_check_approved_pr``.

    Raises on GitHub API failure -- the caller maps that to REASON_API_ERROR.
    """
    review_decision = _review_decision(github_client, owner, name, pr_number)
    metrics["review_decision"] = review_decision if review_decision is not None else ""
    LOGGER.info("reviewDecision=%s", review_decision)
    if review_decision != "APPROVED":
        LOGGER.info("PR is not fully approved; nothing to gate.")
        return REASON_PENDING_REVIEW

    repo = github_client.get_repo(f"{owner}/{name}")
    pull = repo.get_pull(pr_number)
    return _check_approved_pr(repo, pull, metrics)


def _compare_files_within_limit(repo: "Repository", base_ref: str, sha: str) -> list["File"] | None:
    """The changed files of ``base_ref...sha``, or None if the diff hits the Compare cap."""
    files = repo.compare(base_ref, sha).files or []
    if len(files) >= COMPARE_FILE_LIMIT:
        LOGGER.error(
            "Unable to verify this PR's diff is unchanged since approval: the change touches "
            "%d or more files, exceeding what the GitHub Compare API will return. To unblock, "
            "request a fresh approval on the current commit -- re-approving at the latest "
            "commit skips the diff comparison entirely.",
            COMPARE_FILE_LIMIT,
        )
        return None
    return files


def main() -> int:
    metrics: Metrics = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }
    buildevent_file = os.environ.get("BUILDEVENT_FILE", "")

    def emit(reason: str) -> int:
        metrics["reason"] = reason
        metrics["result"] = REASON_TO_RESULT[reason]
        if buildevent_file:
            try:
                write_logfmt(buildevent_file, metrics)
            except OSError as exc:
                LOGGER.error("Failed to write metrics to %s: %s", buildevent_file, exc)
        return _status_for_reason(reason)

    github_org = os.environ.get("GITHUB_ORG", "")
    github_repo = os.environ.get("GITHUB_REPO", "")
    github_token = os.environ.get("GITHUB_TOKEN", "")
    pr_number_raw = os.environ.get("PR_NUMBER", "")
    if not (github_org and github_repo and github_token and pr_number_raw):
        LOGGER.error("Missing one of GITHUB_ORG/GITHUB_REPO/GITHUB_TOKEN/PR_NUMBER.")
        return emit(REASON_API_ERROR)
    try:
        pr_number = int(pr_number_raw)
    except ValueError:
        LOGGER.error("PR_NUMBER is not a valid integer: %r", pr_number_raw)
        return emit(REASON_API_ERROR)
    metrics.update({"org": github_org, "repo": github_repo, "pr_number": pr_number})

    github_client = Github(
        auth=Auth.Token(github_token),
        retry=GithubRetry(
            total=MAX_RETRIES,
            backoff_factor=RETRY_BACKOFF_FACTOR,
            backoff_max=RETRY_BACKOFF_MAX,
        ),
    )
    try:
        reason = _run_gate(github_client, github_org, github_repo, pr_number, metrics)
    except Exception as exc:
        LOGGER.error("GitHub access error; failing closed: %s", exc)
        reason = REASON_API_ERROR
    finally:
        github_client.close()
    return emit(reason)


if __name__ == "__main__":
    sys.exit(main())
