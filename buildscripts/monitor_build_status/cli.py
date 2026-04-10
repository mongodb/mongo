from __future__ import annotations

import os
import sys
from datetime import datetime, timedelta, timezone
from enum import Enum

import requests
import structlog
import typer
from tabulate import tabulate
from typing_extensions import Annotated

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from buildscripts.client.jiraclient import JiraAuth, JiraClient
from buildscripts.monitor_build_status.code_lockdown_config import (
    CodeLockdownConfig,
    IssueThresholds,
    NotificationsConfig,
    ScopesConfig,
)
from buildscripts.monitor_build_status.issue_report import IssueCategory, IssueReport
from buildscripts.monitor_build_status.jira_service import JiraService
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.expansions import get_expansion

LOGGER = structlog.get_logger(__name__)

CODE_LOCKDOWN_CONFIG = "etc/code_lockdown.yml"

JIRA_SERVER = "https://jira.mongodb.org"
DEFAULT_REPO = "10gen/mongo"

ORG_OVERALL = "[Org] Overall"


class CodeMergeStatus(Enum):
    RED = "RED"
    GREEN = "GREEN"

    @classmethod
    def from_threshold_percentages(cls, threshold_percentages: list[float]) -> CodeMergeStatus:
        if any(percentage > 100 for percentage in threshold_percentages):
            return cls.RED
        return cls.GREEN


class SummaryMsg(Enum):
    BELOW_THRESHOLDS = "All metrics are within 100% of their thresholds. All merges are allowed."
    THRESHOLD_EXCEEDED = (
        "At least one team exceeds 100% of its threshold. "
        "Approve only changes that fix BFs, Bugs, and Performance Regressions in the following scopes:"
    )
    ZERO_QUOTA_EXCEEDED = (
        "The following triage queues should be kept empty but currently have issues. "
        "Route or resolve these immediately:"
    )


