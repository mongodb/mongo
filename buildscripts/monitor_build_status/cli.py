from __future__ import annotations

import os
import sys
from datetime import datetime, timedelta, timezone
from enum import Enum
from typing import Dict, List, Tuple

import structlog
import typer
from tabulate import tabulate
from typing_extensions import Annotated

from evergreen import EvergreenApi

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
from buildscripts.resmokelib.utils.evergreen_conn import get_evergreen_api
from buildscripts.util.cmdutils import enable_logging

LOGGER = structlog.get_logger(__name__)

CODE_LOCKDOWN_CONFIG = "etc/code_lockdown.yml"

JIRA_SERVER = "https://jira.mongodb.org"
DEFAULT_REPO = "10gen/mongo"

ORG_OVERALL = "[Org] Overall"


class CodeMergeStatus(Enum):
    RED = "RED"
    GREEN = "GREEN"

    @classmethod
    def from_threshold_percentages(cls, threshold_percentages: List[float]) -> CodeMergeStatus:
        if any(percentage > 100 for percentage in threshold_percentages):
            return cls.RED
        return cls.GREEN


class SummaryMsg(Enum):
    BELOW_THRESHOLDS = "All metrics are within 100% of their thresholds. All merges are allowed."
    THRESHOLD_EXCEEDED = (
        "At least one metric exceeds 100% of its threshold. "
        "Approve only changes that fix BFs, Bugs, and Performance Regressions in the following scopes:"
    )


class MonitorBuildStatusOrchestrator:
    def __init__(
        self,
        jira_service: JiraService,
        evg_api: EvergreenApi,
        code_lockdown_config: CodeLockdownConfig,
    ) -> None:
        self.jira_service = jira_service
        self.evg_api = evg_api
        self.code_lockdown_config = code_lockdown_config

    def evaluate_build_redness(self, notify: bool) -> None:
        for notification_config in self.code_lockdown_config.notifications:
            status_message = f"\n`[STATUS]` Issue count for '{DEFAULT_REPO}' repo\n"
            summaries = ""

            for scopes_config in notification_config.scopes:
                scope_percentages: Dict[str, List[float]] = {}

                issue_report = self._make_report(scopes_config)
                issue_count_status_msg, issue_count_percentages = self._get_issue_counts_status(
                    scopes_config.name, issue_report, notification_config
                )
                status_message = f"{status_message}{issue_count_status_msg}\n"
                scope_percentages.update(issue_count_percentages)

                summary = self._summarize(scopes_config.name, scope_percentages)
                summaries = f"{summaries}{summary}\n"

            status_message = f"{status_message}{summaries}"
            status_message = f"{status_message}\n{notification_config.slack.message_footer}"

            for line in status_message.split("\n"):
                LOGGER.info(line)

            if notify:
                slack_channel = notification_config.slack.channel
                LOGGER.info("Notifying slack channel with results", slack_channel=slack_channel)
                self.evg_api.send_slack_message(target=slack_channel, msg=status_message.strip())

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
    ) -> Tuple[str, Dict[str, List[float]]]:
        now = datetime.utcnow().replace(tzinfo=timezone.utc)
        percentages: Dict[str, List[float]] = {}

        headers = [scope_name, "Hot Issues", "Cold Issues"]
        table_data = []

        def _process_thresholds(
            sub_scope_name: str,
            hot_issue_count: int,
            cold_issue_count: int,
            thresholds: IssueThresholds,
            slack_tags: List[str],
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
            team_thresholds = notification_config.thresholds.team
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

        return message, percentages

    @staticmethod
    def _summarize(scope_name: str, scope_percentages: Dict[str, List[float]]) -> str:
        summary = f"`SUMMARY [{scope_name}]`"

        red_sub_scopes = []
        for sub_scope, percentages in scope_percentages.items():
            status = CodeMergeStatus.from_threshold_percentages(percentages)
            if status == CodeMergeStatus.RED:
                red_sub_scopes.append(sub_scope)

        if len(red_sub_scopes) == 0:
            summary = f"{summary} {SummaryMsg.BELOW_THRESHOLDS.value}"
        else:
            summary = f"{summary} {SummaryMsg.THRESHOLD_EXCEEDED.value}"
            for sub_scope in red_sub_scopes:
                summary = f"{summary}\n\t- {sub_scope}"

        return summary


def main(
    notify: Annotated[
        bool, typer.Option(help="Whether to send slack notification with the results")
    ] = False,  # default to the more "quiet" setting
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

    jira_client = JiraClient(JIRA_SERVER, JiraAuth())
    evg_api = get_evergreen_api()

    jira_service = JiraService(jira_client=jira_client)
    code_lockdown_config = CodeLockdownConfig.from_yaml_config(CODE_LOCKDOWN_CONFIG)
    orchestrator = MonitorBuildStatusOrchestrator(
        jira_service=jira_service,
        evg_api=evg_api,
        code_lockdown_config=code_lockdown_config,
    )

    orchestrator.evaluate_build_redness(notify)


if __name__ == "__main__":
    typer.run(main)
