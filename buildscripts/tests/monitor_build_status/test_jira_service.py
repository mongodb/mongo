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
        bf_issue = under_test.IssueTuple.from_jira_issue(jira_issue_1)
        self.assertEqual(bf_issue.assigned_team, team_name)

        jira_issue_2 = MagicMock(fields=MagicMock(customfield_12751=[], created=self.created))
        bf_issue = under_test.IssueTuple.from_jira_issue(jira_issue_2)
        self.assertEqual(bf_issue.assigned_team, under_test.UNASSIGNED_LABEL)

        jira_issue_3 = MagicMock(fields=MagicMock(created=self.created))
        bf_issue = under_test.IssueTuple.from_jira_issue(jira_issue_3)
        self.assertEqual(bf_issue.assigned_team, under_test.UNASSIGNED_LABEL)
