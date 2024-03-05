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
from buildscripts.patch_builds.change_data import generate_revision_map, \
    RevisionMap, find_changed_files_in_repos

# pylint: enable=wrong-import-position

LOGGER = structlog.get_logger(__name__)
MONGO_REVISION_ENV_VAR = "REVISION"


def _get_repos_and_revisions() -> Tuple[List[Repo], RevisionMap]:
    """Get the repo object and a map of revisions to compare against."""
    modules = [
        path for path in git.get_module_paths()
        # Exclude enterprise module; it's in the "modules" folder but does not correspond to a repo
        if "src/mongo/db/modules/enterprise" not in path
    ]
    repos = [Repo(path) for path in modules]
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
