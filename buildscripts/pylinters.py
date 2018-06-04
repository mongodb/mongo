#!/usr/bin/env python2
"""Extensible script to run one or more Python Linters across a subset of files in parallel."""
from __future__ import absolute_import
from __future__ import print_function

import argparse
import logging
import os
import sys
import threading
from abc import ABCMeta, abstractmethod
from typing import Any, Dict, List

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts.linter import base  # pylint: disable=wrong-import-position
from buildscripts.linter import git  # pylint: disable=wrong-import-position
from buildscripts.linter import mypy  # pylint: disable=wrong-import-position
from buildscripts.linter import parallel  # pylint: disable=wrong-import-position
from buildscripts.linter import pydocstyle  # pylint: disable=wrong-import-position
from buildscripts.linter import pylint  # pylint: disable=wrong-import-position
from buildscripts.linter import runner  # pylint: disable=wrong-import-position
from buildscripts.linter import yapf  # pylint: disable=wrong-import-position

# List of supported linters
_LINTERS = [
    yapf.YapfLinter(),
    pylint.PyLintLinter(),
    pydocstyle.PyDocstyleLinter(),
    mypy.MypyLinter(),
]


def get_py_linter(linter_filter):
    # type: (str) -> List[base.LinterBase]
    """
    Get a list of linters to use.

    'all' or None - select all linters
    'a,b,c' - a comma delimited list is describes a list of linters to choose
    """
    if linter_filter is None or linter_filter == "all":
        return _LINTERS

    linter_list = linter_filter.split(",")

    linter_candidates = [linter for linter in _LINTERS if linter.cmd_name in linter_list]

    if not linter_candidates:
        raise ValueError("No linters found for filter '%s'" % (linter_filter))

    return linter_candidates


def is_interesting_file(file_name):
    # type: (str) -> bool
    """Return true if this file should be checked."""
    file_blacklist = ["buildscripts/cpplint.py"]
    directory_blacklist = ["src/third_party"]
    if file_name in file_blacklist or file_name.startswith(tuple(directory_blacklist)):
        return False
    directory_list = ["buildscripts", "pytests"]
    return file_name.endswith(".py") and file_name.startswith(tuple(directory_list))


def _lint_files(linters, config_dict, file_names):
    # type: (str, Dict[str, str], List[str]) -> None
    """Lint a list of files with clang-format."""
    linter_list = get_py_linter(linters)

    lint_runner = runner.LintRunner()

    linter_instances = runner.find_linters(linter_list, config_dict)
    if not linter_instances:
        sys.exit(1)

    failed_lint = False

    for linter in linter_instances:
        run_fix = lambda param1: lint_runner.run_lint(linter, param1)  # pylint: disable=cell-var-from-loop
        lint_clean = parallel.parallel_process([os.path.abspath(f) for f in file_names], run_fix)

        if not lint_clean:
            failed_lint = True

    if failed_lint:
        print("ERROR: Code Style does not match coding style")
        sys.exit(1)


def lint_patch(linters, config_dict, file_name):
    # type: (str, Dict[str, str], List[str]) -> None
    """Lint patch command entry point."""
    file_names = git.get_files_to_check_from_patch(file_name, is_interesting_file)

    # Patch may have files that we do not want to check which is fine
    if file_names:
        _lint_files(linters, config_dict, file_names)


def lint(linters, config_dict, file_names):
    # type: (str, Dict[str, str], List[str]) -> None
    """Lint files command entry point."""
    all_file_names = git.get_files_to_check(file_names, is_interesting_file)

    _lint_files(linters, config_dict, all_file_names)


def lint_all(linters, config_dict, file_names):
    # type: (str, Dict[str, str], List[str]) -> None
    # pylint: disable=unused-argument
    """Lint files command entry point based on working tree."""
    all_file_names = git.get_files_to_check_working_tree(is_interesting_file)

    _lint_files(linters, config_dict, all_file_names)


def _fix_files(linters, config_dict, file_names):
    # type: (str, Dict[str, str], List[str]) -> None
    """Fix a list of files with linters if possible."""
    linter_list = get_py_linter(linters)

    # Get a list of linters which return a valid command for get_fix_cmd()
    fix_list = [fixer for fixer in linter_list if fixer.get_fix_cmd_args("ignore")]

    if not fix_list:
        raise ValueError("Cannot find any linters '%s' that support fixing." % (linters))

    lint_runner = runner.LintRunner()

    linter_instances = runner.find_linters(fix_list, config_dict)
    if not linter_instances:
        sys.exit(1)

    for linter in linter_instances:
        run_linter = lambda param1: lint_runner.run(linter.cmd_path + linter.linter.get_fix_cmd_args(param1))  # pylint: disable=cell-var-from-loop

        lint_clean = parallel.parallel_process([os.path.abspath(f) for f in file_names], run_linter)

        if not lint_clean:
            print("ERROR: Code Style does not match coding style")
            sys.exit(1)


def fix_func(linters, config_dict, file_names):
    # type: (str, Dict[str, str], List[str]) -> None
    """Fix files command entry point."""
    all_file_names = git.get_files_to_check(file_names, is_interesting_file)

    _fix_files(linters, config_dict, all_file_names)


def main():
    # type: () -> None
    """Execute Main entry point."""

    parser = argparse.ArgumentParser(description='PyLinter frontend.')

    linters = get_py_linter(None)

    dest_prefix = "linter_"
    for linter1 in linters:
        msg = 'Path to linter %s' % (linter1.cmd_name)
        parser.add_argument('--' + linter1.cmd_name, type=str, help=msg,
                            dest=dest_prefix + linter1.cmd_name)

    parser.add_argument('--linters', type=str,
                        help="Comma separated list of filters to use, defaults to 'all'",
                        default="all")

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

    parser_fix = sub.add_parser('fix', help='Fix files if possible')
    parser_fix.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_fix.set_defaults(func=fix_func)

    args = parser.parse_args()

    # Create a dictionary of linter locations if the user needs to override the location of a
    # linter. This is common for mypy on Windows for instance.
    config_dict = {}
    for key in args.__dict__:
        if key.startswith("linter_"):
            name = key.replace(dest_prefix, "")
            config_dict[name] = args.__dict__[key]

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    args.func(args.linters, config_dict, args.file_names)


if __name__ == "__main__":
    main()
