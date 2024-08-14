from __future__ import annotations

import os
import sys
from datetime import datetime, timedelta, timezone
from enum import Enum
from statistics import median
from typing import Dict, Iterable, List, Optional, Tuple

import structlog
import typer
from tabulate import tabulate
from typing_extensions import Annotated

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from buildscripts.client.jiraclient import JiraAuth, JiraClient
from buildscripts.monitor_build_status.bfs_report import BfCategory, BFsReport
from buildscripts.monitor_build_status.code_lockdown_config import (
    BfCountThresholds,
    CodeLockdownConfig,
)
from buildscripts.monitor_build_status.evergreen_service import (
    EvergreenService,
    EvgProjectsInfo,
    TaskStatusCounts,
)
from buildscripts.monitor_build_status.jira_service import (
    JiraCustomFieldNames,
    JiraService,
)
from buildscripts.resmokelib.utils.evergreen_conn import get_evergreen_api
from buildscripts.util.cmdutils import enable_logging

LOGGER = structlog.get_logger(__name__)

BLOCK_ON_RED_PLAYBOOK_URL = "http://go/blockonred"
CODE_LOCKDOWN_CONFIG = "etc/code_lockdown.yml"

JIRA_SERVER = "https://jira.mongodb.org"
DEFAULT_REPO = "10gen/mongo"
DEFAULT_BRANCH = "master"
SLACK_CHANNEL = "#10gen-mongo-code-lockdown"
EVERGREEN_LOOKBACK_DAYS = 14


def iterable_to_jql(entries: Iterable[str]) -> str:
    return ", ".join(f'"{entry}"' for entry in entries)


JIRA_PROJECTS = {"Build Failures"}
END_STATUSES = {"Needs Triage", "Open", "In Progress", "Waiting for Bug Fix"}
PRIORITIES = {"Blocker - P1", "Critical - P2", "Major - P3", "Minor - P4"}
EXCLUDE_LABELS = {"exclude-from-master-quota"}
ACTIVE_BFS_QUERY = (
    f"project in ({iterable_to_jql(JIRA_PROJECTS)})"
    f" AND status in ({iterable_to_jql(END_STATUSES)})"
    f" AND priority in ({iterable_to_jql(PRIORITIES)})"
    f" AND (labels not in ({iterable_to_jql(EXCLUDE_LABELS)}) OR labels is EMPTY)"
)


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


