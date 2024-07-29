from __future__ import annotations

import os
import sys
from datetime import datetime, timedelta, timezone
from enum import Enum
from statistics import median
from typing import Dict, Iterable, List, Optional, Tuple

import structlog
import typer
from typing_extensions import Annotated

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from buildscripts.client.jiraclient import JiraAuth, JiraClient
from buildscripts.monitor_build_status.bfs_report import BFsReport
from buildscripts.monitor_build_status.evergreen_service import (
    EvergreenService,
    EvgProjectsInfo,
    TaskStatusCounts,
)
from buildscripts.monitor_build_status.jira_service import (
    BfTemperature,
    JiraCustomFieldNames,
    JiraService,
    TestType,
)
from buildscripts.resmokelib.utils.evergreen_conn import get_evergreen_api
from buildscripts.util.cmdutils import enable_logging

LOGGER = structlog.get_logger(__name__)

BLOCK_ON_RED_PLAYBOOK_URL = "http://go/blockonred"

JIRA_SERVER = "https://jira.mongodb.org"
DEFAULT_REPO = "10gen/mongo"
DEFAULT_BRANCH = "master"
SLACK_CHANNEL = "#10gen-mongo-code-lockdown"
FRIDAY_INDEX = 4  # datetime.weekday() returns Monday as 0 and Sunday as 6
EVERGREEN_LOOKBACK_DAYS = 14

OVERALL_HOT_BF_COUNT_THRESHOLD = 15
OVERALL_COLD_BF_COUNT_THRESHOLD = 50
OVERALL_PERF_BF_COUNT_THRESHOLD = 15
PER_TEAM_HOT_BF_COUNT_THRESHOLD = 3
PER_TEAM_COLD_BF_COUNT_THRESHOLD = 10
PER_TEAM_PERF_BF_COUNT_THRESHOLD = 5

GREEN_THRESHOLD_PERCENTS = 100
RED_THRESHOLD_PERCENTS = 200


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


class ExitCode(Enum):
    SUCCESS = 0
    PREVIOUS_BUILD_STATUS_UNKNOWN = 1


class BuildStatus(Enum):
    RED = "RED"
    YELLOW = "YELLOW"
    GREEN = "GREEN"
    UNKNOWN = "UNKNOWN"

    @classmethod
    def from_str(cls, status: Optional[str]) -> BuildStatus:
        try:
            return cls[status]
        except KeyError:
            return cls.UNKNOWN

    @classmethod
    def from_threshold_percentages(cls, threshold_percentages: List[float]) -> BuildStatus:
        if any(percentage > RED_THRESHOLD_PERCENTS for percentage in threshold_percentages):
            return cls.RED
        if all(percentage < GREEN_THRESHOLD_PERCENTS for percentage in threshold_percentages):
            return cls.GREEN
        return cls.YELLOW


class SummaryMsg(Enum):
    TITLE = "`[SUMMARY]`"

    BELOW_THRESHOLDS = f"All metrics are within {GREEN_THRESHOLD_PERCENTS}% of their thresholds."
    THRESHOLD_EXCEEDED = (
        f"At least one metric exceeds {GREEN_THRESHOLD_PERCENTS}% of its threshold."
    )
    THRESHOLD_EXCEEDED_X2 = (
        f"At least one metric exceeds {RED_THRESHOLD_PERCENTS}% of its threshold."
    )

    STATUS_CHANGED = "<!here> Build status has changed to `{status}`."
    STATUS_IS = "Build status is `{status}`."

    ENTER_RED = "We are entering the code block state."
    STILL_RED = "We are still in the code block state."
    EXIT_RED = "We are exiting the code block state."

    ACTION_ON_RED = "Approve only changes that fix BFs, Bugs, and Performance Regressions."
    ACTION_ON_YELLOW = (
        "Warning: all merges are still allowed, but now is a good time to get BFs, Bugs, and"
        " Performance Regressions under control to avoid a blocking state."
    )
    ACTION_ON_GREEN = "All merges are allowed."

    PLAYBOOK_REFERENCE = f"Refer to our playbook at <{BLOCK_ON_RED_PLAYBOOK_URL}> for details."


