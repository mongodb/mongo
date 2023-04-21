#!/usr/bin/env python3
"""Check files in git diff to ensure they are within a given size limit."""

# pylint: disable=wrong-import-position

import argparse
import fnmatch
import logging
import os
import pathlib
import sys
import textwrap

from typing import Any, Callable, Dict, List, Optional, Tuple

import structlog

from git import Repo

mongo_dir = os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__))))
# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(mongo_dir)

from buildscripts.linter import git
from buildscripts.patch_builds.change_data import (RevisionMap, find_changed_files_in_repos,
                                                   generate_revision_map)


# Console renderer for structured logging
def renderer(_logger: logging.Logger, _name: str, eventdict: Dict[Any, Any]) -> str:
    if 'files' in eventdict:
        return "{event}: {files}".format(**eventdict)
    if 'repo' in eventdict:
        return "{event}: {repo}".format(**eventdict)
    if 'file' in eventdict:
        if 'bytes' in eventdict:
            return "{event}: {file} {bytes} bytes".format(**eventdict)
        return "{event}: {file}".format(**eventdict)
    return "{event}".format(**eventdict)


# Configure the logger so it doesn't spam output on huge diffs
structlog.configure(
    logger_factory=structlog.stdlib.LoggerFactory(),
    wrapper_class=structlog.stdlib.BoundLogger,
    cache_logger_on_first_use=True,
    processors=[
        structlog.stdlib.filter_by_level,
        renderer,
    ],
)

LOGGER = structlog.get_logger(__name__)
MONGO_REVISION_ENV_VAR = "REVISION"
ENTERPRISE_REVISION_ENV_VAR = "ENTERPRISE_REV"


def _get_repos_and_revisions() -> Tuple[List[Repo], RevisionMap]:
    """Get the repo object and a map of revisions to compare against."""
    modules = git.get_module_paths()
    repos = [Repo(path) for path in modules]
    revision_map = generate_revision_map(
        repos, {
            "mongo": os.environ.get(MONGO_REVISION_ENV_VAR),
            "enterprise": os.environ.get(ENTERPRISE_REVISION_ENV_VAR)
        })
    return repos, revision_map


def git_changed_files(excludes: List[pathlib.Path]) -> List[pathlib.Path]:
    """
    Get the files that have changes since the last git commit.

    :param excludes: A list of files which should be excluded from changed file checks.
    :return: List of changed files.
    """
    repos, revision_map = _get_repos_and_revisions()
    LOGGER.debug("revisions", revision=revision_map)

    def _filter_fn(file_path: pathlib.Path) -> bool:
        if not file_path.exists():
            return False
        for exclude in excludes:
            if fnmatch.fnmatch(file_path, exclude):
                return False
        return True

    files = [
        filename
        for filename in list(map(pathlib.Path, find_changed_files_in_repos(repos, revision_map)))
        if _filter_fn(filename)
    ]

    LOGGER.debug("Found files to check", files=list(map(str, files)))
    return files


def diff_file_sizes(size_limit: int, excludes: Optional[List[str]] = None) -> List[pathlib.Path]:
    if excludes is None:
        excludes = []

    large_files: list[pathlib.Path] = []

    for file_path in git_changed_files(excludes):
        LOGGER.debug("Checking file size", file=str(file_path))
        file_size = file_path.stat().st_size
        if file_size > size_limit:
            LOGGER.error("File too large", file=str(file_path), bytes=file_size)
            large_files.append(file_path)

    return large_files


def main(*args: str) -> int:
    """Execute Main entry point."""

    parser = argparse.ArgumentParser(
        description='Git commit large file checker.', epilog=textwrap.dedent('''\
        NOTE: The --exclude argument is an exact match but can accept glob patterns. If * is used,
        it matches *all* characters, including path separators.
    '''))
    parser.add_argument("--verbose", action="store_true", help="Enable verbose logging")
    parser.add_argument("--exclude", help="Paths to exclude from check", nargs="+",
                        type=pathlib.Path, required=False)
    parser.add_argument("--size-mb", help="File size limit (MiB)", type=int, default="10")
    parsed_args = parser.parse_args(args[1:])

    if parsed_args.verbose:
        logging.basicConfig(level=logging.DEBUG)
        structlog.stdlib.filter_by_level(LOGGER, 'debug', {})
    else:
        logging.basicConfig(level=logging.INFO)
        structlog.stdlib.filter_by_level(LOGGER, 'info', {})

    large_files = diff_file_sizes(parsed_args.size_mb * 1024 * 1024, parsed_args.exclude)
    if len(large_files) == 0:
        LOGGER.info("All files passed size check")
        return 0

    LOGGER.error("Some files failed size check", files=list(map(str, large_files)))
    return 1


if __name__ == '__main__':
    sys.exit(main(*sys.argv))
