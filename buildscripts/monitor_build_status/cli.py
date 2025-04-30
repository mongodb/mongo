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

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from buildscripts.client.jiraclient import JiraAuth, JiraClient
from buildscripts.monitor_build_status.code_lockdown_config import (
    CodeLockdownConfig,
    IssueThresholds,
)
from buildscripts.monitor_build_status.issue_report import IssueCategory, IssueReport
from buildscripts.monitor_build_status.jira_service import JiraService
from buildscripts.resmokelib.utils.evergreen_conn import get_evergreen_api
from buildscripts.util.cmdutils import enable_logging

LOGGER = structlog.get_logger(__name__)

BLOCK_ON_RED_PLAYBOOK_URL = "http://go/blockonred"
DASHBOARD_URL = "https://jira.mongodb.org/secure/Dashboard.jspa?selectPageId=33310"
CODE_LOCKDOWN_CONFIG = "etc/code_lockdown.yml"

JIRA_SERVER = "https://jira.mongodb.org"
DEFAULT_REPO = "10gen/mongo"
DEFAULT_BRANCH = "master"
SLACK_CHANNEL = "#10gen-mongo-code-lockdown"


# filter 53085 is all issues in scope
# filter 53200 identifies those which are hot
HOT_QUERY = "filter = 53085 AND filter = 53200"
COLD_QUERY = "filter = 53085 AND filter != 53200"


class CodeMergeStatus(Enum):
    RED = "RED"
    GREEN = "GREEN"

    @classmethod
    def from_threshold_percentages(cls, threshold_percentages: List[float]) -> CodeMergeStatus:
        if any(percentage > 100 for percentage in threshold_percentages):
            return cls.RED
        return cls.GREEN


