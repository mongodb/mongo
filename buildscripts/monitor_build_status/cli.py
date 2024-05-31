from __future__ import annotations

from datetime import datetime, timedelta, timezone
from typing import Iterable, List

import structlog
import typer
from typing_extensions import Annotated

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

JIRA_SERVER = "https://jira.mongodb.org"
DEFAULT_REPO = "10gen/mongo"
DEFAULT_BRANCH = "master"
EVERGREEN_DAYS_LOOKBACK = 1

GLOBAL_CORRECTNESS_HOT_BF_COUNT_LIMIT = 0
GLOBAL_CORRECTNESS_COLD_BF_COUNT_LIMIT = 0
GLOBAL_PERFORMANCE_BF_COUNT_LIMIT = 0
PER_TEAM_CORRECTNESS_HOT_BF_COUNT_LIMIT = 0
PER_TEAM_CORRECTNESS_COLD_BF_COUNT_LIMIT = 0
PER_TEAM_PERFORMANCE_BF_COUNT_LIMIT = 0
EVERGREEN_WATERFALL_FAILURE_RATE_LIMIT = 0.00
EVERGREEN_PATCH_FAILURE_RATE_LIMIT = 0.00


def iterable_to_jql(entries: Iterable[str]) -> str:
    return ", ".join(f'"{entry}"' for entry in entries)


JIRA_PROJECTS = {"Build Failures"}
END_STATUSES = {"Closed", "Resolved"}
ACTIVE_BFS_QUERY = (
    f"project in ({iterable_to_jql(JIRA_PROJECTS)})"
    f" AND status not in ({iterable_to_jql(END_STATUSES)})"
)


