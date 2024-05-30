from __future__ import annotations

import re
from enum import Enum
from typing import List, NamedTuple, Optional, Set

from jira import Issue

from buildscripts.client.jiraclient import JiraClient
from buildscripts.monitor_build_status.evergreen_service import EvgProjectsInfo

UNASSIGNED_LABEL = "~ Unassigned"
CORRECTNESS_EVG_PROJECT_REGEX = re.compile(r"mongodb-mongo.*")
PERFORMANCE_EVG_PROJECT_REGEX = re.compile(r"sys-perf.*")


class JiraCustomFields(str, Enum):
    ASSIGNED_TEAMS = "customfield_12751"
    EVERGREEN_PROJECTS = "customfield_14278"
    TEMPERATURE = "customfield_24859"


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

    @classmethod
    def from_jira_issue(cls, issue: Issue) -> BfIssue:
        """
        Build BfIssue object from Jira Issue object.

        :param issue: Jira issue object.
        :return: BF issue object.
        """
        assigned_team = UNASSIGNED_LABEL
        assigned_teams_field = getattr(issue.fields, JiraCustomFields.ASSIGNED_TEAMS)
        if isinstance(assigned_teams_field, list) and len(assigned_teams_field) > 0:
            assigned_teams_values = [getattr(item, "value") for item in assigned_teams_field]
            assigned_team = assigned_teams_values[0]

        evergreen_projects = getattr(issue.fields, JiraCustomFields.EVERGREEN_PROJECTS)
        if not isinstance(evergreen_projects, list):
            evergreen_projects = []

        temperature_value = getattr(issue.fields, JiraCustomFields.TEMPERATURE)
        if isinstance(temperature_value, str):
            temperature = BfTemperature.from_str(temperature_value)
        else:
            temperature = BfTemperature.NONE

        return cls(
            key=issue.key,
            assigned_team=assigned_team,
            evergreen_projects=evergreen_projects,
            temperature=temperature,
        )


class BfIssues(NamedTuple):
    bfs: List[BfIssue]

    def get_all_evg_project_names(self) -> Set[str]:
        """
        Gather all evergreen project names the BFs appear on.

        :return: Set of evergreen project names.
        """
        all_bf_evg_project_names = set()
        for bf in self.bfs:
            all_bf_evg_project_names.update(bf.evergreen_projects)
        return all_bf_evg_project_names

    def filtered_bfs(self, evg_project_info: EvgProjectsInfo) -> BfIssues:
        """
        Filter BFs by evergreen project activity.

        :param evg_project_info: Active evergreen projects information.
        :return: Filtered BF issues.
        """
        return BfIssues(
            bfs=[
                bf
                for bf in self.bfs
                if any(
                    project in evg_project_info.active_project_names
                    for project in bf.evergreen_projects
                )
            ]
        )

    def get_all_assigned_teams(self) -> Set[str]:
        """
        Gather all assigned teams from BFs.

        :return: Set of assigned team names.
        """
        return {bf.assigned_team for bf in self.bfs}


class JiraService:
    def __init__(self, jira_client: JiraClient) -> None:
        self.jira_client = jira_client

    def fetch_bfs(self, query: str) -> BfIssues:
        """
        Fetch BFs issues from Jira and transform it into consumable form.

        :param query: Jira query string.
        :return: BF issues.
        """
        jira_issues = self.jira_client.get_issues(query)
        return BfIssues(bfs=[BfIssue.from_jira_issue(issue) for issue in jira_issues])
