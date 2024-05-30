from __future__ import annotations

from typing import Dict, List, NamedTuple, Set

from evergreen import EvergreenApi


class EvgProjectsInfo(NamedTuple):
    project_branch_map: Dict[str, str]
    branch_project_map: Dict[str, List[str]]
    active_project_names: List[str]
    tracking_branches: List[str]

    @classmethod
    def from_project_branch_map(cls, project_branch_map: Dict[str, str]) -> EvgProjectsInfo:
        """
        Build EvgProjectsInfo object from evergreen project name to its tracking branch map.

        :param project_branch_map: Evergreen project name to its tracking branch map.
        :return: Evergreen projects information.
        """
        branch_project_map = {}
        for project, branch in project_branch_map.items():
            if branch not in branch_project_map:
                branch_project_map[branch] = []
            branch_project_map[branch].append(project)

        return cls(
            project_branch_map=project_branch_map,
            branch_project_map=branch_project_map,
            active_project_names=[name for name in project_branch_map.keys()],
            tracking_branches=list({branch for branch in project_branch_map.values()}),
        )


class EvergreenService:
    def __init__(self, evg_api: EvergreenApi) -> None:
        self.evg_api = evg_api

    def get_evg_project_info(
        self, evg_project_names: Set[str], tracking_repo: str
    ) -> EvgProjectsInfo:
        """
        Accumulate active evergreen projects information that tracks the provided repo
        and their tracking branches.

        :param evg_project_names: List of evergreen project names.
        :param tracking_repo: Repo name in {owner}/{repo} format.
        :return: Evergreen projects information.
        """
        evg_projects = [
            project
            for project in self.evg_api.all_projects()
            if project.enabled
            and f"{project.owner_name}/{project.repo_name}" == tracking_repo
            and project.identifier in evg_project_names
        ]

        project_branch_map = {project.identifier: project.branch_name for project in evg_projects}

        return EvgProjectsInfo.from_project_branch_map(project_branch_map)
