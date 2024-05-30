from __future__ import annotations

from typing import Iterable, List, Set

import structlog
import typer

from buildscripts.client.jiraclient import JiraAuth, JiraClient
from buildscripts.monitor_build_status.bf_report import BfReport
from buildscripts.monitor_build_status.evergreen_service import EvergreenService
from buildscripts.monitor_build_status.jira_service import BfTemperature, JiraService, TestType
from buildscripts.resmokelib.utils.evergreen_conn import get_evergreen_api
from buildscripts.util.cmdutils import enable_logging

LOGGER = structlog.get_logger(__name__)

JIRA_SERVER = "https://jira.mongodb.org"
TRACKING_REPO = "10gen/mongo"
BRANCHES_TO_CHECK = ["master"]

GLOBAL_CORRECTNESS_HOT_BF_COUNT_LIMIT = 30
GLOBAL_CORRECTNESS_COLD_BF_COUNT_LIMIT = 100
GLOBAL_PERFORMANCE_BF_COUNT_LIMIT = 30
PER_TEAM_CORRECTNESS_HOT_BF_COUNT_LIMIT = 7
PER_TEAM_CORRECTNESS_COLD_BF_COUNT_LIMIT = 20
PER_TEAM_PERFORMANCE_BF_COUNT_LIMIT = 10


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

    def evaluate_build_redness(self) -> None:
        LOGGER.info("Getting active BFs from Jira")
        active_bfs = self.jira_service.fetch_bfs(ACTIVE_BFS_QUERY)
        LOGGER.info("Got active BFs", count=len(active_bfs.bfs))

        evg_projects_info = self.evg_service.get_evg_project_info(
            active_bfs.get_all_evg_project_names(), TRACKING_REPO
        )
        filtered_bfs = active_bfs.filtered_bfs(evg_projects_info)
        LOGGER.info("Filtered BFs by active evergreen projects", count=len(filtered_bfs.bfs))

        bfs_report = BfReport.empty(evg_projects_info.tracking_branches)
        for bf in filtered_bfs.bfs:
            bfs_report.add_bf_data(bf, evg_projects_info)

        failures = []

        all_assigned_teams = filtered_bfs.get_all_assigned_teams()
        failures.extend(self._check_bf_counts(bfs_report, BRANCHES_TO_CHECK, all_assigned_teams))

        # TODO SERVER-90908: fetch evergreen data
        # TODO SERVER-90908: check evergreen redness

        for failure in failures:
            LOGGER.error(failure)

    @staticmethod
    def _check_bf_counts(
        bfs_report: BfReport, branches_to_check: List[str], all_assigned_teams: Set[str]
    ) -> List[str]:
        failures = []
        bf_count_failure_msg = (
            "BF count check failed: {scope}: {bf_type} count ({bf_count}) is more than {bf_limit}"
        )

        for branch in branches_to_check:
            global_correctness_hot_bf_count = bfs_report.get_bf_count(
                branch=branch,
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
                branch=branch,
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
                branch=branch,
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

            for team in all_assigned_teams:
                per_team_correctness_hot_bf_count = bfs_report.get_bf_count(
                    branch=branch,
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
                    branch=branch,
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
                    branch=branch,
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


def main() -> None:
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
    orchestrator.evaluate_build_redness()


if __name__ == "__main__":
    typer.run(main)
