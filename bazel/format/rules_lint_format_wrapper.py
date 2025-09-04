import argparse
import os
import pathlib
import subprocess
from typing import List, Union

from buildscripts.bazel_custom_formatter import validate_bazel_groups, validate_clang_tidy_configs


def _git_distance(args: list) -> int:
    command = ["git", "rev-list", "--count"] + args
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running git command: {' '.join(command)}")
        print(f"stderr: {e.stderr.strip()}")
        print(f"stdout: {e.stdout.strip()}")
        raise
    return int(result.stdout.strip())


def _get_merge_base(args: list) -> str:
    command = ["git", "merge-base"] + args
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return result.stdout.strip()


def _git_diff(args: list) -> str:
    command = ["git", "diff"] + args
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return result.stdout.strip() + os.linesep


def _git_unstaged_files() -> str:
    command = ["git", "ls-files", "--others", "--exclude-standard"]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return result.stdout.strip() + os.linesep


def _get_files_changed_since_fork_point(origin_branch: str = "origin/master") -> List[str]:
    """Query git to get a list of files in the repo from a diff."""
    # There are 3 diffs we run:
    # 1. List of commits between origin/master and HEAD of current branch
    # 2. Cached/Staged files (--cached)
    # 3. Working Tree files git tracks

    fork_point = _get_merge_base(["HEAD", origin_branch])

    diff_files = _git_diff(["--name-only", f"{fork_point}..HEAD"])
    diff_files += _git_diff(["--name-only", "--cached"])
    diff_files += _git_diff(["--name-only"])
    diff_files += _git_unstaged_files()

    file_set = {
        os.path.normpath(os.path.join(os.curdir, line.rstrip()))
        for line in diff_files.splitlines()
        if line
    }

    return list(file_set)


def run_rules_lint(
    rules_lint_format_path: pathlib.Path,
    rules_lint_format_check_path: pathlib.Path,
    check: bool,
    files_to_format: Union[List[str], str] = "all",
) -> bool:
    try:
        if check:
            command = [str(rules_lint_format_check_path)]
            print("Running rules_lint formatter in check mode")
        else:
            command = [str(rules_lint_format_path)]
            print("Running rules_lint formatter")
        if files_to_format != "all":
            command += files_to_format
        repo_path = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        subprocess.run(command, check=True, env=os.environ, cwd=repo_path)
    except subprocess.CalledProcessError:
        return False
    return True


def run_prettier(
    prettier: pathlib.Path, check: bool, files_to_format: Union[List[str], str] = "all"
) -> bool:
    # Explicitly ignore anything in the output directories or any symlinks in the root of the repository
    # to prevent bad symlinks from failing the run, see https://github.com/prettier/prettier/issues/11568 as
    # to why it the paths being present in .prettierignore isn't sufficient
    force_exclude_dirs = {
        "!./build",
        "!./bazel-bin",
        "!./bazel-out",
        "!./bazel-mongo",
        "!./external",
        "!./.compiledb",
    }
    for path in pathlib.Path(".").iterdir():
        if path.is_symlink():
            force_exclude_dirs.add(f"!./{path}")
    try:
        command = [
            str(prettier),
            "--cache",
            "--log-level",
            "warn",
        ]
        if files_to_format == "all":
            command += ["."]
        elif len(files_to_format) == 0:
            print("Skipping prettier due to having no files to format.")
            return True
        else:
            command += files_to_format
        command += list(force_exclude_dirs)
        if check:
            command.append("--check")
        else:
            command.append("--write")
        print("Running prettier")
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError:
        print("Found formatting errors. Run 'bazel run //:format' to fix")
        print("*** IF BAZEL IS NOT INSTALLED, RUN THE FOLLOWING: ***\n")
        print("python buildscripts/install_bazel.py")

        if os.path.exists("external"):
            print(
                "\nexternal exists which may be causing issues in the linter, please try running:\n"
            )
            print("\tunlink external")
        return False

    if check:
        print("No formatting errors")
    return True


def main() -> int:
    # If we are running in bazel, default the directory to the workspace
    default_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not default_dir:
        print("This script must be run though bazel. Please run 'bazel run //:format' instead")
        print("*** IF BAZEL IS NOT INSTALLED, RUN THE FOLLOWING: ***\n")
        print("python buildscripts/install_bazel.py")
        return 1

    parser = argparse.ArgumentParser(
        prog="Format", description="This script formats code in mongodb"
    )

    parser.add_argument("--check", help="Run in check mode", default=False, action="store_true")
    parser.add_argument(
        "--prettier", help="Set the path to prettier", required=True, type=pathlib.Path
    )
    parser.add_argument(
        "--rules-lint-format",
        help="Set the path to rules_lint's formatter",
        required=True,
        type=pathlib.Path,
    )
    parser.add_argument(
        "--rules-lint-format-check",
        help="Set the path to rules_lint's formatter check script",
        required=True,
        type=pathlib.Path,
    )
    parser.add_argument(
        "--all",
        help="Format all files instead of just formatting files that have changed since the fork point",
        action="store_true",
    )
    parser.add_argument(
        "--origin-branch",
        help="The branch to use as the fork point for changed files",
        default="origin/master",
    )

    args = parser.parse_args()
    prettier_path: pathlib.Path = args.prettier.resolve()

    os.chdir(default_dir)

    files_to_format = "all"
    if not args.all:
        max_distance = 100
        distance = _git_distance([f"{args.origin_branch}..HEAD"])
        if distance > max_distance:
            print(
                f"The number of commits between current branch and origin branch ({args.origin_branch}) is too large: {distance} commits (> {max_distance} commits)."
            )
            print("WARNING!!! Defaulting to formatting all files, this may take a while.")
            print(
                "Please update your local branch with the latest changes from origin, or use `bazel run format -- --origin-branch other_branch` to select a different origin branch"
            )
            args.all = True
        else:
            files_to_format = _get_files_changed_since_fork_point(args.origin_branch)

    def files_to_format_contains_bazel_file(files: Union[List[str], str]) -> bool:
        if files == "all":
            return True
        return any(file.endswith(".bazel") or "BUILD" in file for file in files)

    if files_to_format_contains_bazel_file(files_to_format):
        validate_clang_tidy_configs(generate_report=True, fix=not args.check)
        validate_bazel_groups(generate_report=True, fix=not args.check)

    if files_to_format != "all":
        files_to_format = [str(file) for file in files_to_format if os.path.isfile(file)]

    return (
        0
        if run_prettier(prettier_path, args.check, files_to_format) and run_rules_lint(
            args.rules_lint_format, args.rules_lint_format_check, args.check, files_to_format
        )
        else 1
    )


if __name__ == "__main__":
    exit(main())
