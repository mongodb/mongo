from __future__ import annotations

import re
from datetime import datetime
from enum import Enum
from typing import Any, List, NamedTuple, Optional

import dateutil.parser
from jira import Issue

from buildscripts.client.jiraclient import JiraClient

UNASSIGNED_LABEL = "~ Unassigned"
CORRECTNESS_EVG_PROJECT_REGEX = re.compile(r"mongodb-mongo.*")
PERFORMANCE_EVG_PROJECT_REGEX = re.compile(r"sys-perf.*")


class JiraCustomFieldIds(str, Enum):
    ASSIGNED_TEAMS = "customfield_12751"
    EVERGREEN_PROJECT = "customfield_14278"
    TEMPERATURE = "customfield_24859"


class JiraCustomFieldNames(str, Enum):
    EVERGREEN_PROJECT = "Evergreen Project"


class TestType(str, Enum):
    CORRECTNESS = "correctness"
    PERFORMANCE = "performance"
    UNKNOWN = "unknown"

    @classmethod
    def from_evg_project_name(cls, name: str) -> TestType:
        if CORRECTNESS_EVG_PROJECT_REGEX.match(name):
            return cls.CORRECTNESS
        if PERFORMANCE_EVG_PROJECT_REGEX.match(name):
            return cls.PERFORMANCE
        return cls.UNKNOWN


class BfTemperature(str, Enum):
    HOT = "hot"
    COLD = "cold"
    NONE = "none"

    @classmethod
    def from_str(cls, temperature: Optional[str]) -> BfTemperature:
        match temperature:
            case "hot":
                return cls.HOT
            case "cold":
                return cls.COLD
            case _:
                return cls.NONE


class BfIssue(NamedTuple):
    key: str
    assigned_team: str
    evergreen_projects: List[str]
    temperature: BfTemperature
    created_time: datetime

    @classmethod
    def from_jira_issue(cls, issue: Issue) -> BfIssue:
        """
        Build BfIssue object from Jira Issue object.

        :param issue: Jira issue object.
        :return: BF issue object.
        """
        assigned_team = UNASSIGNED_LABEL
        assigned_teams_field = getattr(issue.fields, JiraCustomFieldIds.ASSIGNED_TEAMS)
        if isinstance(assigned_teams_field, list) and len(assigned_teams_field) > 0:
            assigned_teams_values = [getattr(item, "value") for item in assigned_teams_field]
            assigned_team = assigned_teams_values[0]

        evergreen_projects = getattr(issue.fields, JiraCustomFieldIds.EVERGREEN_PROJECT)
        if not isinstance(evergreen_projects, list):
            evergreen_projects = []

        temperature_value = getattr(issue.fields, JiraCustomFieldIds.TEMPERATURE)
        if isinstance(temperature_value, str):
            temperature = BfTemperature.from_str(temperature_value)
        else:
            temperature = BfTemperature.NONE

        return cls(
            key=issue.key,
            assigned_team=assigned_team,
            evergreen_projects=evergreen_projects,
            temperature=temperature,
            created_time=dateutil.parser.parse(issue.fields.created),
        )

    def __eq__(self, other: Any) -> bool:
        return isinstance(other, self.__class__) and self.key == other.key

    def __hash__(self) -> int:
        return hash(self.key)


class JiraService:
    def __init__(self, jira_client: JiraClient) -> None:
        self.jira_client = jira_client

    def fetch_bfs(self, query: str) -> List[BfIssue]:
        """
        Fetch BFs issues from Jira and transform it into consumable form.

        :param query: Jira query string.
        :return: BF issues.
        """
        jira_issues = self.jira_client.get_issues(query)
        return [BfIssue.from_jira_issue(issue) for issue in jira_issues]