class MonitorBuildStatusOrchestrator:
    def __init__(
        self,
        jira_service: JiraService,
        evg_service: EvergreenService,
    ) -> None:
        self.jira_service = jira_service
        self.evg_service = evg_service

    def evaluate_build_redness(
        self, repo: str, branch: str, notify: bool, input_status_file: str, output_status_file: str
    ) -> ExitCode:
        exit_code = ExitCode.SUCCESS

        status_message = f"\n`[STATUS]` '{repo}' repo '{branch}' branch"
        threshold_percentages = []

        LOGGER.info("Getting Evergreen projects data")
        evg_projects_info = self.evg_service.get_evg_project_info(repo, branch)
        evg_project_names = evg_projects_info.branch_to_projects_map[branch]
        LOGGER.info("Got Evergreen projects data")

        bfs_report = self._make_bfs_report(evg_projects_info)
        bf_count_status_msg, bf_count_percentages = self._get_bf_counts_status(bfs_report)
        status_message = f"{status_message}\n{bf_count_status_msg}\n"
        threshold_percentages.extend(bf_count_percentages)

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

        previous_build_status = BuildStatus.UNKNOWN
        if os.path.exists(input_status_file):
            with open(input_status_file, "r") as input_file:
                input_file_content = input_file.read().strip()
                LOGGER.info(
                    "Input status file content",
                    file=input_status_file,
                    content=input_file_content,
                )
                previous_build_status = BuildStatus.from_str(input_file_content)
                if previous_build_status == BuildStatus.UNKNOWN:
                    LOGGER.error(
                        "Could not parse previous build status from the input build status file,"
                        " the file was corrupted or contained unexpected string"
                    )
        else:
            LOGGER.warning("Input status file is absent", file=input_status_file)
            LOGGER.warning(
                "The absence of input build status file is expected if the task is running for"
                " the first time or for the first time after the file storage location is changed"
            )

        if previous_build_status == BuildStatus.UNKNOWN:
            LOGGER.warning(
                "We were not able to get the previous build status, which could lead to build status"
                " being changed from RED to YELLOW, which should not happen, and/or summary messages"
                " could be somewhat off"
            )
            LOGGER.warning(
                "If we are in a YELLOW condition, there's different behavior to communicate if that"
                " was previously GREEN (things are worse, but not RED yet), YELLOW (no change), or"
                " RED (still RED but improving)"
            )
            LOGGER.warning(
                "The state will still be reported, but may lose accuracy on specific actions to take"
            )
            exit_code = ExitCode.PREVIOUS_BUILD_STATUS_UNKNOWN

        current_build_status = BuildStatus.from_threshold_percentages(threshold_percentages)
        resulting_build_status, summary = self._summarize(
            previous_build_status, current_build_status, self._today_is_friday()
        )
        status_message = f"{status_message}\n{summary}"

        for line in status_message.split("\n"):
            LOGGER.info(line)

        if notify:
            LOGGER.info("Notifying slack channel with results", slack_channel=SLACK_CHANNEL)
            self.evg_service.evg_api.send_slack_message(
                target=SLACK_CHANNEL,
                msg=status_message.strip(),
            )

        with open(output_status_file, "w") as output_file:
            output_file.write(resulting_build_status.value)

        return exit_code

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
    def _get_bf_counts_status(bfs_report: BFsReport) -> Tuple[str, List[float]]:
        percentages = []
        status_message = "`[STATUS]` The current BF count"
        status_message = f"{status_message}\n" f"```\n" f"{bfs_report.as_str_table()}\n" f"```"

        def _make_status_msg(scope_: str, bf_type: str, bf_count: int, threshold: int) -> str:
            percentage = bf_count / threshold * 100
            percentages.append(percentage)
            return (
                f"{scope_} {bf_type} BFs: {bf_count} ({percentage:.2f}% of threshold {threshold})"
            )

        overall_hot_bf_count = bfs_report.get_bf_count(
            test_types=[TestType.CORRECTNESS],
            bf_temperatures=[BfTemperature.HOT],
        )
        overall_cold_bf_count = bfs_report.get_bf_count(
            test_types=[TestType.CORRECTNESS],
            bf_temperatures=[BfTemperature.COLD, BfTemperature.NONE],
        )
        overall_perf_bf_count = bfs_report.get_bf_count(
            test_types=[TestType.PERFORMANCE],
            bf_temperatures=[BfTemperature.HOT, BfTemperature.COLD, BfTemperature.NONE],
        )

        scope = "Overall"
        status_message = (
            f"{status_message}"
            f"\n{_make_status_msg(scope, 'Hot', overall_hot_bf_count, OVERALL_HOT_BF_COUNT_THRESHOLD)}"
            f"\n{_make_status_msg(scope, 'Cold', overall_cold_bf_count, OVERALL_COLD_BF_COUNT_THRESHOLD)}"
            f"\n{_make_status_msg(scope, 'Perf', overall_perf_bf_count, OVERALL_PERF_BF_COUNT_THRESHOLD)}"
        )

        max_per_team_hot_bf_count = 0
        max_per_team_cold_bf_count = 0
        max_per_team_perf_bf_count = 0

        for team in bfs_report.all_assigned_teams:
            per_team_hot_bf_count = bfs_report.get_bf_count(
                test_types=[TestType.CORRECTNESS],
                bf_temperatures=[BfTemperature.HOT],
                assigned_team=team,
            )
            if per_team_hot_bf_count > max_per_team_hot_bf_count:
                max_per_team_hot_bf_count = per_team_hot_bf_count

            per_team_cold_bf_count = bfs_report.get_bf_count(
                test_types=[TestType.CORRECTNESS],
                bf_temperatures=[BfTemperature.COLD, BfTemperature.NONE],
                assigned_team=team,
            )
            if per_team_cold_bf_count > max_per_team_cold_bf_count:
                max_per_team_cold_bf_count = per_team_cold_bf_count

            per_team_perf_bf_count = bfs_report.get_bf_count(
                test_types=[TestType.PERFORMANCE],
                bf_temperatures=[BfTemperature.HOT, BfTemperature.COLD, BfTemperature.NONE],
                assigned_team=team,
            )
            if per_team_perf_bf_count > max_per_team_perf_bf_count:
                max_per_team_perf_bf_count = per_team_perf_bf_count

        scope = "Max per Assigned Team"
        status_message = (
            f"{status_message}"
            f"\n{_make_status_msg(scope, 'Hot', max_per_team_hot_bf_count, PER_TEAM_HOT_BF_COUNT_THRESHOLD)}"
            f"\n{_make_status_msg(scope, 'Cold', max_per_team_cold_bf_count, PER_TEAM_COLD_BF_COUNT_THRESHOLD)}"
            f"\n{_make_status_msg(scope, 'Perf', max_per_team_perf_bf_count, PER_TEAM_PERF_BF_COUNT_THRESHOLD)}"
        )

        return status_message, percentages

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
    def _summarize(
        previous_status: BuildStatus, current_status: BuildStatus, today_is_friday: bool
    ) -> Tuple[BuildStatus, str]:
        resulting_status = previous_status
        if current_status == BuildStatus.RED and previous_status != BuildStatus.RED:
            if today_is_friday:
                resulting_status = BuildStatus.RED
            else:
                resulting_status = BuildStatus.YELLOW
        if current_status == BuildStatus.YELLOW and previous_status != BuildStatus.RED:
            resulting_status = BuildStatus.YELLOW
        if current_status == BuildStatus.GREEN:
            resulting_status = BuildStatus.GREEN

        summary = SummaryMsg.TITLE.value

        if resulting_status != previous_status:
            status_msg = SummaryMsg.STATUS_CHANGED.value.format(status=resulting_status.value)
        else:
            status_msg = SummaryMsg.STATUS_IS.value.format(status=resulting_status.value)
        summary = f"{summary}\n{status_msg}"

        if current_status == BuildStatus.RED:
            summary = f"{summary}\n{SummaryMsg.THRESHOLD_EXCEEDED_X2.value}"
        if current_status == BuildStatus.YELLOW:
            summary = f"{summary}\n{SummaryMsg.THRESHOLD_EXCEEDED.value}"
        if current_status == BuildStatus.GREEN:
            summary = f"{summary}\n{SummaryMsg.BELOW_THRESHOLDS.value}"

        if previous_status != BuildStatus.RED and resulting_status == BuildStatus.RED:
            summary = f"{summary}\n{SummaryMsg.ENTER_RED.value}"
        if previous_status == BuildStatus.RED and resulting_status == BuildStatus.RED:
            summary = f"{summary}\n{SummaryMsg.STILL_RED.value}"
        if previous_status == BuildStatus.RED and resulting_status != BuildStatus.RED:
            summary = f"{summary}\n{SummaryMsg.EXIT_RED.value}"

        if resulting_status == BuildStatus.RED:
            summary = f"{summary}\n{SummaryMsg.ACTION_ON_RED.value}"
        if resulting_status == BuildStatus.YELLOW:
            summary = f"{summary}\n{SummaryMsg.ACTION_ON_YELLOW.value}"
        if resulting_status == BuildStatus.GREEN:
            summary = f"{summary}\n{SummaryMsg.ACTION_ON_GREEN.value}"

        summary = f"{summary}\n\n{SummaryMsg.PLAYBOOK_REFERENCE.value}\n"

        LOGGER.info(
            "Got build statuses",
            previous_build_status=previous_status.value,
            current_build_status=current_status.value,
            resulting_build_status=resulting_status.value,
            today_is_friday=today_is_friday,
        )

        return resulting_status, summary

    @staticmethod
    def _today_is_friday() -> bool:
        return datetime.utcnow().weekday() == FRIDAY_INDEX


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
    input_status_file: Annotated[
        str, typer.Option(help="The file that contains a previous build status")
    ] = "input_build_status_file.txt",
    output_status_file: Annotated[
        str, typer.Option(help="The file that contains the current build status")
    ] = "output_build_status_file.txt",
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
    orchestrator = MonitorBuildStatusOrchestrator(
        jira_service=jira_service, evg_service=evg_service
    )

    exit_code = orchestrator.evaluate_build_redness(
        github_repo, branch, notify, input_status_file, output_status_file
    )
    sys.exit(exit_code.value)


if __name__ == "__main__":
    typer.run(main)
