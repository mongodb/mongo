from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from typing import Any, Dict, List, NamedTuple, Optional

import structlog

from evergreen import EvergreenApi

LOGGER = structlog.get_logger(__name__)

TASK_FAILED_STATUSES = ["failed", "timed_out"]


class EvgProjectsInfo(NamedTuple):
    project_to_branch_map: Dict[str, str]
    branch_to_projects_map: Dict[str, List[str]]
    active_project_names: List[str]
    tracking_branches: List[str]

    @classmethod
    def from_project_branch_map(cls, project_to_branch_map: Dict[str, str]) -> EvgProjectsInfo:
        """
        Build EvgProjectsInfo object from evergreen project name to its tracking branch map.

        :param project_to_branch_map: Evergreen project name to its tracking branch map.
        :return: Evergreen projects information.
        """
        branch_to_projects_map = {}
        for project, branch in project_to_branch_map.items():
            if branch not in branch_to_projects_map:
                branch_to_projects_map[branch] = []
            branch_to_projects_map[branch].append(project)

        return cls(
            project_to_branch_map=project_to_branch_map,
            branch_to_projects_map=branch_to_projects_map,
            active_project_names=[name for name in project_to_branch_map.keys()],
            tracking_branches=list({branch for branch in project_to_branch_map.values()}),
        )


class TaskStatusCounts(NamedTuple):
    project: Optional[str] = None
    version_id: Optional[str] = None
    build_id: Optional[str] = None
    failed: Optional[int] = 0

    def add(self, other: Any) -> TaskStatusCounts:
        """
        Create a new `TaskStatusCounts` object that has a sum of failed and
        completed counts of the current object and of the other object.

        If any values of project, version_id, build_id do not match they will
        be `None` in a new `TaskStatusCounts` object.

        :param other: Other object to add.
        :return: `TaskStatusCounts` object.
        :raises TypeError: If other object is not `TaskStatusCounts`.
        """
        if isinstance(other, self.__class__):
            project = self.project if self.project == other.project else None
            version_id = self.version_id if self.version_id == other.version_id else None
            build_id = self.build_id if self.build_id == other.build_id else None

            return TaskStatusCounts(
                project=project,
                version_id=version_id,
                build_id=build_id,
                failed=self.failed + other.failed,
            )

        raise TypeError


class EvergreenService:
    def __init__(self, evg_api: EvergreenApi) -> None:
        self.evg_api = evg_api

    def get_evg_project_info(self, tracking_repo: str, tracking_branch: str) -> EvgProjectsInfo:
        """
        Accumulate information about active evergreen projects that
        track provided repo and branch.

        :param tracking_repo: Repo name in `{github_org}/{github_repo}` format.
        :param tracking_branch: Branch name.
        :return: Evergreen projects information.
        """
        evg_projects = [
            project
            for project in self.evg_api.all_projects()
            if project.enabled
            and f"{project.owner_name}/{project.repo_name}" == tracking_repo
            and project.branch_name == tracking_branch
        ]

        project_branch_map = {project.identifier: project.branch_name for project in evg_projects}

        return EvgProjectsInfo.from_project_branch_map(project_branch_map)

    def get_waterfall_status(
        self, evg_project_names: List[str], window_start: datetime, window_end: datetime
    ) -> List[TaskStatusCounts]:
        """
        Get task status counts of all builds for a given Evergreen projects.

        :param evg_project_names: Evergreen project names.
        :param window_start: Look for waterfall versions after this date.
        :param window_end: Look for waterfall versions before this date.
        :return: Task status counts of all builds.
        """
        all_build_ids = []

        for evg_project_name in evg_project_names:
            versions = self.evg_api.versions_by_project_time_window(
                project_id=evg_project_name, after=window_start, before=window_end
            )

            for version in versions:
                LOGGER.info(
                    "Getting build ids from version",
                    project=evg_project_name,
                    version_id=version.version_id,
                )
                build_ids = [bvs.build_id for bvs in version.build_variants_status]
                all_build_ids.extend(build_ids)

        return self.get_build_statuses(all_build_ids)

    def get_build_statuses(self, build_ids: List[str]) -> List[TaskStatusCounts]:
        """
        Get task status counts of Evergreen builds for the given build ids.

        :param build_ids: Evergreen build ids.
        :return: Task status counts of the builds.
        """
        LOGGER.info("Getting Evergreen build statuses", num_builds=len(build_ids))

        with ThreadPoolExecutor() as executor:
            futures = [
                executor.submit(
                    self.get_build_status,
                    build_id=build_id,
                )
                for build_id in build_ids
            ]
            build_statuses = [future.result() for future in futures]
        LOGGER.info("Got Evergreen build statuses", num_builds=len(build_statuses))

        return build_statuses

    def get_build_status(self, build_id: str) -> TaskStatusCounts:
        """
        Get task status counts of Evergreen build for the given build id.

        :param build_id: Evergreen build id.
        :return: Task status counts of the build.
        """
        build = self.evg_api.build_by_id(build_id)

        failed_tasks_count = sum(
            count
            for status, count in build.status_counts.json.items()
            if status in TASK_FAILED_STATUSES
        )

        task_status_counts = TaskStatusCounts(
            project=build.project_identifier,
            version_id=build.version,
            build_id=build.id,
            failed=failed_tasks_count,
        )
        LOGGER.info("Got Evergreen build status", task_status_counts=task_status_counts)

        return task_status_counts
