import unittest

import buildscripts.monitor_build_status.issue_report as under_test

ONE_DAY_HOURS = 24


class TestIssueReport(unittest.TestCase):
    def test_add_hot_bf(self):
        bf_1 = under_test.IssueTuple(
            key="BF-1",
            assigned_team="team-1",
            team_assignment_duration_hours=ONE_DAY_HOURS,
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
            team_assignment_duration_hours=ONE_DAY_HOURS,
        )
        bfs_report = under_test.IssueReport.empty()

        bfs_report.add_issue(under_test.IssueCategory.COLD, bf_1)

        self.assertEqual(len(bfs_report.team_reports["team-1"].hot), 0)
        self.assertEqual(bfs_report.team_reports["team-1"].cold, {bf_1})
        self.assertEqual(len(bfs_report.team_reports.keys()), 1)

    def test_get_issue_count_excludes_within_grace_period(self):
        grace_period_hours = 2 * ONE_DAY_HOURS

        old_issue = under_test.IssueTuple(
            key="BF-1", assigned_team="team-1", team_assignment_duration_hours=3 * ONE_DAY_HOURS
        )
        new_issue = under_test.IssueTuple(
            key="BF-2", assigned_team="team-1", team_assignment_duration_hours=ONE_DAY_HOURS
        )

        report = under_test.IssueReport.empty()
        report.add_issue(under_test.IssueCategory.HOT, old_issue)
        report.add_issue(under_test.IssueCategory.HOT, new_issue)

        count = report.get_issue_count(under_test.IssueCategory.HOT, grace_period_hours)
        self.assertEqual(count, 1)

    def test_get_issue_count_no_filter_counts_all(self):
        issue_1 = under_test.IssueTuple(
            key="BF-1", assigned_team="team-1", team_assignment_duration_hours=0
        )
        issue_2 = under_test.IssueTuple(
            key="BF-2", assigned_team="team-1", team_assignment_duration_hours=ONE_DAY_HOURS
        )

        report = under_test.IssueReport.empty()
        report.add_issue(under_test.IssueCategory.HOT, issue_1)
        report.add_issue(under_test.IssueCategory.HOT, issue_2)

        count = report.get_issue_count(under_test.IssueCategory.HOT)
        self.assertEqual(count, 2)
