from __future__ import annotations

from enum import Enum
from typing import Dict, List, NamedTuple, Optional, Set

from buildscripts.monitor_build_status.evergreen_service import EvgProjectsInfo
from buildscripts.monitor_build_status.jira_service import BfIssue, BfTemperature, TestType


class BfCategory(str, Enum):
    HOT = "hot"
    COLD = "cold"
    PERF = "perf"


class CategorizedBFs(NamedTuple):
    hot_bfs: Set[str]
    cold_bfs: Set[str]
    perf_bfs: Set[str]

    @classmethod
    def empty(cls) -> CategorizedBFs:
        return cls(hot_bfs=set(), cold_bfs=set(), perf_bfs=set())

    def add_bf_data(self, bf: BfIssue, evg_projects_info: EvgProjectsInfo) -> None:
        """
        Add BF data to report.

        :param bf: BF issue.
        :param evg_projects_info: Evergreen project information.
        """
        for evg_project in bf.evergreen_projects:
            if evg_project not in evg_projects_info.active_project_names:
                continue

            test_type = TestType.from_evg_project_name(evg_project)

            if test_type == TestType.PERFORMANCE:
                self.perf_bfs.add(bf.key)

            if test_type == TestType.CORRECTNESS:
                if bf.temperature == BfTemperature.HOT:
                    self.hot_bfs.add(bf.key)
                if bf.temperature in [BfTemperature.COLD, BfTemperature.NONE]:
                    self.cold_bfs.add(bf.key)

    def add(self, more_bfs: CategorizedBFs) -> None:
        """
        Add categorized BFs to report.

        :param more_bfs: Categorized BFs to add.
        """
        self.hot_bfs.update(more_bfs.hot_bfs)
        self.cold_bfs.update(more_bfs.cold_bfs)
        self.perf_bfs.update(more_bfs.perf_bfs)


class BFsReport(NamedTuple):
    team_reports: Dict[str, CategorizedBFs]

    @classmethod
    def empty(cls) -> BFsReport:
        return cls(team_reports={})

    def add_bf_data(self, bf: BfIssue, evg_projects_info: EvgProjectsInfo) -> None:
        """
        Add BF data to report.

        :param bf: BF issue.
        :param evg_projects_info: Evergreen project information.
        """
        if bf.assigned_team not in self.team_reports:
            self.team_reports[bf.assigned_team] = CategorizedBFs.empty()
        self.team_reports[bf.assigned_team].add_bf_data(bf, evg_projects_info)

    def get_bf_count(
        self,
        bf_category: BfCategory,
        assigned_team: Optional[str] = None,
    ) -> int:
        """
        Calculate BFs count for a given criteria.

        :param bf_category: BF category (hot, cold, perf).
        :param assigned_team: Assigned team criterion, all teams if None.
        :return: BFs count.
        """
        total_bf_count = 0

        team_reports = self.team_reports.values()
        if assigned_team is not None:
            team_reports = [self.team_reports[assigned_team]]

        for team_report in team_reports:
            if bf_category == BfCategory.HOT:
                total_bf_count += len(team_report.hot_bfs)
            if bf_category == BfCategory.COLD:
                total_bf_count += len(team_report.cold_bfs)
            if bf_category == BfCategory.PERF:
                total_bf_count += len(team_report.perf_bfs)

        return total_bf_count

    def combine_teams(self, team_names: List[str], group_name: str) -> None:
        """
        Mutate `self.team_reports` by removing `team_names` entries and combining their BFs under `group_name`.

        :param team_names: Names of teams to combine BFs.
        :param group_name: Group name to combine BFs under.
        """
        group_bfs = CategorizedBFs.empty()

        for team in team_names:
            team_bfs = self.team_reports.pop(team, None)
            if team_bfs is not None:
                group_bfs.add(team_bfs)

        self.team_reports[group_name] = group_bfs