class MonitorBuildStatusOrchestrator:
    def __init__(
        self,
        jira_service: JiraService,
        evg_service: EvergreenService,
        code_lockdown_config: CodeLockdownConfig,
    ) -> None:
        self.jira_service = jira_service
        self.evg_service = evg_service
        self.code_lockdown_config = code_lockdown_config

    def evaluate_build_redness(self, repo: str, branch: str, notify: bool) -> None:
        status_message = f"\n`[STATUS]` '{repo}' repo '{branch}' branch"
        scope_percentages: Dict[str, List[float]] = {}

        LOGGER.info("Getting Evergreen projects data")
        evg_projects_info = self.evg_service.get_evg_project_info(repo, branch)
        evg_project_names = evg_projects_info.branch_to_projects_map[branch]
        LOGGER.info("Got Evergreen projects data")

        bfs_report = self._make_bfs_report(evg_projects_info)
        bf_count_status_msg, bf_count_percentages = self._get_bf_counts_status(
            bfs_report, self.code_lockdown_config
        )
        status_message = f"{status_message}\n{bf_count_status_msg}\n"
        scope_percentages.update(bf_count_percentages)

        # We are looking for Evergreen versions that started before the beginning of yesterday
        # to give them time to complete
        window_end = datetime.utcnow().replace(
            hour=0, minute=0, second=0, microsecond=0, tzinfo=timezone.utc
        ) - timedelta(days=1)
        window_start = window_end - timedelta(days=EVERGREEN_LOOKBACK_DAYS)

        waterfall_report = self._make_waterfall_report(
            evg_project_names=evg_project_names, window_end=window_end
        )
        waterfall_failure_rate_status_msg = self._get_waterfall_redness_status(
            waterfall_report=waterfall_report, window_start=window_start, window_end=window_end
        )
        status_message = f"{status_message}\n{waterfall_failure_rate_status_msg}\n"

        summary = self._summarize(scope_percentages)
        status_message = f"{status_message}\n{summary}"

        for line in status_message.split("\n"):
            LOGGER.info(line)

        if notify:
            LOGGER.info("Notifying slack channel with results", slack_channel=SLACK_CHANNEL)
            self.evg_service.evg_api.send_slack_message(
                target=SLACK_CHANNEL,
                msg=status_message.strip(),
            )

    def _make_bfs_report(self, evg_projects_info: EvgProjectsInfo) -> BFsReport:
        query = (
            f'{ACTIVE_BFS_QUERY} AND "{JiraCustomFieldNames.EVERGREEN_PROJECT}" in'
            f" ({iterable_to_jql(evg_projects_info.active_project_names)})"
        )
        LOGGER.info("Getting active BFs from Jira", query=query)

        active_bfs = self.jira_service.fetch_bfs(query)
        LOGGER.info("Got active BFs", count=len(active_bfs))

        bfs_report = BFsReport.empty()
        for bf in active_bfs:
            bfs_report.add_bf_data(bf, evg_projects_info)

        return bfs_report

    @staticmethod
    def _get_bf_counts_status(
        bfs_report: BFsReport, code_lockdown_config: CodeLockdownConfig
    ) -> Tuple[str, Dict[str, List[float]]]:
        now = datetime.utcnow().replace(tzinfo=timezone.utc)
        scope_percentages: Dict[str, List[float]] = {}

        status_message = "`[STATUS]` The current BF count"
        headers = ["Scope", "Hot BFs", "Cold BFs", "Perf BFs"]
        table_data = []

        def _process_thresholds(
            scope: str,
            hot_bf_count: int,
            cold_bf_count: int,
            perf_bf_count: int,
            thresholds: BfCountThresholds,
        ) -> None:
            if all(count == 0 for count in [hot_bf_count, cold_bf_count, perf_bf_count]):
                return

            hot_bf_percentage = hot_bf_count / thresholds.hot_bf_count * 100
            cold_bf_percentage = cold_bf_count / thresholds.cold_bf_count * 100
            perf_bf_percentage = perf_bf_count / thresholds.perf_bf_count * 100

            scope_percentages[scope] = [hot_bf_percentage, cold_bf_percentage, perf_bf_percentage]

            table_data.append(
                [
                    scope,
                    f"{hot_bf_percentage:.0f}% ({hot_bf_count} / {thresholds.hot_bf_count})",
                    f"{cold_bf_percentage:.0f}% ({cold_bf_count} / {thresholds.cold_bf_count})",
                    f"{perf_bf_percentage:.0f}% ({perf_bf_count} / {thresholds.perf_bf_count})",
                ]
            )

        overall_older_than_time = now - timedelta(
            hours=code_lockdown_config.overall_thresholds.include_bfs_older_than_hours
        )
        _process_thresholds(
            "[Org] Overall",
            bfs_report.get_bf_count(BfCategory.HOT, overall_older_than_time),
            bfs_report.get_bf_count(BfCategory.COLD, overall_older_than_time),
            bfs_report.get_bf_count(BfCategory.PERF, overall_older_than_time),
            code_lockdown_config.overall_thresholds,
        )

        for group in code_lockdown_config.team_groups or []:
            group_thresholds = code_lockdown_config.get_thresholds(group.name)
            group_older_than_time = now - timedelta(
                hours=group_thresholds.include_bfs_older_than_hours
            )
            _process_thresholds(
                f"[Group] {group.name}",
                bfs_report.get_bf_count(BfCategory.HOT, group_older_than_time, group.teams),
                bfs_report.get_bf_count(BfCategory.COLD, group_older_than_time, group.teams),
                bfs_report.get_bf_count(BfCategory.PERF, group_older_than_time, group.teams),
                group_thresholds,
            )

        for assigned_team in sorted(list(bfs_report.team_reports.keys())):
            team_thresholds = code_lockdown_config.get_thresholds(assigned_team)
            team_older_than_time = now - timedelta(
                hours=team_thresholds.include_bfs_older_than_hours
            )
            _process_thresholds(
                f"[Team] {assigned_team}",
                bfs_report.get_bf_count(BfCategory.HOT, team_older_than_time, [assigned_team]),
                bfs_report.get_bf_count(BfCategory.COLD, team_older_than_time, [assigned_team]),
                bfs_report.get_bf_count(BfCategory.PERF, team_older_than_time, [assigned_team]),
                team_thresholds,
            )

        table_str = tabulate(
            table_data, headers, tablefmt="outline", colalign=("left", "right", "right", "right")
        )
        status_message = f"{status_message}\n```\n{table_str}\n```"

        return status_message, scope_percentages

    def _make_waterfall_report(
        self, evg_project_names: List[str], window_end: datetime
    ) -> Dict[str, List[TaskStatusCounts]]:
        task_status_counts = []
        for day in range(EVERGREEN_LOOKBACK_DAYS):
            day_window_end = window_end - timedelta(days=day)
            day_window_start = day_window_end - timedelta(days=1)
            LOGGER.info(
                "Getting Evergreen waterfall data",
                projects=evg_project_names,
                window_start=day_window_start.isoformat(),
                window_end=day_window_end.isoformat(),
            )
            waterfall_status = self.evg_service.get_waterfall_status(
                evg_project_names=evg_project_names,
                window_start=day_window_start,
                window_end=day_window_end,
            )
            task_status_counts.extend(
                self._accumulate_project_statuses(evg_project_names, waterfall_status)
            )

        waterfall_report = {evg_project_name: [] for evg_project_name in evg_project_names}
        for task_status_count in task_status_counts:
            waterfall_report[task_status_count.project].append(task_status_count)

        return waterfall_report

    @staticmethod
    def _accumulate_project_statuses(
        evg_project_names: List[str], build_statuses: List[TaskStatusCounts]
    ) -> List[TaskStatusCounts]:
        project_statuses = []

        for evg_project_name in evg_project_names:
            project_status = TaskStatusCounts(project=evg_project_name)
            for build_status in build_statuses:
                if build_status.project == evg_project_name:
                    project_status = project_status.add(build_status)
            project_statuses.append(project_status)

        return project_statuses

    @staticmethod
    def _get_waterfall_redness_status(
        waterfall_report: Dict[str, List[TaskStatusCounts]],
        window_start: datetime,
        window_end: datetime,
    ) -> str:
        date_format = "%Y-%m-%d"
        status_message = (
            f"`[STATUS]` Evergreen waterfall red and purple boxes median count per day"
            f" between {window_start.strftime(date_format)}"
            f" and {window_end.strftime(date_format)}"
        )

        for evg_project_name, daily_task_status_counts in waterfall_report.items():
            daily_per_project_red_box_counts = [
                task_status_counts.failed for task_status_counts in daily_task_status_counts
            ]
            LOGGER.info(
                "Daily per project red box counts",
                project=evg_project_name,
                daily_red_box_counts=daily_per_project_red_box_counts,
            )
            median_per_day_red_box_count = median(daily_per_project_red_box_counts)
            status_message = (
                f"{status_message}\n{evg_project_name}: {median_per_day_red_box_count:.0f}"
            )

        return status_message

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

        summary = f"{summary}\n\n{SummaryMsg.PLAYBOOK_REFERENCE.value}\n"

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
    Analyze Jira BFs count and Evergreen redness data.

    For Jira API authentication please use `JIRA_AUTH_PAT` env variable.
    More about Jira Personal Access Tokens (PATs) here:

    - https://wiki.corp.mongodb.com/pages/viewpage.action?pageId=218995581

    For Evergreen API authentication please create `~/.evergreen.yml`.
    More about Evergreen auth here:

    - https://spruce.mongodb.com/preferences/cli

    Example:

    JIRA_AUTH_PAT=<auth-token> python buildscripts/monitor_build_status/cli.py --help
    """
    enable_logging(verbose=False)

    jira_client = JiraClient(JIRA_SERVER, JiraAuth())
    evg_api = get_evergreen_api()

    jira_service = JiraService(jira_client=jira_client)
    evg_service = EvergreenService(evg_api=evg_api)
    code_lockdown_config = CodeLockdownConfig.from_yaml_config(CODE_LOCKDOWN_CONFIG)
    orchestrator = MonitorBuildStatusOrchestrator(
        jira_service=jira_service,
        evg_service=evg_service,
        code_lockdown_config=code_lockdown_config,
    )

    orchestrator.evaluate_build_redness(github_repo, branch, notify)


if __name__ == "__main__":
    typer.run(main)
