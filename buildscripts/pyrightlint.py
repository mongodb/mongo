#!/usr/bin/env python3
"""Script to run pyright linter on Python files."""

import argparse
import logging
import os
import subprocess
import sys
from typing import List

import structlog

# Configure the base directory for the MongoDB source.
RELATIVE_MONGO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__))))
REPO_ROOT = os.environ.get("BUILD_WORKSPACE_DIRECTORY", RELATIVE_MONGO_DIR)

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(REPO_ROOT)

from buildscripts.linter import pyrightlinter, runner
from buildscripts.linter.filediff import gather_changed_files_for_lint


def is_interesting_file(filename: str) -> bool:
    """Return true if this file should be checked."""
    return filename.endswith(".py")


def lint(paths: List[str]):
    """Lint specified paths (files or directories) using Pyright."""
    if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
        subprocess.run(
            [
                "python",
                "-m",
                "pyright",
                "-p",
                "pyproject.toml"
            ] + paths,
            env=os.environ,
            check=True,
            cwd=REPO_ROOT,
        )
        return

    lint_runner = runner.LintRunner()
    pyright = pyrightlinter.PyrightLinter()

    linter_instance = runner.find_linters([pyright], {})

    if not linter_instance:
        print("ERROR: Pyright linter not found or failed to initialize")
        sys.exit(1)

    lint_success = lint_runner.run_lint(linter_instance[0], paths, MONGO_DIR)

    if not lint_success:
        print("ERROR: Code Style does not match coding style")
        sys.exit(1)


def lint_git_diff():
    """Lint files that have changed based on git diff."""
    files = gather_changed_files_for_lint(is_interesting_file)
    if files:
        lint(files)
    else:
        print("No files to lint.")


def lint_all():
    """Lint all Python files in the repository."""
    lint([])


def main():
    """Main entry point for the Pyright linter script."""

    parser = argparse.ArgumentParser(description="Pyright Lint frontend.")

    parser.add_argument("-v", "--verbose", action="store_true", help="Enable verbose logging")

    sub = parser.add_subparsers(title="Linter subcommands", help="sub-command help")

    parser_lint = sub.add_parser(
        "lints",
        help="Lint paths (files or directories) and exit with nonzero status if any violations are found",
    )
    parser_lint.add_argument("paths", nargs="*", help="Paths (files or directories) to check")
    parser_lint.set_defaults(func=lint)

    parser_git_diff = sub.add_parser("git-diff", help="Lint files changed in git")
    parser_git_diff.set_defaults(func=lambda _: lint_git_diff())

    parser_all = sub.add_parser("lint-all", help="Lint all files")
    parser_all.set_defaults(func=lambda _: lint_all())

    # No args given? Fall back to usage screen:
    if len(sys.argv) == 1:
        parser.print_help()
        return

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())

    # Use the nodejs binary from the bazel's npm repository
    # had to add '--build_runfile_links', '--legacy_external_runfiles' due to hard coded path below
    subprocess.run(
        [
            "bazel",
            "build",
            "//:eslint",
            "--config=local",
            "--build_runfile_links",
            "--legacy_external_runfiles",
        ],
        env=os.environ,
        check=True,
        cwd=REPO_ROOT,
    )
    os.environ["PATH"] = (
        "bazel-bin/eslint_/eslint.runfiles/nodejs_linux_arm64/bin/nodejs/bin/" + os.pathsep
    ) + os.environ["PATH"]

    args.func(args.paths if hasattr(args, "paths") else [])


if __name__ == "__main__":
    main()
