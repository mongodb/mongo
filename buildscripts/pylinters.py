#!/usr/bin/env python3
"""Extensible script to run one or more Python Linters across a subset of files in parallel."""

import argparse
import logging
import os
import sys

import structlog

mongo_dir = os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__))))
# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(mongo_dir)

# pylint: disable=wrong-import-position
from buildscripts.linter import ruffchecker, ruffformatter, runner

# pylint: enable=wrong-import-position

# List of supported linters
_LINTERS = [
    ruffchecker.RuffChecker(),
    ruffformatter.RuffFormatter(),
    # TODO: pyright
]


def lint(file_names: list[str]):
    """Lint files command entry point."""

    lint_runner = runner.LintRunner()
    linter_instances = runner.find_linters(_LINTERS, {})

    any_failed = False
    for linter in linter_instances:
        lint_success = lint_runner.run_lint(linter, file_names, mongo_dir)
        any_failed |= not lint_success

    if any_failed:
        print("ERROR: Code Style does not match coding style")
        print("To fix formatting errors, run `buildscripts/pylinters.py fix`.")
        sys.exit(1)


def fix_func(file_names: list[str]):
    """Fix a list of files with linters if possible."""

    lint_runner = runner.LintRunner()
    linter_instances = runner.find_linters(_LINTERS, {})

    any_failed = False
    for linter in linter_instances:
        lint_success = lint_runner.run_fix(linter, file_names, mongo_dir)
        any_failed |= not lint_success

    if any_failed:
        print("ERROR: Code Style does not match coding style")
        print("Code could not be automatically fixed.")
        sys.exit(1)


def main():
    # type: () -> None
    """Execute Main entry point."""

    parser = argparse.ArgumentParser(
        description="PyLinter frontend; see more details at https://wiki.corp.mongodb.com/x/1vP5BQ"
    )

    parser.add_argument("-v", "--verbose", action="store_true", help="Enable verbose logging")

    sub = parser.add_subparsers(title="Linter subcommands", help="sub-command help")

    parser_lint = sub.add_parser(
        "lint", help="Lint files and exit with nonzero status if any violations are found"
    )
    parser_lint.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_lint.set_defaults(func=lint)

    parser_fix = sub.add_parser("fix", help="Fix files if possible")
    parser_fix.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_fix.set_defaults(func=fix_func)

    # No args given? Fall back to usage screen:
    if len(sys.argv) == 1:
        parser.print_help()
        return
    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())

    args.func(args.file_names)


if __name__ == "__main__":
    main()