class SummaryMsg(Enum):
    PREFIX = "`[SUMMARY]`"

    BELOW_THRESHOLDS = "All metrics are within 100% of their thresholds.\nAll merges are allowed."
    THRESHOLD_EXCEEDED = (
        "At least one metric exceeds 100% of its threshold.\n"
        "Approve only changes that fix BFs, Bugs, and Performance Regressions in the following scopes:"
    )

    PLAYBOOK_REFERENCE = f"Refer to our playbook at <{BLOCK_ON_RED_PLAYBOOK_URL}> for details."
    DASHBOARD_REFERENCE = f"Drill into the data using the <{DASHBOARD_URL}|Jira Dashboard>."


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

    def evaluate_build_redness(self, repo: str, branch: str, notify: bool) -> None:
        status_message = f"\n`[STATUS]` Issue count for '{repo}' repo '{branch}' branch"
        scope_percentages: Dict[str, List[float]] = {}

        issue_report = self._make_report()
        issue_count_status_msg, issue_count_percentages = self._get_issue_counts_status(
            issue_report, self.code_lockdown_config
        )
        status_message = f"{status_message}\n{issue_count_status_msg}\n"
        scope_percentages.update(issue_count_percentages)

        summary = self._summarize(scope_percentages)
        status_message = f"{status_message}\n{summary}"

        for line in status_message.split("\n"):
            LOGGER.info(line)

        if notify:
            LOGGER.info("Notifying slack channel with results", slack_channel=SLACK_CHANNEL)
            self.evg_api.send_slack_message(
                target=SLACK_CHANNEL,
                msg=status_message.strip(),
            )

    def _make_report(self) -> IssueReport:
        LOGGER.info("Getting hot issues from Jira", query=HOT_QUERY)
        hot_issues = self.jira_service.fetch_issues(HOT_QUERY)
        LOGGER.info("Getting cold issues from Jira", query=COLD_QUERY)
        cold_issues = self.jira_service.fetch_issues(COLD_QUERY)

        LOGGER.info("Got active Issues", count_hot=len(hot_issues), count_cold=len(cold_issues))

        report = IssueReport.empty()
        report.add_issues(hot=hot_issues, cold=cold_issues)

        return report

    @staticmethod
    def _get_issue_counts_status(
        bfs_report: IssueReport, code_lockdown_config: CodeLockdownConfig
    ) -> Tuple[str, Dict[str, List[float]]]:
        now = datetime.utcnow().replace(tzinfo=timezone.utc)
        percentages: Dict[str, List[float]] = {}

        headers = ["Scope", "Hot Issues", "Cold Issues"]
        table_data = []

        def _process_thresholds(
            scope: str,
            hot_issue_count: int,
            cold_issue_count: int,
            thresholds: IssueThresholds,
            slack_tags: List[str],
        ) -> None:
            if all(count == 0 for count in [hot_issue_count, cold_issue_count]):
                return

            hot_bf_percentage = hot_issue_count / thresholds.hot.count * 100
            cold_bf_percentage = cold_issue_count / thresholds.cold.count * 100

            label = f"{scope} {' '.join(slack_tags)}"
            percentages[label] = [hot_bf_percentage, cold_bf_percentage]

            table_data.append(
                [
                    scope,
                    f"{hot_bf_percentage:.0f}% ({hot_issue_count} / {thresholds.hot.count})",
                    f"{cold_bf_percentage:.0f}% ({cold_issue_count} / {thresholds.cold.count})",
                ]
            )

        overall_thresholds = code_lockdown_config.get_overall_thresholds()
        overall_slack_tags = code_lockdown_config.get_overall_slack_tags()
        _process_thresholds(
            "[Org] Overall",
            bfs_report.get_issue_count(
                IssueCategory.HOT,
                now - timedelta(days=overall_thresholds.hot.grace_period_days),
            ),
            bfs_report.get_issue_count(
                IssueCategory.COLD,
                now - timedelta(days=overall_thresholds.cold.grace_period_days),
            ),
            overall_thresholds,
            overall_slack_tags,
        )

        for group_name in sorted(code_lockdown_config.get_all_group_names()):
            group_teams = code_lockdown_config.get_group_teams(group_name)
            group_thresholds = code_lockdown_config.get_group_thresholds(group_name)
            group_slack_tags = code_lockdown_config.get_group_slack_tags(group_name)
            _process_thresholds(
                f"[Group] {group_name}",
                bfs_report.get_issue_count(
                    IssueCategory.HOT,
                    now - timedelta(days=group_thresholds.hot.grace_period_days),
                    group_teams,
                ),
                bfs_report.get_issue_count(
                    IssueCategory.COLD,
                    now - timedelta(days=group_thresholds.cold.grace_period_days),
                    group_teams,
                ),
                group_thresholds,
                group_slack_tags,
            )

        for assigned_team in sorted(list(bfs_report.team_reports.keys())):
            team_thresholds = code_lockdown_config.get_team_thresholds(assigned_team)
            team_slack_tags = code_lockdown_config.get_team_slack_tags(assigned_team)
            _process_thresholds(
                f"[Team] {assigned_team}",
                bfs_report.get_issue_count(
                    IssueCategory.HOT,
                    now - timedelta(days=team_thresholds.hot.grace_period_days),
                    [assigned_team],
                ),
                bfs_report.get_issue_count(
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
    def _summarize(scope_percentages: Dict[str, List[float]]) -> str:
        summary = SummaryMsg.PREFIX.value

        red_scopes = []
        for scope, percentages in scope_percentages.items():
            status = CodeMergeStatus.from_threshold_percentages(percentages)
            if status == CodeMergeStatus.RED:
                red_scopes.append(scope)

        if len(red_scopes) == 0:
            summary = f"{summary} {SummaryMsg.BELOW_THRESHOLDS.value}"
        else:
            summary = f"{summary} {SummaryMsg.THRESHOLD_EXCEEDED.value}"
            for scope in red_scopes:
                summary = f"{summary}\n\t- {scope}"

        summary = f"{summary}\n\n{SummaryMsg.PLAYBOOK_REFERENCE.value}\n{SummaryMsg.DASHBOARD_REFERENCE.value}"

        return summary


def main(
    github_repo: Annotated[
        str, typer.Option(help="Github repository name that Evergreen projects track")
    ] = DEFAULT_REPO,
    branch: Annotated[
        str, typer.Option(help="Branch name that Evergreen projects track")
    ] = DEFAULT_BRANCH,
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

    orchestrator.evaluate_build_redness(github_repo, branch, notify)


if __name__ == "__main__":
    typer.run(main)
