#!/usr/bin/env python3
"""Linter that fails if any TODO comments not referencing SERVER tickets are found.

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
from evergreen import RetryingEvergreenApi

EXCLUSION_VALUE = "(Ignore linting)"
TODO_REGEX = re.compile(r'[^"]TODO[^"]')
JIRA_TICKET_REGEX = re.compile(r"\w+-\d+")
FILES_RE = re.compile(r"\.(cpp|c|h|hpp|py|js|mjs|inl|idl|yml|bazel)$")
EVG_CONFIG_FILE = "./.evergreen.yml"
AUTO_REVERT_APP_BOT_MARKER = "auto-revert-app[bot]"
GITHUB_PULL_REQUEST_IDENTIFIERS = {"github_pr", "github_pull_request"}


def is_interesting_file(file_name: str) -> bool:
    """Return true if this file should be checked."""
    return (
        not file_name.startswith("src/third_party/")
        and not file_name.startswith("src/mongo/gotools/")
        and not file_name.startswith("src/streams/third_party")
        and not file_name.startswith("src/mongo/db/modules/enterprise/src/streams/third_party")
        # Exclude these two files because they are filled with TODO SERVER-XXXXX patterns
        and not file_name == "buildscripts/todo_linter.py"
        and not file_name == "buildscripts/todo_check.py"
        and FILES_RE.search(file_name) is not None
    )


def check_file(file_name: str) -> bool:
    """Check a single file for TODO comments without any SERVER ticket attached. Returns True if no violations."""
    try:
        with open(file_name, encoding="utf-8") as f:
            lines = f.readlines()
    except (OSError, UnicodeDecodeError):
        return True

    errors: list[tuple[int, str]] = []
    for lineno, line in enumerate(lines, 1):
        if EXCLUSION_VALUE in line:
            continue
        if TODO_REGEX.search(line) and not JIRA_TICKET_REGEX.search(line):
            # The regex found a TODO without any SERVER ticket present next to it
            errors.append((lineno, line.rstrip()))

    for lineno, line in errors:
        print(
            f"Error: {file_name}:{lineno} - todo/server_ticket"
            f" - Found TODO with unlinked SERVER ticket reference, make sure to add a valid ticket"
            f' to track its cleanup or add "(Ignore linting)" to the line to silence the linter: {line.strip()}'
        )

    return len(errors) == 0


def get_patch_description(version_id: str) -> str:
    """Return the Evergreen patch description for the given version."""
    evg_api = RetryingEvergreenApi.get_api(config_file=EVG_CONFIG_FILE)
    version = evg_api.version_by_id(version_id)
    return version.message or ""


def should_ignore_todo_lint_failure() -> bool:
    """Return whether TODO lint failures should be ignored for this run."""
    requester = os.environ.get("requester") or os.environ.get("REQUESTER")
    evergreen_user = os.environ.get("author") or os.environ.get("AUTHOR")
    if not any(
        identifier in GITHUB_PULL_REQUEST_IDENTIFIERS
        for identifier in (requester, evergreen_user)
        if identifier
    ):
        return False

    version_id = os.environ.get("version_id") or os.environ.get("VERSION_ID")
    if not version_id:
        return False

    try:
        patch_description = get_patch_description(version_id)
    except Exception as exc:  # pylint: disable=broad-except
        logging.warning("Unable to determine patch description for TODO lint skip: %s", exc)
        return False

    # The auto-revert marker is attached to Evergreen's patch description, so use the version
    # message rather than trying to infer it from GitHub PR metadata.
    return AUTO_REVERT_APP_BOT_MARKER in patch_description.lower()


def _lint_files(file_names: list[str]) -> None:
    if not parallel.parallel_process([os.path.abspath(f) for f in file_names], check_file):
        if should_ignore_todo_lint_failure():
            print(
                "Skipping TODO lint failure because this Evergreen GitHub pull request was "
                "created by auto-revert-app[bot]."
            )
            return
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
    repo = git.Repo(git.get_base_dir())
    if repo.does_branch_exist(args.origin_branch):
        origin_branch = args.origin_branch
    else:
        # We're running this against a stacked PR potentially. Make sure we only test against the parent branch.
        origin_branch = repo.get_branch_name()
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
    parser_lint_patch.add_argument(
        "--branch", dest="origin_branch", default="origin/master", help="Branch to compare against"
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
