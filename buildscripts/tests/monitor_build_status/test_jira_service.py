import unittest
from unittest.mock import MagicMock

import buildscripts.monitor_build_status.jira_service as under_test


class TestBfIssue(unittest.TestCase):
    def setUp(self):
        self.created = "Mon, 5 Aug 2024 15:05:10 +0000"

    def test_parse_assigned_team_from_jira_issue(self):
        team_name = "Team Name"
        jira_issue_1 = MagicMock(
            fields=MagicMock(customfield_12751=[MagicMock(value=team_name)], created=self.created)
        )
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_1)
        self.assertEqual(bf_issue.assigned_team, team_name)

        jira_issue_2 = MagicMock(fields=MagicMock(customfield_12751=[], created=self.created))
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_2)
        self.assertEqual(bf_issue.assigned_team, under_test.UNASSIGNED_LABEL)

        jira_issue_3 = MagicMock(fields=MagicMock(created=self.created))
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_3)
        self.assertEqual(bf_issue.assigned_team, under_test.UNASSIGNED_LABEL)

    def test_parse_bf_temperature_from_jira_issue(self):
        jira_issue_1 = MagicMock(fields=MagicMock(customfield_24859="hot", created=self.created))
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_1)
        self.assertEqual(bf_issue.temperature, under_test.BfTemperature.HOT)

        jira_issue_2 = MagicMock(fields=MagicMock(customfield_24859="cold", created=self.created))
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_2)
        self.assertEqual(bf_issue.temperature, under_test.BfTemperature.COLD)

        jira_issue_3 = MagicMock(fields=MagicMock(created=self.created))
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_3)
        self.assertEqual(bf_issue.temperature, under_test.BfTemperature.NONE)

    def test_parse_performance_change_type_from_jira_issue(self):
        jira_issue_1 = MagicMock(
            fields=MagicMock(
                customfield_22850=[MagicMock(value="Improvement")], created=self.created
            )
        )
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_1)
        self.assertEqual(
            bf_issue.performance_change_type, under_test.PerformanceChangeType.IMPROVEMENT
        )

        jira_issue_2 = MagicMock(
            fields=MagicMock(
                customfield_22850=[MagicMock(value="Regression")], created=self.created
            )
        )
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_2)
        self.assertEqual(
            bf_issue.performance_change_type, under_test.PerformanceChangeType.REGRESSION
        )

        jira_issue_3 = MagicMock(fields=MagicMock(created=self.created))
        bf_issue = under_test.BfIssue.from_jira_issue(jira_issue_3)
        self.assertEqual(bf_issue.performance_change_type, under_test.PerformanceChangeType.NONE)
