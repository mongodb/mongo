from __future__ import annotations

from typing import List, Optional

import yaml
from pydantic import BaseModel


class ThresholdConfig(BaseModel):
    count: int
    grace_period_days: int


class IssueThresholds(BaseModel):
    hot: ThresholdConfig
    cold: ThresholdConfig


class ThresholdsConfig(BaseModel):
    overall: IssueThresholds
    group: IssueThresholds
    team: IssueThresholds


class JiraQueriesConfig(BaseModel):
    hot: str
    cold: str


class ScopesConfig(BaseModel):
    name: str
    jira_queries: JiraQueriesConfig


class SlackConfig(BaseModel):
    channel: str
    overall_scope_tags: List[str]
    message_footer: str
    short_issue_data_table: bool = False


class NotificationsConfig(BaseModel):
    scopes: List[ScopesConfig]
    thresholds: ThresholdsConfig
    slack: SlackConfig


class TeamConfig(BaseModel):
    name: str
    slack_tags: Optional[List[str]]


class GroupConfig(BaseModel):
    name: str
    teams: List[str]
    slack_tags: Optional[List[str]]


class CodeLockdownConfig(BaseModel):
    notifications: List[NotificationsConfig]
    teams: List[TeamConfig]
    groups: List[GroupConfig]

    @classmethod
    def from_yaml_config(cls, file_path: str) -> CodeLockdownConfig:
        """
        Read the configuration from the given file.

        :param file_path: Path to file.
        :return: Config object.
        """
        with open(file_path, encoding="utf8") as file_handler:
            return cls(**yaml.safe_load(file_handler))

    def get_all_group_names(self) -> List[str]:
        """Get all group names."""
        return [group.name for group in self.groups]

    def get_group_teams(self, group_name: str) -> List[str]:
        """
        Get group teams.

        :param group_name: The name of the group.
        :return: List of teams that belongs to the group.
        """
        for group in self.groups:
            if group.name == group_name:
                return group.teams

        return []

    def get_group_slack_tags(self, group_name: str) -> List[str]:
        """
        Get group slack tags.

        :param group_name: The name of the group.
        :return: Group slack tags.
        """
        for group in self.groups:
            if group.name == group_name:
                return group.slack_tags or []

        return []

    def get_team_slack_tags(self, team_name: str) -> List[str]:
        """
        Get team slack tags.

        :param team_name: The name of the team.
        :return: Team slack tags.
        """
        for team in self.teams:
            if team.name == team_name:
                return team.slack_tags or []

        return []
