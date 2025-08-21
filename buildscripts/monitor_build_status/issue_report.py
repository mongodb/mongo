from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Dict, Iterable, List, NamedTuple, Optional, Set

from buildscripts.monitor_build_status.jira_service import IssueTuple


class IssueCategory(str, Enum):
    HOT = "hot"
    COLD = "cold"


class CategorizedIssues(NamedTuple):
    hot: Set[IssueTuple]
    cold: Set[IssueTuple]

    @classmethod
    def empty(cls) -> CategorizedIssues:
        return cls(hot=set(), cold=set())

    def add_issues(self, hot: Iterable[IssueTuple], cold: Iterable[IssueTuple]) -> None:
        self.hot.update(hot)
        self.cold.update(cold)

    def add_issue(self, category: IssueCategory, issue: IssueTuple) -> None:
        """
        Add Issue data to report.

        :param category: Issue category.
        :param issue: IssueTuple representing issue.
        """
        if category == IssueCategory.HOT:
            self.hot.add(issue)
        elif category == IssueCategory.COLD:
            self.cold.add(issue)
        else:
            raise ValueError(f"Invalid category: {category}")

    def add(self, more_issues: CategorizedIssues) -> None:
        """
        Add categorized BFs to report.

        :param more_issues: CategorizedIssues to add.
        """
        self.hot.update(more_issues.hot)
        self.cold.update(more_issues.cold)


class IssueReport(NamedTuple):
    team_reports: Dict[str, CategorizedIssues]

    @classmethod
    def empty(cls) -> IssueReport:
        return cls(team_reports={})

    def add_issues(self, hot: Iterable[IssueTuple], cold: Iterable[IssueTuple]) -> None:
        for item in hot:
            self.add_issue(IssueCategory.HOT, item)
        for item in cold:
            self.add_issue(IssueCategory.COLD, item)

    def add_issue(self, category: IssueCategory, item: IssueTuple) -> None:
        """
        Add Issue data to report.

        :param category: Issue category.
        :param item: IssueTuple representing issue.
        """
        if item.assigned_team not in self.team_reports:
            self.team_reports[item.assigned_team] = CategorizedIssues.empty()
        self.team_reports[item.assigned_team].add_issue(category, item)

    def get_issue_count(
        self,
        category: IssueCategory,
        include_items_older_than_time: Optional[datetime] = None,
        assigned_teams: Optional[List[str]] = None,
    ) -> int:
        """
        Calculate Issue count for a given criteria.

        :param category: Issue category.
        :param include_items_older_than_time: Count Items that have created date older than provided time.
        :param assigned_teams: List of Assigned teams criterion, all teams if None.
        :return: Item count.
        """
        total_issue_count = 0

        if include_items_older_than_time is None:
            include_items_older_than_time = datetime.utcnow().replace(tzinfo=timezone.utc)

        team_reports = self.team_reports.values()
        if assigned_teams is not None:
            team_reports = [
                self.team_reports.get(team, CategorizedIssues.empty()) for team in assigned_teams
            ]

        for team_report in team_reports:
            issues = set()
            if category == IssueCategory.HOT:
                issues = team_report.hot
            if category == IssueCategory.COLD:
                issues = team_report.cold
            total_issue_count += len(
                [item for item in issues if item.created_time < include_items_older_than_time]
            )

        return total_issue_count