class MonitorBuildStatusOrchestrator:
    def __init__(
        self,
        jira_service: JiraService,
        evg_service: EvergreenService,
    ) -> None:
        self.jira_service = jira_service
        self.evg_service = evg_service

    def evaluate_build_redness(self, repo: str, branch: str) -> None:
        failures = []

        LOGGER.info("Getting Evergreen projects data")
        evg_projects_info = self.evg_service.get_evg_project_info(repo, branch)
        evg_project_names = evg_projects_info.branch_to_projects_map[branch]
        LOGGER.info("Got Evergreen projects data")

        bfs_report = self._make_bfs_report(evg_projects_info)
        failures.extend(self._check_bf_counts(bfs_report, branch))

        # We are looking for versions and patches starting window_end the beginning of yesterday
        # to give them time to complete
        window_end = datetime.utcnow().replace(
            hour=0, minute=0, second=0, microsecond=0, tzinfo=timezone.utc
        ) - timedelta(days=1)
        window_start = window_end - timedelta(days=EVERGREEN_DAYS_LOOKBACK)

        waterfall_report = self._make_waterfall_report(
            evg_project_names=evg_project_names, window_start=window_start, window_end=window_end
        )
        failures.extend(
            self._check_waterfall_failure_rate(
                reports=waterfall_report, window_start=window_start, window_end=window_end
            )
        )

        patch_report = self._make_patch_report(
            evg_project_names=evg_project_names, window_start=window_start, window_end=window_end
        )
        failures.extend(
            self._check_patch_failure_rate(
                reports=patch_report, window_start=window_start, window_end=window_end
            )
        )

        for failure in failures:
            LOGGER.error(failure)

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
    def _check_bf_counts(bfs_report: BFsReport, branch: str) -> List[str]:
        failures = []
        bf_count_failure_msg = (
            "BF count check failed: {scope}: {bf_type}"
            " count ({bf_count}) exceeds threshold {bf_limit}"
        )

        global_correctness_hot_bf_count = bfs_report.get_bf_count(
            test_types=[TestType.CORRECTNESS],
            bf_temperatures=[BfTemperature.HOT],
        )
        if global_correctness_hot_bf_count > GLOBAL_CORRECTNESS_HOT_BF_COUNT_LIMIT:
            failures.append(
                bf_count_failure_msg.format(
                    scope=f"Branch({branch}): Global",
                    bf_type="Correctness HOT BF",
                    bf_count=global_correctness_hot_bf_count,
                    bf_limit=GLOBAL_CORRECTNESS_HOT_BF_COUNT_LIMIT,
                )
            )

        global_correctness_cold_bf_count = bfs_report.get_bf_count(
            test_types=[TestType.CORRECTNESS],
            bf_temperatures=[BfTemperature.COLD, BfTemperature.NONE],
        )
        if global_correctness_cold_bf_count > GLOBAL_CORRECTNESS_COLD_BF_COUNT_LIMIT:
            failures.append(
                bf_count_failure_msg.format(
                    scope=f"Branch({branch}): Global",
                    bf_type="Correctness COLD BF",
                    bf_count=global_correctness_cold_bf_count,
                    bf_limit=GLOBAL_CORRECTNESS_COLD_BF_COUNT_LIMIT,
                )
            )

        global_performance_bf_count = bfs_report.get_bf_count(
            test_types=[TestType.PERFORMANCE],
            bf_temperatures=[BfTemperature.HOT, BfTemperature.COLD, BfTemperature.NONE],
        )
        if global_performance_bf_count > GLOBAL_PERFORMANCE_BF_COUNT_LIMIT:
            failures.append(
                bf_count_failure_msg.format(
                    scope=f"Branch({branch}): Global",
                    bf_type="Performance BF",
                    bf_count=global_performance_bf_count,
                    bf_limit=GLOBAL_PERFORMANCE_BF_COUNT_LIMIT,
                )
            )

        for team in bfs_report.all_assigned_teams:
            per_team_correctness_hot_bf_count = bfs_report.get_bf_count(
                test_types=[TestType.CORRECTNESS],
                bf_temperatures=[BfTemperature.HOT],
                assigned_team=team,
            )
            if per_team_correctness_hot_bf_count > PER_TEAM_CORRECTNESS_HOT_BF_COUNT_LIMIT:
                failures.append(
                    bf_count_failure_msg.format(
                        scope=f"Branch({branch}): Team({team})",
                        bf_type="Correctness HOT BF",
                        bf_count=per_team_correctness_hot_bf_count,
                        bf_limit=PER_TEAM_CORRECTNESS_HOT_BF_COUNT_LIMIT,
                    )
                )

            per_team_correctness_cold_bf_count = bfs_report.get_bf_count(
                test_types=[TestType.CORRECTNESS],
                bf_temperatures=[BfTemperature.COLD, BfTemperature.NONE],
                assigned_team=team,
            )
            if per_team_correctness_cold_bf_count > PER_TEAM_CORRECTNESS_COLD_BF_COUNT_LIMIT:
                failures.append(
                    bf_count_failure_msg.format(
                        scope=f"Branch({branch}): Team({team})",
                        bf_type="Correctness COLD BF",
                        bf_count=per_team_correctness_cold_bf_count,
                        bf_limit=PER_TEAM_CORRECTNESS_COLD_BF_COUNT_LIMIT,
                    )
                )

            per_team_performance_bf_count = bfs_report.get_bf_count(
                test_types=[TestType.PERFORMANCE],
                bf_temperatures=[BfTemperature.HOT, BfTemperature.COLD, BfTemperature.NONE],
                assigned_team=team,
            )
            if per_team_performance_bf_count > PER_TEAM_PERFORMANCE_BF_COUNT_LIMIT:
                failures.append(
                    bf_count_failure_msg.format(
                        scope=f"Branch({branch}): Team({team})",
                        bf_type="Performance BF",
                        bf_count=per_team_performance_bf_count,
                        bf_limit=PER_TEAM_PERFORMANCE_BF_COUNT_LIMIT,
                    )
                )

        return failures

    def _make_waterfall_report(
        self, evg_project_names: List[str], window_start: datetime, window_end: datetime
    ) -> List[TaskStatusCounts]:
        LOGGER.info(
            "Getting Evergreen waterfall data",
            projects=evg_project_names,
            window_start=window_start.isoformat(),
            window_end=window_end.isoformat(),
        )
        waterfall_status = self.evg_service.get_waterfall_status(
            evg_project_names=evg_project_names, window_start=window_start, window_end=window_end
        )

        return self._accumulate_project_statuses(evg_project_names, waterfall_status)

    def _make_patch_report(
        self, evg_project_names: List[str], window_start: datetime, window_end: datetime
    ) -> List[TaskStatusCounts]:
        LOGGER.info(
            "Getting Evergreen patches data",
            projects=evg_project_names,
            window_start=window_start.isoformat(),
            window_end=window_end.isoformat(),
        )
        waterfall_status = self.evg_service.get_patch_statuses(
            evg_project_names=evg_project_names, window_start=window_start, window_end=window_end
        )

        return self._accumulate_project_statuses(evg_project_names, waterfall_status)

    @staticmethod
    def _accumulate_project_statuses(
        evg_project_names: List[str], build_statuses: List[TaskStatusCounts]
    ) -> List[TaskStatusCounts]:
        project_statuses = []

        for evg_project_name in evg_project_names:
            project_status = TaskStatusCounts(project=evg_project_name)
            for build_status in build_statuses:
                if build_status.project == evg_project_name:
                    project_status += build_status
            project_statuses.append(project_status)

        return project_statuses

    @staticmethod
    def _check_waterfall_failure_rate(
        reports: List[TaskStatusCounts], window_start: datetime, window_end: datetime
    ) -> List[str]:
        failures = []

        for project_status in reports:
            failure_rate = project_status.failure_rate()
            if failure_rate > EVERGREEN_WATERFALL_FAILURE_RATE_LIMIT:
                failures.append(
                    f"Waterfall redness check failed:"
                    f" Project({project_status.project}):"
                    f" Failure rate ({failure_rate})"
                    f" exceeds threshold {EVERGREEN_WATERFALL_FAILURE_RATE_LIMIT}"
                    f" between {window_start.isoformat()} and {window_end.isoformat()}"
                )

        return failures

    @staticmethod
    def _check_patch_failure_rate(
        reports: List[TaskStatusCounts], window_start: datetime, window_end: datetime
    ) -> List[str]:
        failures = []

        for project_status in reports:
            failure_rate = project_status.failure_rate()
            if failure_rate > EVERGREEN_WATERFALL_FAILURE_RATE_LIMIT:
                failures.append(
                    f"Patch redness check failed:"
                    f" Project({project_status.project}):"
                    f" Failure rate ({failure_rate})"
                    f" exceeds threshold {EVERGREEN_WATERFALL_FAILURE_RATE_LIMIT}"
                    f" between {window_start.isoformat()} and {window_end.isoformat()}"
                )

        return failures


def main(
    github_repo: Annotated[
        str, typer.Option(help="Github repository name that Evergreen projects track")
    ] = DEFAULT_REPO,
    branch: Annotated[
        str, typer.Option(help="Branch name that Evergreen projects track")
    ] = DEFAULT_BRANCH,
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
    orchestrator.evaluate_build_redness(github_repo, branch)


if __name__ == "__main__":
    typer.run(main)
