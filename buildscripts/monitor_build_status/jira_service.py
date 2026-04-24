from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Any, NamedTuple, Optional

import dateutil.parser
from jira import Issue

from buildscripts.client.jiraclient import JiraClient

UNASSIGNED_LABEL = "~ Unassigned"


class JiraCustomFieldId(str, Enum):
    ASSIGNED_TEAMS = "customfield_12751"
    TEAM_ASSIGNMENT_DURATION = "customfield_28251"


class IssueTuple(NamedTuple):
    key: str
    assigned_team: str
    team_assignment_duration_hours: int

    @classmethod
    def from_jira_issue(cls, issue: Issue) -> IssueTuple:
        """
        Build IssueTuple object from Jira Issue object.

        :param issue: Jira issue object.
        :return: IssueTuple object.
        """
        assigned_team = (
            cls._get_first_value(issue, JiraCustomFieldId.ASSIGNED_TEAMS) or UNASSIGNED_LABEL
        )
        raw_duration = getattr(issue.fields, JiraCustomFieldId.TEAM_ASSIGNMENT_DURATION, None)
        if raw_duration is not None:
            duration_hours = int(raw_duration)
        else:
            created_time = dateutil.parser.parse(issue.fields.created)
            duration_hours = int((datetime.now(timezone.utc) - created_time).total_seconds() / 3600)

        return cls(
            key=issue.key,
            assigned_team=assigned_team,
            team_assignment_duration_hours=duration_hours,
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

    def fetch_issues(self, query: str) -> list[IssueTuple]:
        """
        Fetch issues from Jira and transform it into consumable form.

        :param query: Jira query string.
        :return: list of IssueTuple representing found issues.
        """
        jira_issues = self.jira_client.get_issues(query)
        return [IssueTuple.from_jira_issue(issue) for issue in jira_issues]
