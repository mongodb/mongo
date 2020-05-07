"""Modules for find which files should be linted."""
import os
import sys
from typing import Tuple, List, Dict, Callable

from git import Repo
import structlog

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

# pylint: disable=wrong-import-position
from buildscripts.linter import git
from buildscripts.patch_builds.change_data import find_changed_files_in_repos
# pylint: enable=wrong-import-position

LOGGER = structlog.get_logger(__name__)


def _get_revision_for_repo(path: str) -> str:
    """
    Get the git revision for the given git repository.

    :param path: Path to git repository.
    :return: Git revision to compare against for given repo.
    """
    if "enterprise" in path:
        return os.environ.get("ENTERPRISE_REV")
    return os.environ.get("REVISION")


def _get_repos_and_revisions() -> Tuple[List[Repo], Dict[str, str]]:
    """Get the repo object and a map of revisions to compare against."""
    modules = git.get_module_paths()
    repos = [Repo(path) for path in modules]
    revision_map = {repo.git_dir: _get_revision_for_repo(repo.git_dir) for repo in repos}
    return repos, revision_map


def _filter_file(filename: str, is_interesting_file: Callable) -> bool:
    """
    Determine if file should be included based on existence and passed in method.

    :param filename: Filename to check.
    :param is_interesting_file: Function to determine if file is interesting.
    :return: True if file exists and is interesting.
    """
    return os.path.exists(filename) and is_interesting_file(filename)


def gather_changed_files_for_lint(is_interesting_file: Callable):
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
