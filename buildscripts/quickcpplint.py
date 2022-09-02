#!/usr/bin/env python3
"""Extensible script to run one or more simple C++ Linters across a subset of files in parallel."""

import argparse
import logging
import os
import re
import sys
import threading
from typing import List

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts.linter import git  # pylint: disable=wrong-import-position
from buildscripts.linter import parallel  # pylint: disable=wrong-import-position
from buildscripts.linter import simplecpplint  # pylint: disable=wrong-import-position

FILES_RE = re.compile('\\.(h|cpp)$')


def is_interesting_file(file_name: str) -> bool:
    """Return true if this file should be checked."""
    return (file_name.startswith("jstests")
            or file_name.startswith("src") and not file_name.startswith("src/third_party/")
            and not file_name.startswith("src/mongo/gotools/")
            # TODO SERVER-49805: These files should be generated at compile time.
            and not file_name == "src/mongo/db/cst/parser_gen.cpp") and FILES_RE.search(file_name)


def _lint_files(file_names: List[str]) -> None:
    """Lint a list of files with clang-format."""
    run_lint1 = lambda param1: simplecpplint.lint_file(param1) == 0
    if not parallel.parallel_process([os.path.abspath(f) for f in file_names], run_lint1):
        print("ERROR: Code Style does not match coding style")
        sys.exit(1)


def lint_patch(file_name: str) -> None:
    """Lint patch command entry point."""
    file_names = git.get_files_to_check_from_patch(file_name, is_interesting_file)

    # Patch may have files that we do not want to check which is fine
    if file_names:
        _lint_files(file_names)


def lint(file_names: List[str]) -> None:
    # type: (str, Dict[str, str], List[str]) -> None
    """Lint files command entry point."""
    all_file_names = git.get_files_to_check(file_names, is_interesting_file)

    _lint_files(all_file_names)


def lint_all(file_names: List[str]) -> None:
    # pylint: disable=unused-argument
    """Lint files command entry point based on working tree."""
    all_file_names = git.get_files_to_check_working_tree(is_interesting_file)

    _lint_files(all_file_names)


def lint_my(origin_branch: List[str]) -> None:
    """Lint files command based on local changes."""
    files = git.get_my_files_to_check(is_interesting_file, origin_branch)
    files = [f for f in files if os.path.exists(f)]

    _lint_files(files)


def main() -> None:
    """Execute Main entry point."""

    parser = argparse.ArgumentParser(description='Quick C++ Lint frontend.')

    parser.add_argument('-v', "--verbose", action='store_true', help="Enable verbose logging")

    sub = parser.add_subparsers(title="Linter subcommands", help="sub-command help")

    parser_lint = sub.add_parser('lint', help='Lint only Git files')
    parser_lint.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_lint.set_defaults(func=lint)

    parser_lint_all = sub.add_parser('lint-all', help='Lint All files')
    parser_lint_all.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_lint_all.set_defaults(func=lint_all)

    parser_lint_patch = sub.add_parser('lint-patch', help='Lint the files in a patch')
    parser_lint_patch.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_lint_patch.set_defaults(func=lint_patch)

    parser_lint_my = sub.add_parser('lint-my', help='Lint my files')
    parser_lint_my.add_argument("--branch", dest="file_names", default="origin/master",
                                help="Branch to compare against")
    parser_lint_my.set_defaults(func=lint_my)

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    args.func(args.file_names)


if __name__ == "__main__":
    main()
