import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import MagicMock, patch

import buildscripts.monitor_build_status.jira_service as under_test


class TestBfIssue(unittest.TestCase):
    def setUp(self):
        self.fake_now = datetime(2024, 8, 4, 12, 0, 0, tzinfo=timezone.utc)
        self.assignment_timestamp = "Thu, 3 Aug 2024 12:00:00 +0000"  # 1 day before fake_now

    def test_parse_assigned_team_from_jira_issue(self):
        team_name = "Team Name"
        jira_issue_1 = MagicMock(
            fields=MagicMock(
                customfield_12751=[MagicMock(value=team_name)],
                customfield_28352=self.assignment_timestamp,
            )
        )
        with patch("buildscripts.monitor_build_status.jira_service.datetime") as mock_dt:
            mock_dt.now.return_value = self.fake_now
            bf_issue = under_test.IssueTuple.from_jira_issue(jira_issue_1)
        self.assertEqual(bf_issue.assigned_team, team_name)

        jira_issue_2 = MagicMock(
            fields=MagicMock(
                customfield_12751=[],
                customfield_28352=self.assignment_timestamp,
            )
        )
        with patch("buildscripts.monitor_build_status.jira_service.datetime") as mock_dt:
            mock_dt.now.return_value = self.fake_now
            bf_issue = under_test.IssueTuple.from_jira_issue(jira_issue_2)
        self.assertEqual(bf_issue.assigned_team, under_test.UNASSIGNED_LABEL)

        jira_issue_3 = MagicMock(fields=MagicMock(customfield_28352=self.assignment_timestamp))
        with patch("buildscripts.monitor_build_status.jira_service.datetime") as mock_dt:
            mock_dt.now.return_value = self.fake_now
            bf_issue = under_test.IssueTuple.from_jira_issue(jira_issue_3)
        self.assertEqual(bf_issue.assigned_team, under_test.UNASSIGNED_LABEL)

    def test_parse_team_assignment_duration(self):
        # Timestamp 48 hours before fake_now
        assignment_ts = "Thu, 2 Aug 2024 12:00:00 +0000"
        jira_issue = MagicMock(
            fields=MagicMock(
                customfield_12751=[MagicMock(value="Team A")],
                customfield_28352=assignment_ts,
            )
        )
        with patch("buildscripts.monitor_build_status.jira_service.datetime") as mock_dt:
            mock_dt.now.return_value = self.fake_now
            bf_issue = under_test.IssueTuple.from_jira_issue(jira_issue)
        self.assertEqual(bf_issue.team_assignment_duration_hours, 48)

    def test_null_duration_field_falls_back_to_created_time(self):
        created = datetime(2024, 8, 1, 12, 0, 0, tzinfo=timezone.utc)
        fake_now = created + timedelta(days=3)

        jira_issue = MagicMock(
            fields=MagicMock(
                customfield_12751=[MagicMock(value="Team A")],
                customfield_28352=None,
                created="Thu, 1 Aug 2024 12:00:00 +0000",
            )
        )

        with patch("buildscripts.monitor_build_status.jira_service.datetime") as mock_dt:
            mock_dt.now.return_value = fake_now
            bf_issue = under_test.IssueTuple.from_jira_issue(jira_issue)

        self.assertEqual(bf_issue.team_assignment_duration_hours, 3 * 24)


if __name__ == "__main__":
    unittest.main()
