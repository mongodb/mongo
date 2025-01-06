import unittest
from datetime import datetime

import buildscripts.monitor_build_status.bfs_report as under_test
from buildscripts.monitor_build_status.jira_service import BfTemperature, PerformanceChangeType


class TestBFsReport(unittest.TestCase):
    def test_add_hot_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            temperature=BfTemperature.HOT,
            created_time=datetime.now(),
            labels=[],
            performance_change_type=PerformanceChangeType.NONE,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1)

        self.assertEqual(bfs_report.team_reports["team-1"].hot_bfs, {bf_1})
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports["team-1"].perf_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_cold_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            temperature=BfTemperature.COLD,
            created_time=datetime.now(),
            labels=[],
            performance_change_type=PerformanceChangeType.NONE,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot_bfs), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].cold_bfs, {bf_1})
        self.assertEqual(len(bfs_report.team_reports["team-1"].perf_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_perf_regression_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            temperature=BfTemperature.HOT,
            created_time=datetime.now(),
            labels=[],
            performance_change_type=PerformanceChangeType.REGRESSION,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold_bfs), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].perf_bfs, {bf_1})
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_perf_improvement_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            temperature=BfTemperature.HOT,
            created_time=datetime.now(),
            labels=[],
            performance_change_type=PerformanceChangeType.IMPROVEMENT,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold_bfs), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].perf_bfs, {bf_1})
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_perf_noise_failure_bf(self):
        bf_1 = under_test.BfIssue(
            key="BF-1",
            assigned_team="team-1",
            temperature=BfTemperature.HOT,
            created_time=datetime.now(),
            labels=["noise-failure"],
            performance_change_type=PerformanceChangeType.NONE,
        )
        bfs_report = under_test.BFsReport.empty()

        bfs_report.add_bf_data(bf_1)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot_bfs), 0)
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold_bfs), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].perf_bfs, {bf_1})
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)
