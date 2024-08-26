from __future__ import annotations

from typing import List, Optional

import yaml
from pydantic import BaseModel


class BfCountThresholds(BaseModel):
    hot_bf_count: int
    cold_bf_count: int
    perf_bf_count: int
    include_bfs_older_than_hours: int


class Defaults(BaseModel):
    thresholds: BfCountThresholds
    slack_tags: List[str]


class DefaultsConfig(BaseModel):
    overall: Defaults
    group: Defaults
    team: Defaults


class TeamConfig(BaseModel):
    name: str
    thresholds: Optional[BfCountThresholds]
    slack_tags: Optional[List[str]]


class GroupConfig(BaseModel):
    name: str
    teams: List[str]
    thresholds: Optional[BfCountThresholds]
    slack_tags: Optional[List[str]]


class CodeLockdownConfig(BaseModel):
    defaults: DefaultsConfig
    teams: List[TeamConfig]
    groups: List[GroupConfig]

    @classmethod
    def from_yaml_config(cls, file_path: str) -> CodeLockdownConfig:
        """
        Read the configuration from the given file.

        :param file_path: Path to file.
        :return: Config object.
        """
        with open(file_path) as file_handler:
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

    def get_overall_thresholds(self) -> BfCountThresholds:
        """Get overall thresholds."""
        return self.defaults.overall.thresholds

    def get_group_thresholds(self, group_name: str) -> BfCountThresholds:
        """
        Get group or default thresholds.

        :param group_name: The name of the group.
        :return: Group thresholds or default group thresholds.
        """
        for group in self.groups:
            if group.name == group_name:
                return group.thresholds or self.defaults.group.thresholds

        return self.defaults.group.thresholds

    def get_team_thresholds(self, team_name: str) -> BfCountThresholds:
        """
        Get team or default thresholds.

        :param team_name: The name of the team.
        :return: Team thresholds or default team thresholds.
        """
        for team in self.teams:
            if team.name == team_name:
                return team.thresholds or self.defaults.team.thresholds

        return self.defaults.team.thresholds

    def get_overall_slack_tags(self) -> List[str]:
        """Get overall slack tags."""
        return self.defaults.overall.slack_tags

    def get_group_slack_tags(self, group_name: str) -> List[str]:
        """
        Get group or default slack tags.

        :param group_name: The name of the group.
        :return: Group slack tags or default group slack tags.
        """
        for group in self.groups:
            if group.name == group_name:
                return group.slack_tags or self.defaults.group.slack_tags

        return self.defaults.group.slack_tags

    def get_team_slack_tags(self, team_name: str) -> List[str]:
        """
        Get team or default slack tags.

        :param team_name: The name of the team.
        :return: Team slack tags or default team slack tags.
        """
        for team in self.teams:
            if team.name == team_name:
                return team.slack_tags or self.defaults.team.slack_tags

        return self.defaults.team.slack_tags