class MonitorBuildStatusOrchestrator:
    def __init__(
        self,
        jira_service: JiraService,
        code_lockdown_config: CodeLockdownConfig,
        slack_webhook_url: str = None,
    ) -> None:
        self.jira_service = jira_service
        self.code_lockdown_config = code_lockdown_config
        self.slack_webhook_url = slack_webhook_url

    def evaluate_build_redness(self, notify: bool) -> None:
        for notification_config in self.code_lockdown_config.notifications:
            status_message = f"\n`[STATUS]` Issue count for '{DEFAULT_REPO}' repo\n"
            summaries = ""

            for scopes_config in notification_config.scopes:
                scope_percentages: dict[str, list[float]] = {}

                issue_report = self._make_report(scopes_config)
                issue_count_status_msg, issue_count_percentages, zero_quota_labels = (
                    self._get_issue_counts_status(
                        scopes_config.name, issue_report, notification_config
                    )
                )
                status_message = f"{status_message}{issue_count_status_msg}\n"
                scope_percentages.update(issue_count_percentages)

                summary = self._summarize(scopes_config.name, scope_percentages, zero_quota_labels)
                summaries = f"{summaries}{summary}\n"

            status_message = f"{status_message}{summaries}"
            status_message = f"{status_message}\n{notification_config.slack.message_footer}"

            for line in status_message.split("\n"):
                LOGGER.info(line)

            if notify:
                LOGGER.info("Sending Slack webhook notification")
                self._send_slack_webhook(status_message.strip())

    def _send_slack_webhook(self, message: str) -> None:
        """Send a message to Slack using a webhook URL from the Devprod Correctness Slack app."""
        if not self.slack_webhook_url:
            raise ValueError(
                "Slack webhook URL is required for notifications. "
                "Please provide --slack-webhook-url parameter."
            )

        payload = {"text": message}
        try:
            response = requests.post(self.slack_webhook_url, json=payload, timeout=30)
            response.raise_for_status()
            LOGGER.info("Successfully sent Slack webhook notification")
        except requests.exceptions.RequestException as e:
            LOGGER.error("Failed to send Slack webhook notification", error=str(e))
            raise

    def _make_report(self, scopes_config: ScopesConfig) -> IssueReport:
        LOGGER.info("Processing scope", name=scopes_config.name)
        hot_query = scopes_config.jira_queries.hot
        LOGGER.info("Getting hot issues from Jira", query=hot_query)
        hot_issues = self.jira_service.fetch_issues(hot_query)

        cold_query = scopes_config.jira_queries.cold
        LOGGER.info("Getting cold issues from Jira", query=cold_query)
        cold_issues = self.jira_service.fetch_issues(cold_query)

        LOGGER.info("Got active Issues", count_hot=len(hot_issues), count_cold=len(cold_issues))

        report = IssueReport.empty()
        report.add_issues(hot=hot_issues, cold=cold_issues)

        return report

    def _get_issue_counts_status(
        self, scope_name: str, issue_report: IssueReport, notification_config: NotificationsConfig
    ) -> tuple[str, dict[str, list[float]], set[str]]:
        now = datetime.now(timezone.utc)
        percentages: dict[str, list[float]] = {}
        zero_quota_labels: set[str] = set()

        headers = [scope_name, "Hot Issues", "Cold Issues"]
        table_data = []

        def _process_thresholds(
            sub_scope_name: str,
            hot_issue_count: int,
            cold_issue_count: int,
            thresholds: IssueThresholds,
            slack_tags: list[str],
        ) -> None:
            if all(count == 0 for count in [hot_issue_count, cold_issue_count]):
                return

            try:
                hot_bf_percentage = hot_issue_count / thresholds.hot.count * 100
                hot_bf_percentage_str = f"{hot_bf_percentage:.0f}"
            except ZeroDivisionError:
                if hot_issue_count > 0:
                    hot_bf_percentage = 9999
                    hot_bf_percentage_str = "∞"
                else:
                    hot_bf_percentage = 0
                    hot_bf_percentage_str = "0"

            try:
                cold_bf_percentage = cold_issue_count / thresholds.cold.count * 100
                cold_bf_percentage_str = f"{cold_bf_percentage:.0f}"
            except ZeroDivisionError:
                if cold_issue_count > 0:
                    cold_bf_percentage = 9999
                    cold_bf_percentage_str = "∞"
                else:
                    cold_bf_percentage = 0
                    cold_bf_percentage_str = "0"

            label = f"{sub_scope_name} {' '.join(slack_tags)}"
            if (thresholds.hot.count == 0 and hot_issue_count > 0) or (
                thresholds.cold.count == 0 and cold_issue_count > 0
            ):
                zero_quota_labels.add(label)
            percentages[label] = [hot_bf_percentage, cold_bf_percentage]

            if (
                sub_scope_name != ORG_OVERALL
                and notification_config.slack.short_issue_data_table
                and CodeMergeStatus.from_threshold_percentages(
                    [hot_bf_percentage, cold_bf_percentage]
                )
                == CodeMergeStatus.GREEN
            ):
                return

            table_data.append(
                [
                    sub_scope_name,
                    f"{hot_bf_percentage_str}% ({hot_issue_count} / {thresholds.hot.count})",
                    f"{cold_bf_percentage_str}% ({cold_issue_count} / {thresholds.cold.count})",
                ]
            )

        overall_thresholds = notification_config.thresholds.overall
        overall_slack_tags = notification_config.slack.overall_scope_tags
        _process_thresholds(
            ORG_OVERALL,
            issue_report.get_issue_count(
                IssueCategory.HOT,
                now - timedelta(days=overall_thresholds.hot.grace_period_days),
            ),
            issue_report.get_issue_count(
                IssueCategory.COLD,
                now - timedelta(days=overall_thresholds.cold.grace_period_days),
            ),
            overall_thresholds,
            overall_slack_tags,
        )

        for group_name in sorted(self.code_lockdown_config.get_all_group_names()):
            group_teams = self.code_lockdown_config.get_group_teams(group_name)
            group_thresholds = notification_config.thresholds.group
            group_slack_tags = self.code_lockdown_config.get_group_slack_tags(group_name)
            _process_thresholds(
                f"[Group] {group_name}",
                issue_report.get_issue_count(
                    IssueCategory.HOT,
                    now - timedelta(days=group_thresholds.hot.grace_period_days),
                    group_teams,
                ),
                issue_report.get_issue_count(
                    IssueCategory.COLD,
                    now - timedelta(days=group_thresholds.cold.grace_period_days),
                    group_teams,
                ),
                group_thresholds,
                group_slack_tags,
            )

        for assigned_team in sorted(list(issue_report.team_reports.keys())):
            team_thresholds = self.code_lockdown_config.get_team_thresholds(
                assigned_team, notification_config.thresholds.team
            )
            team_slack_tags = self.code_lockdown_config.get_team_slack_tags(assigned_team)
            _process_thresholds(
                f"[Team] {assigned_team}",
                issue_report.get_issue_count(
                    IssueCategory.HOT,
                    now - timedelta(days=team_thresholds.hot.grace_period_days),
                    [assigned_team],
                ),
                issue_report.get_issue_count(
                    IssueCategory.COLD,
                    now - timedelta(days=team_thresholds.cold.grace_period_days),
                    [assigned_team],
                ),
                team_thresholds,
                team_slack_tags,
            )

        table_str = tabulate(
            table_data, headers, tablefmt="outline", colalign=("left", "right", "right")
        )
        message = f"```\n{table_str}\n```"

        return message, percentages, zero_quota_labels

    @staticmethod
    def _summarize(
        scope_name: str,
        scope_percentages: dict[str, list[float]],
        zero_quota_labels: set[str],
    ) -> str:
        summary = f"`SUMMARY [{scope_name}]`"

        normal_red = []
        zero_quota_red = []
        for sub_scope, percentages in scope_percentages.items():
            if CodeMergeStatus.from_threshold_percentages(percentages) == CodeMergeStatus.RED:
                if sub_scope in zero_quota_labels:
                    zero_quota_red.append(sub_scope)
                else:
                    normal_red.append(sub_scope)

        if not normal_red and not zero_quota_red:
            summary = f"{summary} {SummaryMsg.BELOW_THRESHOLDS.value}"
        else:
            if normal_red:
                summary = f"{summary} {SummaryMsg.THRESHOLD_EXCEEDED.value}"
                for sub_scope in normal_red:
                    summary = f"{summary}\n\t- {sub_scope}"
            if zero_quota_red:
                summary = f"{summary}\n{SummaryMsg.ZERO_QUOTA_EXCEEDED.value}"
                for sub_scope in zero_quota_red:
                    summary = f"{summary}\n\t- {sub_scope}"

        return summary


