"""Tools for detecting changes in a commit."""

import os
from itertools import chain
from typing import Any, Dict, Iterable, List, Optional, Set

import structlog
from git import DiffIndex, Repo

LOGGER = structlog.get_logger(__name__)

RevisionMap = Dict[str, str]


def _get_id_from_repo(repo: Repo) -> str:
    """
    Get the identifier of the given repo.

    :param repo: Repository to get id for.
    :return: Identifier for repository.
    """
    if repo.working_dir == os.getcwd():
        return "mongo"
    return os.path.basename(repo.working_dir)


def generate_revision_map(repos: List[Repo], revisions_data: Dict[str, str]) -> RevisionMap:
    """
    Generate a revision map for the given repositories using the revisions in the given file.

    :param repos: Repositories to generate map for.
    :param revisions_data: Dictionary of revisions to use for repositories.
    :return: Map of repositories to revisions
    """
    revision_map = {repo.git_dir: revisions_data.get(_get_id_from_repo(repo)) for repo in repos}
    return {k: v for k, v in revision_map.items() if v}


def _paths_for_iter(diff, iter_type):
    """
    Get the set for all the files in the given diff for the specified type.

    :param diff: git diff to query.
    :param iter_type: Iter type ['M', 'A', 'R', 'D'].
    :return: set of changed files.
    """
    a_path_changes = {change.a_path for change in diff.iter_change_type(iter_type)}
    b_path_changes = {change.b_path for change in diff.iter_change_type(iter_type)}
    return a_path_changes.union(b_path_changes)


def _modified_files_for_diff(diff: DiffIndex, log: Any) -> Set:
    """
    Get the set of files modified in the given git diff.

    :param diff: Git diff information.
    :param log: Logger for logging.
    :return: Set of files that were modified in diff.
    """
    modified_files = _paths_for_iter(diff, "M")
    log.debug("modified files", files=modified_files)

    added_files = _paths_for_iter(diff, "A")
    log.debug("added files", files=added_files)

    renamed_files = _paths_for_iter(diff, "R")
    log.debug("renamed files", files=renamed_files)

    deleted_files = _paths_for_iter(diff, "D")
    log.debug("deleted files", files=deleted_files)

    return modified_files.union(added_files).union(renamed_files).union(deleted_files)


def find_changed_files(repo: Repo, revision_map: Optional[RevisionMap] = None) -> Set[str]:
    """
    Find files that were new or added to the repository between commits.

    :param repo: Git repository.
    :param revision_map: Map of revisions to compare against for repos.

    :return: Set of changed files.
    """
    LOGGER.info("Getting diff for repo", repo=repo.git_dir)
    if not revision_map:
        revision_map = {}
    diff = repo.index.diff(None)
    work_tree_files = _modified_files_for_diff(diff, LOGGER.bind(diff="working tree diff"))

    commit = repo.index
    diff = commit.diff(revision_map.get(repo.git_dir, repo.head.commit), R=True)
    index_files = _modified_files_for_diff(diff, LOGGER.bind(diff="index diff"))

    untracked_files = set(repo.untracked_files)
    LOGGER.info("untracked files", files=untracked_files, diff="untracked diff")

    paths = work_tree_files.union(index_files).union(untracked_files)

    return {
        os.path.relpath(f"{repo.working_dir}/{os.path.normpath(path)}", os.getcwd())
        for path in paths
    }


def find_changed_files_in_repos(
    repos: Iterable[Repo], revision_map: Optional[RevisionMap] = None
) -> Set[str]:
    """
    Find the changed files.

    Use git to find which files have changed in this patch.

    :param repos: List of repos containing changed files.
    :param revision_map: Map of revisions to compare against for repos.
    :return: Set of changed files.
    """
    return set(chain.from_iterable([find_changed_files(repo, revision_map) for repo in repos]))
