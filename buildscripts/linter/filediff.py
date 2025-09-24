"""Modules for find which files should be linted."""

import os
import sys
from typing import Callable, Dict, List, Tuple

import structlog
from git import Repo

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts.linter import git
from buildscripts.patch_builds.change_data import (
    RevisionMap,
    find_changed_files_in_repos,
    find_modified_lines_for_files_in_repos,
    generate_revision_map,
)

LOGGER = structlog.get_logger(__name__)
MONGO_REVISION_ENV_VAR = "REVISION"


def _get_repos_and_revisions() -> Tuple[List[Repo], RevisionMap]:
    """Get the repo object and a map of revisions to compare against."""

    repos = [Repo(git.get_base_dir())]
    revision_map = generate_revision_map(repos, {"mongo": os.environ.get(MONGO_REVISION_ENV_VAR)})
    return repos, revision_map


def _filter_file(filename: str, is_interesting_file: Callable[[str], bool]) -> bool:
    """
    Determine if file should be included based on existence and passed in method.

    :param filename: Filename to check.
    :param is_interesting_file: Function to determine if file is interesting.
    :return: True if file exists and is interesting.
    """
    return os.path.exists(filename) and is_interesting_file(filename)


def gather_changed_files_for_lint(is_interesting_file: Callable[[str], bool]) -> List[str]:
    """
    Get the files that have changes since the last git commit.

    :param is_interesting_file: Filter for whether a file should be returned.
    :return: List of files for linting.
    """
    repos, revision_map = _get_repos_and_revisions()
    LOGGER.info("revisions", revision=revision_map)

    candidate_files = find_changed_files_in_repos(repos, revision_map)
    files = [
        filename for filename in candidate_files if _filter_file(filename, is_interesting_file)
    ]

    LOGGER.info("Found files to lint", files=files)
    return files


def gather_changed_files_with_lines(
    is_interesting_file: Callable[[str], bool],
) -> Dict[str, List[Tuple[int, str]]]:
    """
    Get the files that have changes since the last git commit, along with details of the specific lines that have changed.

    :param is_interesting_file: Filter for whether a file should be returned.
    :return: Dictionary mapping each changed file to a list of tuples, where each tuple contains the modified line number and its content.
    """
    repos, revision_map = _get_repos_and_revisions()
    changed_files = find_changed_files_in_repos(repos, revision_map)

    filtered_files = [f for f in changed_files if is_interesting_file(f)]
    return find_modified_lines_for_files_in_repos(repos, filtered_files, revision_map)
