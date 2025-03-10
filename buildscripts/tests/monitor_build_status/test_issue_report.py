import unittest
from datetime import datetime

import buildscripts.monitor_build_status.issue_report as under_test


class TestIssueReport(unittest.TestCase):
    def test_add_hot_bf(self):
        bf_1 = under_test.IssueTuple(
            key="BF-1",
            assigned_team="team-1",
            created_time=datetime.now(),
        )
        bfs_report = under_test.IssueReport.empty()

        bfs_report.add_issue(under_test.IssueCategory.HOT, bf_1)

        self.assertEqual(bfs_report.team_reports["team-1"].hot, {bf_1})
        self.assertEqual(len(bfs_report.team_reports["team-1"].cold), 0)
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_add_cold_bf(self):
        bf_1 = under_test.IssueTuple(
            key="BF-1",
            assigned_team="team-1",
            created_time=datetime.now(),
        )
        bfs_report = under_test.IssueReport.empty()

        bfs_report.add_issue(under_test.IssueCategory.COLD, bf_1)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].cold, {bf_1})
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)
