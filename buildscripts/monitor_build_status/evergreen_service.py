from __future__ import annotations

from typing import Dict, List, NamedTuple

from evergreen import EvergreenApi


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
