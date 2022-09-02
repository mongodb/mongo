#!/usr/bin/env python3
"""Extensible script to run one or more Python Linters across a subset of files in parallel."""

import argparse
import logging
import os
import sys
from typing import Dict, List

import structlog

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

# pylint: disable=wrong-import-position
from buildscripts.linter.filediff import gather_changed_files_for_lint
from buildscripts.linter import base
from buildscripts.linter import git
from buildscripts.linter import mypy
from buildscripts.linter import parallel
from buildscripts.linter import pydocstyle
from buildscripts.linter import pylint
from buildscripts.linter import runner
from buildscripts.linter import yapf
# pylint: enable=wrong-import-position

# List of supported linters
_LINTERS = [
    yapf.YapfLinter(),
    pylint.PyLintLinter(),
    pydocstyle.PyDocstyleLinter(),
    mypy.MypyLinter(),
]

# List of supported SCons linters
_SCONS_LINTERS: List[base.LinterBase] = [
    yapf.YapfLinter(),
]


def get_py_linter(linter_filter):
    # type: (str) -> List[base.LinterBase]
    """
    Get a list of linters to use.

    'all' or None - select all linters
    'scons' - get all scons linters
    'a,b,c' - a comma delimited list is describes a list of linters to choose
    """
    if linter_filter is None or linter_filter == "all":
        return _LINTERS

    if linter_filter == "scons":
        return _SCONS_LINTERS

    linter_list = linter_filter.split(",")

    linter_candidates = [linter for linter in _LINTERS if linter.cmd_name in linter_list]

    if not linter_candidates:
        raise ValueError("No linters found for filter '%s'" % (linter_filter))

    return linter_candidates


def is_interesting_file(file_name):
    # type: (str) -> bool
    """Return true if this file should be checked."""
    file_denylist = []  # type: List[str]
    directory_denylist = ["src/third_party", "buildscripts/gdb"]
    if file_name in file_denylist or file_name.startswith(tuple(directory_denylist)):
        return False
    directory_list = ["buildscripts", "pytests"]
    return file_name.endswith(".py") and file_name.startswith(tuple(directory_list))


def is_scons_file(file_name):
    # type: (str) -> bool
    """Return true if this file is related to SCons."""
    file_denylist = []  # type: List[str]
    directory_denylist = ["site_scons/third_party"]
    if file_name in file_denylist or file_name.startswith(tuple(directory_denylist)):
        return False
    return (file_name.endswith("SConscript") and file_name.startswith("src")) or \
            (file_name.endswith(".py") and file_name.startswith("site_scons")) or \
            file_name == "SConstruct"


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


def lint_git_diff(linters, config_dict, _):
    # type: (str, Dict[str, str], List[str]) -> None
    """Lint git diff command entry point."""
    file_names = gather_changed_files_for_lint(is_interesting_file)

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


def lint_scons(linters, config_dict, file_names):
    # type: (str, Dict[str, str], List[str]) -> None
    """Lint SCons files command entry point."""
    scons_file_names = git.get_files_to_check(file_names, is_scons_file)

    _lint_files(linters, config_dict, scons_file_names)


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
        run_linter = lambda param1: lint_runner.run(linter.cmd_path + linter.linter.  # pylint: disable=cell-var-from-loop
                                                    get_fix_cmd_args(param1))

        lint_clean = parallel.parallel_process([os.path.abspath(f) for f in file_names], run_linter)

        if not lint_clean:
            print("ERROR: Code Style does not match coding style")
            sys.exit(1)


def fix_func(linters, config_dict, file_names):
    # type: (str, Dict[str, str], List[str]) -> None
    """Fix files command entry point."""
    all_file_names = git.get_files_to_check(file_names, is_interesting_file)

    _fix_files(linters, config_dict, all_file_names)


def fix_scons_func(linters, config_dict, file_names):
    # type: (str, Dict[str, str], List[str]) -> None
    """Fix SCons files command entry point."""
    scons_file_names = git.get_files_to_check(file_names, is_scons_file)

    _fix_files(linters, config_dict, scons_file_names)


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

    parser_lint_patch = sub.add_parser('lint-git-diff',
                                       help='Lint the files since the last git commit')
    parser_lint_patch.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_lint_patch.set_defaults(func=lint_git_diff)

    parser_fix = sub.add_parser('fix', help='Fix files if possible')
    parser_fix.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_fix.set_defaults(func=fix_func)

    parser_lint = sub.add_parser('lint-scons', help='Lint only SCons files')
    parser_lint.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_lint.set_defaults(func=lint_scons, linters="scons")

    parser_fix = sub.add_parser('fix-scons', help='Fix SCons related files if possible')
    parser_fix.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_fix.set_defaults(func=fix_scons_func, linters="scons")

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
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())

    args.func(args.linters, config_dict, args.file_names)


if __name__ == "__main__":
    main()
