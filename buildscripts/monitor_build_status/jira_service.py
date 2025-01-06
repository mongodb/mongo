from __future__ import annotations

from datetime import datetime
from enum import Enum
from typing import Any, List, NamedTuple, Optional

import dateutil.parser
from jira import Issue

from buildscripts.client.jiraclient import JiraClient

UNASSIGNED_LABEL = "~ Unassigned"
NOISE_FAILURE_LABEL = "noise-failure"


class JiraCustomFieldId(str, Enum):
    ASSIGNED_TEAMS = "customfield_12751"
    TEMPERATURE = "customfield_24859"
    PERFORMANCE_CHANGE_TYPE = "customfield_22850"


class JiraCustomFieldName(str, Enum):
    EVERGREEN_PROJECT = "Evergreen Project"


class BfType(str, Enum):
    PERFORMANCE = "performance"
    HOT = "hot"
    COLD = "cold"

    @classmethod
    def from_bf_issue(cls, bf_issue: BfIssue) -> BfType:
        if (
            bf_issue.performance_change_type != PerformanceChangeType.NONE
            or NOISE_FAILURE_LABEL in bf_issue.labels
        ):
            return cls.PERFORMANCE
        if bf_issue.temperature == BfTemperature.HOT:
            return cls.HOT
        return cls.COLD


class BfTemperature(str, Enum):
    HOT = "hot"
    COLD = "cold"
    NONE = "none"

    @classmethod
    def from_str(cls, value: Optional[str]) -> BfTemperature:
        try:
            return cls(value)
        except ValueError:
            return cls.NONE


class PerformanceChangeType(str, Enum):
    IMPROVEMENT = "Improvement"
    REGRESSION = "Regression"
    NONE = "None"

    @classmethod
    def from_str(cls, value: Optional[str]) -> PerformanceChangeType:
        try:
            return cls(value)
        except ValueError:
            return cls.NONE


class BfIssue(NamedTuple):
    key: str
    assigned_team: str
    temperature: BfTemperature
    created_time: datetime
    labels: List[str]
    performance_change_type: PerformanceChangeType

    @classmethod
    def from_jira_issue(cls, issue: Issue) -> BfIssue:
        """
        Build BfIssue object from Jira Issue object.

        :param issue: Jira issue object.
        :return: BF issue object.
        """
        assigned_team = (
            cls._get_first_value(issue, JiraCustomFieldId.ASSIGNED_TEAMS) or UNASSIGNED_LABEL
        )
        temperature = BfTemperature.from_str(
            cls._get_first_value(issue, JiraCustomFieldId.TEMPERATURE)
        )
        performance_change_type = PerformanceChangeType.from_str(
            cls._get_first_value(issue, JiraCustomFieldId.PERFORMANCE_CHANGE_TYPE)
        )

        return cls(
            key=issue.key,
            assigned_team=assigned_team,
            temperature=temperature,
            created_time=dateutil.parser.parse(issue.fields.created),
            labels=issue.fields.labels,
            performance_change_type=performance_change_type,
        )

    @staticmethod
    def _get_first_value(issue: Issue, field_id: JiraCustomFieldId) -> Optional[str]:
        field_data = getattr(issue.fields, field_id)

        if isinstance(field_data, list) and len(field_data) > 0:
            values = [getattr(item, "value") for item in field_data]
            return values[0]

        if isinstance(field_data, str):
            return field_data

        return None

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
