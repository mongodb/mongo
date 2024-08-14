from __future__ import annotations

from typing import List, Optional

import yaml
from pydantic import BaseModel


class BfCountThresholds(BaseModel):
    hot_bf_count: int
    cold_bf_count: int
    perf_bf_count: int
    include_bfs_older_than_hours: int


class TeamGroupConfig(BaseModel):
    name: str
    teams: List[str]
    thresholds: BfCountThresholds


class CodeLockdownConfig(BaseModel):
    overall_thresholds: BfCountThresholds
    team_default_thresholds: BfCountThresholds
    team_groups: Optional[List[TeamGroupConfig]]

    @classmethod
    def from_yaml_config(cls, file_path: str) -> CodeLockdownConfig:
        """
        Read the configuration from the given file.

        :param file_path: Path to file.
        :return: Config object.
        """
        with open(file_path) as file_handler:
            return cls(**yaml.safe_load(file_handler))

    def get_thresholds(self, group_name: str) -> BfCountThresholds:
        """
        Get group or default team thresholds.

        :param group_name: The name of the group.
        :return: Group thresholds or default team thresholds.
        """
        for group in self.team_groups or []:
            if group.name == group_name:
                return group.thresholds

        return self.team_default_thresholds
