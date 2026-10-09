#!/usr/bin/env python3
"""
Walk all files in a directory, looking for TODOs that reference a resolved JIRA
ticket, and file a follow-up ticket for each one.

Follow-up tickets are assigned to the JIRA team that owns the file containing
the TODO, as determined by `.github/CODEOWNERS` and the GitHub team to JIRA team
mapping in `buildscripts/util/co_jira_map.yml`.
"""

import argparse
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import structlog
import yaml
from codeowners import CodeOwners
from jira import JIRAError
from structlog.stdlib import LoggerFactory

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.client.jiraclient import ASSIGNED_TEAMS_FIELD, JiraAuth, JiraClient

structlog.configure(logger_factory=LoggerFactory())
LOG = structlog.getLogger(__name__)
JIRA_SERVER = "https://jira.mongodb.org"

REPO_ROOT = Path(__file__).resolve().parents[1]
CODEOWNERS_FILE = REPO_ROOT / ".github" / "CODEOWNERS"
CO_JIRA_MAP_FILE = REPO_ROOT / "buildscripts" / "util" / "co_jira_map.yml"

# Team used when CODEOWNERS does not resolve to a known JIRA team, or when JIRA
# rejects the team we resolved.
FALLBACK_TEAM = "Server Triage"

ISSUE_KEY_RE = re.compile(
    "(BUILD|SERVER|WT|SPM|TOOLS|TIG|PERF|BF|REP|BACKPORT|WRITING|STAR|SLS)-[0-9]+",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class TodoOccurrence:
    """A single line of code containing a TODO that references an issue key."""

    issue_key: str
    file_path: str  # Repo relative, posix separators.
    line_number: int
    line: str


class TeamResolver:
    """Resolve the JIRA team(s) that own a file, based on CODEOWNERS."""

    def __init__(self, codeowners_contents: str, co_jira_map: dict[str, list[str] | None]):
        self._codeowners = CodeOwners(codeowners_contents)
        # CODEOWNERS spells GitHub teams `@10gen/foo`; the map keys are `10gen/foo`.
        # Entries may map to a null list, meaning "deliberately not a JIRA team".
        self._co_jira_map = {
            f"@{codeowner}": [team for team in (teams or []) if team]
            for codeowner, teams in co_jira_map.items()
        }

    @classmethod
    def from_repo(cls) -> "TeamResolver":
        """Build a resolver from the CODEOWNERS and mapping files in this repo."""
        with open(CODEOWNERS_FILE, "r", encoding="utf8") as codeowners_file:
            codeowners_contents = codeowners_file.read()
        with open(CO_JIRA_MAP_FILE, "r", encoding="utf8") as map_file:
            co_jira_map = yaml.safe_load(map_file)
        return cls(codeowners_contents, co_jira_map)

    def _jira_teams_for_codeowner(self, codeowner: str) -> list[str]:
        """
        Map a single GitHub team to its JIRA team(s).

        Ownership is often expressed with a sub team that has no JIRA team of its
        own (`@10gen/query-execution-sbe`, `@10gen/server-replication-reviewers`),
        so fall back to the longest mapped ancestor of the team name.
        """
        name = codeowner
        while name:
            if name in self._co_jira_map:
                return self._co_jira_map[name]
            name, _, _ = name.rpartition("-")
        return []

    def jira_teams(self, file_path: str) -> list[str]:
        """
        Return the JIRA teams owning the given repo relative file path.

        `matching_lines` yields the CODEOWNERS rules matching the file from most
        to least specific. Ownership is frequently narrowed to a sub team with no
        JIRA team of its own, so keep widening until a rule names a team we can
        map, rather than giving up on the most specific rule.
        """
        for owners, _, _, _ in self._codeowners.matching_lines(file_path):
            teams = []
            for owner_type, codeowner in owners:
                if owner_type != "TEAM":
                    continue
                for team in self._jira_teams_for_codeowner(codeowner):
                    if team not in teams:
                        teams.append(team)
            if teams:
                return teams
        return []


def get_issue_key_from_line(line: str) -> str | None:
    """
    Given a string of text, find and return any issue keys from relevant projects.

    Keys are matched case insensitively but returned upper case, so spellings of the same key
    group together and the project checks downstream (`"REP" in key`) behave.
    """
    match = ISSUE_KEY_RE.search(line)
    return match.group(0).upper() if match else None


def find_todos(search_file, file_path: str) -> list[TodoOccurrence]:
    """Iterate through a file, returning every TODO that references an issue key."""
    occurrences = []
    for i, line in enumerate(search_file):
        if "todo" not in line.lower():
            continue
        issue_key = get_issue_key_from_line(line)
        if issue_key:
            occurrences.append(
                TodoOccurrence(
                    issue_key=issue_key,
                    file_path=file_path,
                    line_number=i + 1,
                    line=line.strip(),
                )
            )
    return occurrences


def collect_todos(path: str) -> dict[str, list[TodoOccurrence]]:
    """Walk `path`, grouping every TODO that references an issue key by issue key."""
    todos_by_key: dict[str, list[TodoOccurrence]] = defaultdict(list)
    for root, dirs, files in os.walk(path):
        # ignore .git/
        if ".git" in dirs:
            dirs.remove(".git")
        for file_name in files:
            full_path = os.path.join(root, file_name)
            try:
                with open(full_path, "r") as search_file:
                    occurrences = find_todos(search_file, repo_relative_path(full_path))
            except (UnicodeDecodeError, OSError):
                continue
            for occurrence in occurrences:
                todos_by_key[occurrence.issue_key].append(occurrence)
    return todos_by_key


def repo_relative_path(file_path: str) -> str:
    """Convert a path to a repo relative posix path, for CODEOWNERS matching."""
    return Path(os.path.relpath(Path(file_path).resolve(), REPO_ROOT)).as_posix()


def resolve_teams(resolver: TeamResolver, occurrences: list[TodoOccurrence]) -> list[str]:
    """Return the JIRA teams owning any of the files the TODOs live in."""
    teams = []
    for occurrence in occurrences:
        for team in resolver.jira_teams(occurrence.file_path):
            if team not in teams:
                teams.append(team)

    if not teams:
        LOG.warning(
            "No JIRA team owns the files containing this TODO; falling back",
            fallback_team=FALLBACK_TEAM,
            files=sorted({occurrence.file_path for occurrence in occurrences}),
        )
        return [FALLBACK_TEAM]
    return teams


def todo_ticket_exists(jira, resolved_issue) -> bool:
    """Check if we have already created a ticket for this resolved issue."""
    jql = (
        "labels = autogen-todo AND resolution is empty AND issueFunction in"
        f" linkedIssuesOf('key={resolved_issue.key}')"
    )
    results = jira.search_issues(jql, maxResults=1000)
    if not results or results.total == 0:
        return False
    return True


def create_todo_ticket(jira, resolved_issue, teams: list[str], occurrences: list[TodoOccurrence]):
    """Create a ticket for the outstanding TODO work and link it to the resolved ticket."""
    key = resolved_issue.key

    issue_dict = {
        "project": {"key": "REP" if "REP" in key else "SERVER"},
        "issuetype": {"name": "Task"},
        "summary": "Complete TODO listed in " + key,
        "description": construct_description(key, occurrences),
        "labels": ["autogen-todo"],
        ASSIGNED_TEAMS_FIELD: [{"value": team} for team in teams],
    }

    LOG.info("Creating TODO ticket", key=key, teams=teams)
    try:
        new_issue = jira.create_issue(fields=issue_dict)
    except JIRAError as err:
        # The team that owns the file may not be a valid option for the target
        # project, in which case the ticket goes to triage for rerouting.
        if teams == [FALLBACK_TEAM] or not _is_assigned_teams_error(err):
            raise
        LOG.warning(
            "JIRA rejected the owning team; falling back",
            teams=teams,
            fallback_team=FALLBACK_TEAM,
            key=key,
        )
        issue_dict[ASSIGNED_TEAMS_FIELD] = [{"value": FALLBACK_TEAM}]
        new_issue = jira.create_issue(fields=issue_dict)

    jira.create_issue_link(
        type="Related", inwardIssue=resolved_issue.key, outwardIssue=new_issue.key
    )


def _is_assigned_teams_error(err: JIRAError) -> bool:
    """Determine whether a JIRAError was caused by an invalid assigned team value."""
    return ASSIGNED_TEAMS_FIELD in _get_jira_field_errors(err) or ASSIGNED_TEAMS_FIELD in str(err)


def _get_jira_field_errors(err: JIRAError) -> dict:
    """
    Best-effort extraction of field-level errors from a JIRAError.

    JIRA commonly returns:
        {"errorMessages": [], "errors": {"customfield_12751": "Option value 'X' is not valid"}}
    """
    response = getattr(err, "response", None)
    if response is None:
        return {}

    json_fn = getattr(response, "json", None)
    if json_fn is None:
        return {}

    try:
        payload = json_fn()
    except Exception:
        return {}

    if not isinstance(payload, dict):
        return {}

    errors = payload.get("errors")
    return errors if isinstance(errors, dict) else {}


def construct_description(key: str, occurrences: list[TodoOccurrence]) -> str:
    repo = "10gen/mongosync" if "REP" in key else "10gen/mongo"
    locations = "\n".join(
        f"* {occurrence.file_path}:{occurrence.line_number}"
        for occurrence in sorted(
            occurrences, key=lambda occurrence: (occurrence.file_path, occurrence.line_number)
        )
    )
    return (
        "There is a TODO in code owned by your team which references a resolved"
        f" ticket.\n\nThe TODO appears in:\n{locations}\n\nYou can also search for"
        f" all references to the resolved ticket"
        f" here:\nhttps://github.com/{repo}/search?q={key}&type=Code\n\nThe next"
        " steps for this ticket are to either remove the outdated TODO or follow the"
        " steps in the TODO if it is correct. If the latter, please update the summary"
        " and description of this ticket to represent the work you're actually doing."
    )


def process_todos(
    jira, resolver: TeamResolver, todos_by_key: dict[str, list[TodoOccurrence]]
) -> list[str]:
    """
    Create a ticket for each resolved issue that still has TODOs referencing it.

    Returns the issue keys for which creating a ticket failed.
    """
    failed: list[str] = []
    for issue_key, occurrences in sorted(todos_by_key.items()):
        LOG.info("\n")
        LOG.info("=== Found Issue Key ===")
        LOG.info(f"{JIRA_SERVER}/browse/{issue_key}")
        for occurrence in occurrences:
            LOG.info(f"{occurrence.file_path}:{occurrence.line_number}: {occurrence.line}")

        # It is possible the referenced ticket in the code was deleted, so we need to
        # handle the possibility that searching for it will return nothing.
        try:
            issue = jira.issue(issue_key)
        except JIRAError:
            LOG.warn(f"{issue_key} not found in Jira.  Skipping.")
            continue

        status = str(issue.fields.status)
        if status not in ["Resolved", "Closed"]:
            LOG.info(f"{issue_key} is not resolved.  Skipping.")
            continue

        if todo_ticket_exists(jira, issue):
            LOG.info(f"Autogenerated ticket linked to {issue_key} already exists.")
            continue

        try:
            create_todo_ticket(jira, issue, resolve_teams(resolver, occurrences), occurrences)
        except JIRAError as err:
            # Don't let one rejected ticket stop the rest from being filed. Log the response
            # body, which is where JIRA explains which field it objected to.
            LOG.error(
                "Failed to create TODO ticket",
                key=issue_key,
                status_code=err.status_code,
                response=err.text,
            )
            failed.append(issue_key)
    return failed


def main():
    """Execute main function."""
    argparser = argparse.ArgumentParser(description=__doc__)
    argparser.add_argument("--env", "-e", help="Jira environment {stg, prod}", required=False)
    argparser.add_argument("--path", "-p", help="File path to walk", required=True)
    args = vars(argparser.parse_args())

    jira = JiraClient(JIRA_SERVER, JiraAuth(), dry_run=False)
    resolver = TeamResolver.from_repo()

    failed = process_todos(jira._jira, resolver, collect_todos(args["path"]))
    if failed:
        LOG.error("Failed to create tickets for some TODOs", keys=failed)
        sys.exit(1)


if __name__ == "__main__":
    main()
