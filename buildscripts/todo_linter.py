#!/usr/bin/env python3
"""Linter that fails if any TODO comments referencing SERVER tickets are found.

Matches patterns like:
  // TODO(SERVER-XXXXX): fix this
  # TODO(SERVER-XXXXX): fix this
  // TODO SERVER-XXXXX - fix this
  // TODO: fix this
"""

import argparse
import logging
import os
import re
import sys

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts.linter import git, parallel

_RE_TODO_SERVER = re.compile(r"(?<!\(Ignore linting\) )(TODO\W.*SERVER-X{5,}|TODO:\s+(?!SERVER))")

FILES_RE = re.compile(r"\.(cpp|c|h|hpp|py|js|mjs|inl|idl|yml|bazel)$")


def is_interesting_file(file_name: str) -> bool:
    """Return true if this file should be checked."""
    return (
        not file_name.startswith("src/third_party/")
        and not file_name.startswith("src/mongo/gotools/")
        and not file_name.startswith("src/streams/third_party")
        and not file_name.startswith("src/mongo/db/modules/enterprise/src/streams/third_party")
        # Exclude this file because it's filled with TODO SERVER-XXXXX patterns
        and not file_name == "buildscripts/todo_linter.py"
        and FILES_RE.search(file_name) is not None
    )


def check_file(file_name: str) -> bool:
    """Check a single file for TODO SERVER-XXXXX patterns. Returns True if no violations."""
    try:
        with open(file_name, encoding="utf-8") as f:
            lines = f.readlines()
    except (OSError, UnicodeDecodeError):
        return True

    errors = []
    for lineno, line in enumerate(lines, 1):
        if _RE_TODO_SERVER.search(line):
            errors.append((lineno, line.rstrip()))

    for lineno, line in errors:
        print(
            f"Error: {file_name}:{lineno} - todo/server_ticket"
            f" - Found TODO with unlinked SERVER ticket reference: {line.strip()}"
        )

    return len(errors) == 0


def _lint_files(file_names: list[str]) -> None:
    if not parallel.parallel_process([os.path.abspath(f) for f in file_names], check_file):
        print(
            "ERROR: Found TODO comments referencing unlinked SERVER tickets."
            " Please resolve or remove them before committing."
        )
        sys.exit(1)


def lint(args) -> None:
    """Lint only Git-tracked files."""
    file_names = args.file_names
    all_file_names = git.get_files_to_check(file_names, is_interesting_file)
    _lint_files(all_file_names)


def lint_all(args) -> None:
    """Lint all files in the working tree."""
    all_file_names = git.get_files_to_check_working_tree(is_interesting_file)
    _lint_files(all_file_names)


def lint_patch(args) -> None:
    """Lint all files that are divergent from the most recent fork point off of the main branch"""
    origin_branch = "origin/master"
    files = git.get_my_files_to_check(is_interesting_file, origin_branch)
    files = [f for f in files if os.path.exists(f)]

    _lint_files(files)


def main() -> None:
    """Execute main entry point."""
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    parser = argparse.ArgumentParser(
        description="MongoDB TODO SERVER ticket linter. Fails if any unlinked TODO comments are found."
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Enable verbose logging")

    sub = parser.add_subparsers(title="Linter subcommands", help="sub-command help")

    parser_lint = sub.add_parser("lint", help="Lint only Git-tracked files")
    parser_lint.add_argument("file_names", nargs="*", help="Globs of files to check")
    parser_lint.set_defaults(func=lint)

    parser_lint_all = sub.add_parser("lint-all", help="Lint all files in the working tree")
    parser_lint_all.set_defaults(func=lint_all)

    parser_lint_patch = sub.add_parser(
        "lint-patch",
        help="Lint files that are different from the most recent fork point from master",
    )
    parser_lint_patch.set_defaults(func=lint_patch)

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    if not hasattr(args, "func"):
        parser.print_help()
        sys.exit(1)

    args.func(args)


if __name__ == "__main__":
    main()
