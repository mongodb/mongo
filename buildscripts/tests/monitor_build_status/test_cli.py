import unittest

import buildscripts.monitor_build_status.cli as under_test
from buildscripts.monitor_build_status.code_lockdown_config import (
    CodeLockdownConfig,
    GroupConfig,
    IssueThresholds,
    JiraQueriesConfig,
    NotificationsConfig,
    ScopesConfig,
    SlackConfig,
    TeamConfig,
    ThresholdConfig,
    ThresholdOverride,
    ThresholdsConfig,
)
from buildscripts.monitor_build_status.issue_report import IssueCategory, IssueReport
from buildscripts.monitor_build_status.jira_service import IssueTuple


class TestSummarize(unittest.TestCase):
    def test_all_thresholds_below_100(self):
        scope_percentages = {
            "Scope 1": [0.0, 0.0, 0.0],
            "Scope 2": [0.0, 0.0, 0.0],
            "Scope 3": [0.0, 0.0, 0.0],
            "Scope 4": [0.0, 0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, set()
        )

        expected_summary = f"`SUMMARY [scope]` " f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}"

        self.assertEqual(summary, expected_summary)

    def test_all_thresholds_are_100(self):
        scope_percentages = {
            "Scope 1": [100.0, 100.0, 100.0],
            "Scope 2": [100.0, 100.0, 100.0],
            "Scope 3": [100.0, 100.0, 100.0],
            "Scope 4": [100.0, 100.0, 100.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, set()
        )

        expected_summary = f"`SUMMARY [scope]` " f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}"

        self.assertEqual(summary, expected_summary)

    def test_some_threshold_exceeded_100(self):
        scope_percentages = {
            "Scope 1": [101.0, 0.0, 0.0],
            "Scope 2": [0.0, 101.0, 0.0],
            "Scope 3": [0.0, 0.0, 101.0],
            "Scope 4": [0.0, 0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, set()
        )

        expected_summary = (
            f"`SUMMARY [scope]` "
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"\t- Scope 1\n\t- Scope 2\n\t- Scope 3"
        )

        self.assertEqual(summary, expected_summary)

    def test_zero_quota_violated_only(self):
        scope_percentages = {
            "Scope 1": [9999.0, 9999.0],
            "Scope 2": [0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, {"Scope 1"}
        )

        expected_summary = (
            f"`SUMMARY [scope]`\n"
            f"{under_test.SummaryMsg.ZERO_QUOTA_EXCEEDED.value}\n"
            f"\t- Scope 1"
        )

        self.assertEqual(summary, expected_summary)

    def test_zero_quota_violated_mixed(self):
        scope_percentages = {
            "Scope 1": [101.0, 0.0],
            "Scope 2": [9999.0, 9999.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, {"Scope 2"}
        )

        expected_summary = (
            f"`SUMMARY [scope]` "
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"\t- Scope 1\n"
            f"{under_test.SummaryMsg.ZERO_QUOTA_EXCEEDED.value}\n"
            f"\t- Scope 2"
        )

        self.assertEqual(summary, expected_summary)

    def test_zero_quota_label_green(self):
        # A label tracked as zero-quota but with no issues should still show as GREEN
        scope_percentages = {
            "Scope 1": [0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, {"Scope 1"}
        )

        expected_summary = f"`SUMMARY [scope]` " f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}"

        self.assertEqual(summary, expected_summary)


def _make_notification_config(group_hot: int = 10, group_cold: int = 7) -> NotificationsConfig:
    return NotificationsConfig(
        scopes=[ScopesConfig(name="test", jira_queries=JiraQueriesConfig(hot="", cold=""))],
        thresholds=ThresholdsConfig(
            overall=IssueThresholds(
                hot=ThresholdConfig(count=60, grace_period_days=0),
                cold=ThresholdConfig(count=40, grace_period_days=0),
            ),
            group=IssueThresholds(
                hot=ThresholdConfig(count=group_hot, grace_period_days=0),
                cold=ThresholdConfig(count=group_cold, grace_period_days=0),
            ),
            team=IssueThresholds(
                hot=ThresholdConfig(count=3, grace_period_days=0),
                cold=ThresholdConfig(count=3, grace_period_days=0),
            ),
        ),
        slack=SlackConfig(overall_scope_tags=[], message_footer=""),
    )


def _make_issue(key: str, team: str) -> IssueTuple:
    return IssueTuple(key=key, assigned_team=team, team_assignment_duration_hours=999)


class TestGetIssueCountsStatusGroupThreshold(unittest.TestCase):
    def _run(
        self,
        config: CodeLockdownConfig,
        report: IssueReport,
        notification_config: NotificationsConfig,
    ):
        orchestrator = under_test.MonitorBuildStatusOrchestrator(
            jira_service=None,
            code_lockdown_config=config,
            slack_webhook_url=None,
        )
        _, percentages, _ = orchestrator._get_issue_counts_status(
            "test", report, notification_config
        )
        return percentages

    def test_group_override_used_for_percentage_calculation(self):
        # DTA group has override hot=50; 25 hot issues → 50%, not 125% (which would be 25/20)
        config = CodeLockdownConfig(
            notifications=[],
            teams=[TeamConfig(name="Replication", slack_tags=None, thresholds=None)],
            groups=[
                GroupConfig(
                    name="DTA",
                    teams=["Replication"],
                    slack_tags=None,
                    thresholds=ThresholdOverride(hot=50, cold=25),
                )
            ],
        )
        report = IssueReport.empty()
        for i in range(25):
            report.add_issue(IssueCategory.HOT, _make_issue(f"BF-{i}", "Replication"))

        percentages = self._run(config, report, _make_notification_config())

        group_key = next(k for k in percentages if "[Group] DTA" in k)
        hot_pct, cold_pct = percentages[group_key]
        self.assertEqual(hot_pct, 50.0)  # 25/50 * 100
        self.assertEqual(cold_pct, 0.0)

    def test_group_without_override_uses_default(self):
        # Group has no override; 25 hot issues against default hot=10 → 250%
        config = CodeLockdownConfig(
            notifications=[],
            teams=[TeamConfig(name="Query Execution", slack_tags=None, thresholds=None)],
            groups=[
                GroupConfig(
                    name="Query",
                    teams=["Query Execution"],
                    slack_tags=None,
                    thresholds=None,
                )
            ],
        )
        report = IssueReport.empty()
        for i in range(25):
            report.add_issue(IssueCategory.HOT, _make_issue(f"BF-{i}", "Query Execution"))

        percentages = self._run(config, report, _make_notification_config())

        group_key = next(k for k in percentages if "[Group] Query" in k)
        hot_pct, _ = percentages[group_key]
        self.assertEqual(hot_pct, 250.0)  # 25/10 * 100


if __name__ == "__main__":
    unittest.main()
