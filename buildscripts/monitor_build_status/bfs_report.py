from __future__ import annotations

from typing import Any, Dict, List, NamedTuple, Optional, Set

from buildscripts.monitor_build_status.evergreen_service import EvgProjectsInfo
from buildscripts.monitor_build_status.jira_service import BfIssue, BfTemperature, TestType


class BFsTemperatureReport(NamedTuple):
    hot: Dict[str, int]
    cold: Dict[str, int]
    none: Dict[str, int]

    @classmethod
    def empty(cls) -> BFsTemperatureReport:
        return cls(hot={}, cold={}, none={})

    def add_bf_data(self, bf_temperature: BfTemperature, assigned_team: str) -> None:
        """
        Add BF data to report.

        :param bf_temperature: BF temperature.
        :param assigned_team: Assigned team.
        """
        match bf_temperature:
            case BfTemperature.HOT:
                self._increment_bf_count(self.hot, assigned_team)
            case BfTemperature.COLD:
                self._increment_bf_count(self.cold, assigned_team)
            case BfTemperature.NONE:
                self._increment_bf_count(self.none, assigned_team)

    @staticmethod
    def _increment_bf_count(bf_count_dict: Dict[str, int], assigned_team: str) -> None:
        if assigned_team not in bf_count_dict:
            bf_count_dict[assigned_team] = 0
        bf_count_dict[assigned_team] += 1


class BFsReport(NamedTuple):
    correctness: BFsTemperatureReport
    performance: BFsTemperatureReport
    unknown: BFsTemperatureReport
    all_assigned_teams: Set[str]

    @classmethod
    def empty(cls) -> BFsReport:
        return cls(
            correctness=BFsTemperatureReport.empty(),
            performance=BFsTemperatureReport.empty(),
            unknown=BFsTemperatureReport.empty(),
            all_assigned_teams=set(),
        )

    def add_bf_data(self, bf: BfIssue, evg_projects_info: EvgProjectsInfo) -> None:
        """
        Add BF data to report.

        :param bf: BF issue.
        :param evg_projects_info: Evergreen project information.
        """
        for evg_project in bf.evergreen_projects:
            if evg_project not in evg_projects_info.active_project_names:
                continue

            self.all_assigned_teams.add(bf.assigned_team)
            test_type = TestType.from_evg_project_name(evg_project)

            match test_type:
                case TestType.CORRECTNESS:
                    self.correctness.add_bf_data(bf.temperature, bf.assigned_team)
                case TestType.PERFORMANCE:
                    self.performance.add_bf_data(bf.temperature, bf.assigned_team)
                case TestType.UNKNOWN:
                    self.unknown.add_bf_data(bf.temperature, bf.assigned_team)

    def get_bf_count(
        self,
        test_types: List[TestType],
        bf_temperatures: List[BfTemperature],
        assigned_team: Optional[str] = None,
    ) -> int:
        """
        Calculate BFs count for a given criteria.

        :param test_types: List of test types (correctness, performance or unknown) criteria.
        :param bf_temperatures: List of BF temperatures (hot, cold or none) criteria.
        :param assigned_team: Assigned team criterion, all teams if None.
        :return: BFs count.
        """
        total_bf_count = 0

        test_type_reports = []
        for test_type in test_types:
            match test_type:
                case TestType.CORRECTNESS:
                    test_type_reports.append(self.correctness)
                case TestType.PERFORMANCE:
                    test_type_reports.append(self.performance)
                case TestType.UNKNOWN:
                    test_type_reports.append(self.unknown)

        bf_temp_reports = []
        for test_type_report in test_type_reports:
            for bf_temperature in bf_temperatures:
                match bf_temperature:
                    case BfTemperature.HOT:
                        bf_temp_reports.append(test_type_report.hot)
                    case BfTemperature.COLD:
                        bf_temp_reports.append(test_type_report.cold)
                    case BfTemperature.NONE:
                        bf_temp_reports.append(test_type_report.none)

        for bf_temp_report in bf_temp_reports:
            if assigned_team is None:
                total_bf_count += sum(bf_temp_report.values())
            else:
                total_bf_count += bf_temp_report.get(assigned_team, 0)

        return total_bf_count

    def as_dict(self) -> Dict[str, Any]:
        return {
            TestType.CORRECTNESS.value: self.correctness._asdict(),
            TestType.PERFORMANCE.value: self.performance._asdict(),
            TestType.UNKNOWN.value: self.unknown._asdict(),
        }
