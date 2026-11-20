"""Merge-queue quota gate (SERVER-138460).

Blocks a merge when the team owning its ticket is over its Block-on-Red hot
quota, unless the change is exempt or fixes a failure.

`decide()` is pure and table-driven; all I/O lives in `gather()`. That split is
what makes the decision exhaustively testable without mocking Jira.

Usage (local, needs a Jira PAT):
    JIRA_AUTH_PAT=<token> python -m buildscripts.monitor_build_status.merge_queue_quota_check --commit-message "SERVER-12345 ..."
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from enum import Enum

import structlog
import yaml

from buildscripts.client.jiraclient import JiraAuth, JiraClient
from buildscripts.monitor_build_status.code_lockdown_config import CodeLockdownConfig
from buildscripts.monitor_build_status.issue_report import IssueCategory, IssueReport
from buildscripts.monitor_build_status.jira_service import (
    UNASSIGNED_LABEL,
    IssueTuple,
    JiraService,
)
from buildscripts.util.cmdutils import enable_logging

LOGGER = structlog.get_logger(__name__)

TICKET = re.compile(r"\b([A-Z][A-Z0-9]{1,9}-\d+)\b")
# Merge-queue subjects end with the pull request, e.g. "... (#66955)".
PULL_REQUEST = re.compile(r"\(#(\d+)\)")
EXEMPT_PROJECTS = frozenset({"BF", "AF", "SECBUG"})
# Only the link types that mean "this fixes that". Related is a bookmark, and
# Problem/Incident marks the change that caused the failure.
FIX_LINKS = frozenset({("Fix", "outwardIssue"), ("Depends", "inwardIssue")})

CODE_LOCKDOWN_CONFIG = "etc/code_lockdown.yml"
EXCEPTIONS_CONFIG = "etc/block_on_red_exceptions.yml"
JIRA_SERVER = "https://jira.mongodb.org"


class Verdict(Enum):
    BLOCK = "block"
    PASS = "pass"
    UNRESOLVED = "unresolved"  # fail open, but always logged


@dataclass(frozen=True)
class Facts:
    """Everything the decision needs. None means 'could not resolve'."""

    ticket: str | None = None
    team: str | None = None
    assignee: str | None = None
    over_quota: bool | None = None
    exempt: bool = False
    linked_projects: frozenset[str] = frozenset()
    links: tuple[Link, ...] = ()  # every link seen, counted or not
    hot_count: int | None = None  # informational, for the audit line
    hot_limit: int | None = None


@dataclass(frozen=True)
class Finding:
    verdict: Verdict
    rule: str
    detail: str


# Evaluated in order; the first rule whose predicate holds decides.
# Adding a case means adding a row, not touching control flow.
RULES: tuple[tuple[str, object, Verdict, str], ...] = (
    # Placeholder ids like SERVER-XXX never match TICKET, so they land here too.
    ("no-ticket", lambda f: not f.ticket, Verdict.UNRESOLVED, "no ticket id in commit"),
    ("no-team", lambda f: not f.team, Verdict.UNRESOLVED, "no team assigned on the ticket"),
    ("no-quota", lambda f: f.over_quota is None, Verdict.UNRESOLVED, "quota state unavailable"),
    ("under-quota", lambda f: not f.over_quota, Verdict.PASS, "team under hot quota"),
    ("exempt", lambda f: f.exempt, Verdict.PASS, "VP-approved exception"),
    (
        "fix-work",
        lambda f: bool(f.linked_projects & EXEMPT_PROJECTS),
        Verdict.PASS,
        "fixes a BF/AF/security issue",
    ),
)

BLOCKED = Finding(Verdict.BLOCK, "over-quota", "team over hot quota, no exemption")
FAILED = Finding(Verdict.UNRESOLVED, "lookup-failed", "could not evaluate the commit")


def decide(facts: Facts) -> Finding:
    for rule, holds, verdict, detail in RULES:
        if holds(facts):
            return Finding(verdict, rule, detail)
    return BLOCKED


def commit_message_from_git(revision: str | None = None) -> str:
    """The commit subject at `revision` (default HEAD).

    Only the subject is read. It is the PR title, which the commit-message check
    already requires to carry a ticket; the body is free text that cites
    unrelated tickets, so scanning it would block or pass the wrong merges. A
    merge group is a single squashed commit, so the tip subject is the whole
    group.
    """
    command = ["git", "log", "-1", "--pretty=%s"]
    if revision:
        command.append(revision)
    return subprocess.run(command, capture_output=True, text=True, check=True).stdout


def pull_request_from_subject(subject: str) -> int | None:
    """The PR number from the subject, or None if it has none."""
    match = PULL_REQUEST.search(subject)
    return int(match.group(1)) if match else None


def ticket_from_subject(subject: str) -> str | None:
    """The ticket the change is for — the leading key in the subject.

    Only the first is taken. A subject may name another ticket in passing
    ("Update TODO listed in SERVER-130659"), and that one is not the change's,
    so deciding on it would block or pass the wrong merge.
    """
    keys = TICKET.findall(subject)
    return keys[0] if keys else None


def approved_emails(path: str = EXCEPTIONS_CONFIG) -> frozenset[str]:
    """Emails of people with a VP-approved exception, from the checked-in list."""
    with open(path, encoding="utf-8") as handle:
        config = yaml.safe_load(handle) or {}
    entries = config.get("exceptions", []) if isinstance(config, dict) else []
    emails = set()
    for entry in entries:
        # A malformed row must not take the whole gate down.
        if isinstance(entry, dict) and entry.get("email"):
            emails.add(str(entry["email"]).lower())
        else:
            LOGGER.warning("Ignoring malformed exceptions entry", entry=entry)
    return frozenset(emails)


def assignee_email(issue) -> str | None:
    """The ticket's assignee, as a corporate address.

    Jira always holds an @mongodb.com identity here, unlike the git commit
    author — which is frequently a GitHub noreply or personal address.
    """
    assignee = getattr(issue.fields, "assignee", None)
    return (getattr(assignee, "emailAddress", None) or None) if assignee else None


@dataclass(frozen=True)
class Link:
    """One link on the ticket, and whether it counts as fix work."""

    key: str
    link_type: str
    counted: bool


def links_seen(issue) -> tuple[Link, ...]:
    """Every link on the ticket, so the log can say why one did not count."""
    seen = []
    for link in getattr(issue.fields, "issuelinks", None) or []:
        link_type = getattr(link, "type", None)
        if link_type is None:
            continue
        for side in ("inwardIssue", "outwardIssue"):
            other = getattr(link, side, None)
            if other is not None:
                seen.append(
                    Link(
                        key=other.key,
                        link_type=link_type.name,
                        counted=(link_type.name, side) in FIX_LINKS,
                    )
                )
    return tuple(seen)


def linked_projects(issue) -> frozenset[str]:
    """Project keys of the issues this one fixes, by link type."""
    return frozenset(link.key.split("-")[0] for link in links_seen(issue) if link.counted)


def hot_quota(team: str, jira_service: JiraService, config: CodeLockdownConfig) -> tuple[int, int]:
    """(count, limit) for `team`'s hot quota, on the same basis Block-on-Red reports.

    Only the hot issues are fetched — the cold ones are never counted here, and
    fetching them would double the Jira work on every merge.
    """
    notification = config.notifications[0]
    thresholds = config.get_team_thresholds(team, notification.thresholds.team)

    report = IssueReport.empty()
    for scope in notification.scopes:
        report.add_issues(hot=jira_service.fetch_issues(scope.jira_queries.hot), cold=[])

    count = report.get_issue_count(IssueCategory.HOT, thresholds.hot.grace_period_days * 24, [team])
    return count, thresholds.hot.count


def gather(
    ticket: str,
    jira_client: JiraClient,
    config: CodeLockdownConfig,
    exceptions: frozenset[str],
) -> Facts | None:
    """Resolve one ticket into the facts `decide()` needs. Read-only.

    Returns None when the key is not a Jira issue at all — a word-number token
    like UTF-8 matches the ticket pattern but is not a ticket.
    """
    issues = list(jira_client.get_issues(f"key = {ticket}"))
    if not issues:
        return None

    issue = issues[0]
    assignee = assignee_email(issue)
    exempt = bool(assignee) and assignee.lower() in exceptions
    team = IssueTuple.from_jira_issue(issue).assigned_team
    links = links_seen(issue)
    fix_projects = frozenset(link.key.split("-")[0] for link in links if link.counted)
    if team == UNASSIGNED_LABEL:
        return Facts(
            ticket=ticket,
            assignee=assignee,
            exempt=exempt,
            linked_projects=fix_projects,
            links=links,
        )

    count, limit = hot_quota(team, JiraService(jira_client), config)
    return Facts(
        ticket=ticket,
        team=team,
        assignee=assignee,
        over_quota=count > limit,
        exempt=exempt,
        linked_projects=fix_projects,
        links=links,
        hot_count=count,
        hot_limit=limit,
    )


def evaluate(
    commit_message: str,
    jira_client: JiraClient,
    config: CodeLockdownConfig,
    exceptions: frozenset[str],
) -> tuple[Finding, Facts | None]:
    """Decide on the ticket the subject names."""
    ticket = ticket_from_subject(commit_message)
    if ticket is None:
        return Finding(Verdict.UNRESOLVED, "no-ticket", "no ticket id in commit"), None

    facts = gather(ticket, jira_client, config, exceptions)
    if facts is None:
        return Finding(Verdict.UNRESOLVED, "not-an-issue", "not a Jira issue"), None

    return decide(facts), facts


def enforce_requested() -> bool:
    """Whether to block merges, from the project variable.

    A project variable rather than a code change, so turning the gate on is a
    config edit.
    """
    return os.environ.get("MERGE_QUEUE_QUOTA_ENFORCE", "").strip().lower() == "true"


def print_audit(finding: Finding, facts: Facts | None) -> None:
    """The whole decision, so a block can be read without a Jira round trip."""
    print(f"{finding.verdict.value}: {finding.detail}  [{finding.rule}]")
    if facts is None:
        return
    print(f"  ticket    {facts.ticket or '-'}")
    print(f"  team      {facts.team or '-'}  hot={facts.hot_count}/{facts.hot_limit}")
    print(f"  assignee  {facts.assignee or '(unassigned)'}  exempt={facts.exempt}")
    print_links(facts.links)


NOT_COUNTED = "not counted (only Fix and Depends count)"


def print_links(links: tuple[Link, ...]) -> None:
    """Name the type, so a rejected link is not mistaken for a missing one.

    Only the projects the rule can use: a link to another SERVER ticket cannot
    change the verdict, and listing it only buries the one that could.
    """
    relevant = [link for link in links if link.key.split("-")[0] in EXEMPT_PROJECTS]
    if not relevant:
        print("  links     none to a BF, AF or security issue")
        return

    # One line per linked issue: a pair can carry more than one link.
    by_key: dict[str, list[Link]] = {}
    for link in relevant:
        by_key.setdefault(link.key, []).append(link)
    for index, (key, group) in enumerate(by_key.items()):
        lead = "  links   " if index == 0 else "          "
        kinds = ", ".join(dict.fromkeys(link.link_type for link in group))
        note = "counted" if any(link.counted for link in group) else NOT_COUNTED
        print(f"{lead} {key}  {kinds}  {note}")


def write_verdict(
    path: str | None, commit_message: str, finding: Finding, facts: Facts | None
) -> None:
    """Temporary dry-run record. Nothing is written unless a path is given."""
    if not path:
        return
    try:
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(
                {
                    "pr": pull_request_from_subject(commit_message),
                    "team": facts.team if facts else None,
                    "verdict": finding.verdict.value,
                    "rule": finding.rule,
                },
                handle,
            )
    except OSError as exc:
        print(f"  could not write the verdict record: {exc}")


def main(
    commit_message: str | None = None,
    revision: str | None = None,
    dry_run: bool = True,
    verdict_file: str | None = None,
) -> int:
    enable_logging(verbose=False)
    # Only the subject is ever decided on, whether it came from git or was
    # passed in by hand.
    commit_message = (
        commit_message.splitlines()[0] if commit_message else commit_message_from_git(revision)
    )

    try:
        config = CodeLockdownConfig.from_yaml_config(CODE_LOCKDOWN_CONFIG)
        finding, facts = evaluate(
            commit_message, JiraClient(JIRA_SERVER, JiraAuth()), config, approved_emails()
        )
    except Exception as exc:  # noqa: BLE001 - fail open, but never silently
        print(f"unresolved: could not evaluate the commit ({type(exc).__name__}: {exc})")
        print("  failing open: a lookup failure must not block a merge")
        finding, facts = FAILED, None

    try:
        print_audit(finding, facts)
    except Exception as exc:  # noqa: BLE001 - diagnostics must never fail the gate
        print(f"  could not print the audit line: {type(exc).__name__}: {exc}")

    # Written on every path, including the fail-open one: the upload step
    # expects the file, and a missing one would fail the task.
    write_verdict(verdict_file, commit_message, finding, facts)

    if dry_run:
        print("  dry-run: reporting only, not blocking")
        return 0
    return 1 if finding.verdict is Verdict.BLOCK else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--revision", help="revision to read the commit message from (default HEAD)"
    )
    parser.add_argument("--commit-message", help="commit message (default: read from git)")
    parser.add_argument("--enforce", action="store_true", help="exit non-zero to block the merge")
    parser.add_argument(
        "--verdict-file",
        help="write a one-line verdict record here (temporary, for the dry run)",
    )
    args = parser.parse_args()
    sys.exit(
        main(
            args.commit_message,
            args.revision,
            dry_run=not (args.enforce or enforce_requested()),
            verdict_file=args.verdict_file,
        )
    )
