from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Dict, List, NamedTuple, Optional, Set

from buildscripts.monitor_build_status.jira_service import BfIssue, BfType


class BfCategory(str, Enum):
    HOT = "hot"
    COLD = "cold"
    PERF = "perf"


class CategorizedBFs(NamedTuple):
    hot_bfs: Set[BfIssue]
    cold_bfs: Set[BfIssue]
    perf_bfs: Set[BfIssue]

    @classmethod
    def empty(cls) -> CategorizedBFs:
        return cls(hot_bfs=set(), cold_bfs=set(), perf_bfs=set())

    def add_bf_data(self, bf: BfIssue) -> None:
        """
        Add BF data to report.

        :param bf: BF issue.
        """
        bf_type = BfType.from_bf_issue(bf)

        if bf_type == BfType.PERFORMANCE:
            self.perf_bfs.add(bf)
        if bf_type == BfType.HOT:
            self.hot_bfs.add(bf)
        if bf_type == BfType.COLD:
            self.cold_bfs.add(bf)

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

    def add_bf_data(self, bf: BfIssue) -> None:
        """
        Add BF data to report.

        :param bf: BF issue.
        """
        if bf.assigned_team not in self.team_reports:
            self.team_reports[bf.assigned_team] = CategorizedBFs.empty()
        self.team_reports[bf.assigned_team].add_bf_data(bf)

    def get_bf_count(
        self,
        bf_category: BfCategory,
        include_bfs_older_than_time: Optional[datetime] = None,
        assigned_teams: Optional[List[str]] = None,
    ) -> int:
        """
        Calculate BFs count for a given criteria.

        :param bf_category: BF category (hot, cold, perf).
        :param include_bfs_older_than_time: Count BFs that have created date older than provided time.
        :param assigned_teams: List of Assigned teams criterion, all teams if None.
        :return: BFs count.
        """
        total_bf_count = 0

        if include_bfs_older_than_time is None:
            include_bfs_older_than_time = datetime.utcnow().replace(tzinfo=timezone.utc)

        team_reports = self.team_reports.values()
        if assigned_teams is not None:
            team_reports = [
                self.team_reports.get(team, CategorizedBFs.empty()) for team in assigned_teams
            ]

        for team_report in team_reports:
            bfs = set()
            if bf_category == BfCategory.HOT:
                bfs = team_report.hot_bfs
            if bf_category == BfCategory.COLD:
                bfs = team_report.cold_bfs
            if bf_category == BfCategory.PERF:
                bfs = team_report.perf_bfs
            total_bf_count += len(
                [bf for bf in bfs if bf.created_time < include_bfs_older_than_time]
            )

        return total_bf_count