def main(
    notify: Annotated[
        bool, typer.Option(help="Whether to send slack notification with the results")
    ] = False,  # default to the more "quiet" setting
    webhook_expansion_name: Annotated[
        str, typer.Option(help="Evergreen expansion name for the Slack webhook URL")
    ] = "mongo-code-lockdown-webhook",
) -> None:
    """
    Analyze Jira BFs count for redness reports.

    For Jira API authentication, use `JIRA_AUTH_PAT` env variable.
    More about Jira Personal Access Tokens (PATs) here:

    - https://wiki.corp.mongodb.com/pages/viewpage.action?pageId=218995581

    Example:

    JIRA_AUTH_PAT=<auth-token> python buildscripts/monitor_build_status/cli.py --help
    """
    enable_logging(verbose=False)

    # Get webhook URL from Evergreen expansion
    slack_webhook_url = get_expansion(webhook_expansion_name)

    jira_client = JiraClient(JIRA_SERVER, JiraAuth())

    jira_service = JiraService(jira_client=jira_client)
    code_lockdown_config = CodeLockdownConfig.from_yaml_config(CODE_LOCKDOWN_CONFIG)
    orchestrator = MonitorBuildStatusOrchestrator(
        jira_service=jira_service,
        code_lockdown_config=code_lockdown_config,
        slack_webhook_url=slack_webhook_url,
    )

    orchestrator.evaluate_build_redness(notify)


app = typer.Typer(pretty_exceptions_show_locals=False)
app.command()(main)

if __name__ == "__main__":
    app()
