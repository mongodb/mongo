"""Tools for detecting changes in a commit."""
from typing import Any, Set

from git import Repo, DiffIndex
import structlog
from structlog.stdlib import LoggerFactory

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.get_logger(__name__)


def _paths_for_iter(diff, iter_type):
    return {change.a_path for change in diff.iter_change_type(iter_type)}


def _modified_files_for_diff(diff: DiffIndex, log: Any) -> Set:
    modified_files = _paths_for_iter(diff, 'M')
    log.debug("modified files", files=modified_files)

    added_files = _paths_for_iter(diff, 'A')
    log.debug("added files", files=added_files)

    renamed_files = _paths_for_iter(diff, 'R')
    log.debug("renamed files", files=renamed_files)

    # We don't care about delete files, but log them just in case.
    deleted_files = _paths_for_iter(diff, 'D')
    log.debug("deleted files", files=deleted_files)

    return modified_files.union(added_files).union(renamed_files)


def find_changed_files(repo: Repo) -> Set[str]:
    """
    Find files that were new or added to the repository between commits.

    :param repo: Git repository.

    :return: Set of changed files.
    """
    diff = repo.index.diff(None)
    work_tree_files = _modified_files_for_diff(diff, LOGGER.bind(diff="working tree diff"))

    commit = repo.index
    diff = commit.diff(repo.head.commit)
    index_files = _modified_files_for_diff(diff, LOGGER.bind(diff="index diff"))

    untracked_files = set(repo.untracked_files)
    LOGGER.info("untracked files", files=untracked_files, diff="untracked diff")

    return work_tree_files.union(index_files).union(untracked_files)
