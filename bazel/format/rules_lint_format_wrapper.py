import argparse
import os
import pathlib
import subprocess

from buildscripts.unittest_grouper import validate_bazel_groups


def run_rules_lint(
    rules_lint_format_path: pathlib.Path, rules_lint_format_check_path: pathlib.Path, check: bool
) -> bool:
    try:
        if check:
            command = [str(rules_lint_format_check_path)]
            print("Running rules_lint formatter in check mode")
        else:
            command = [str(rules_lint_format_path)]
            print("Running rules_lint formatter")
        repo_path = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        subprocess.run(command, check=True, env=os.environ, cwd=repo_path)
    except subprocess.CalledProcessError:
        return False
    return True


def run_shellscripts_linters(shellscripts_linters: pathlib.Path, check: bool) -> bool:
    try:
        command = [str(shellscripts_linters)]
        if not check:
            print("Running shellscripts formatter")
            command.append("fix")
        else:
            print("Running shellscripts linter")
        repo_path = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        subprocess.run(command, check=True, env=os.environ, cwd=repo_path)
    except subprocess.CalledProcessError:
        return False
    return True


def run_prettier(prettier: pathlib.Path, check: bool) -> bool:
    # Explicitly ignore anything in the output directories or any symlinks in the root of the repository
    # to prevent bad symlinks from failing the run, see https://github.com/prettier/prettier/issues/11568 as
    # to why it the paths being present in .prettierignore isn't sufficient
    force_exclude_dirs = {
        "!./build",
        "!./bazel-bin",
        "!./bazel-out",
        "!./bazel-mongo",
        "!./external",
    }
    for path in pathlib.Path(".").iterdir():
        if path.is_symlink():
            force_exclude_dirs.add(f"!./{path}")
    try:
        command = [
            str(prettier),
            "--cache",
            ".",
            "--log-level",
            "warn",
        ] + list(force_exclude_dirs)
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
        "--shellscripts-linters",
        help="Set the path to shellscripts_linters",
        required=True,
        type=pathlib.Path,
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

    args = parser.parse_args()
    prettier_path: pathlib.Path = args.prettier.resolve()
    shellscripts_linters_path: pathlib.Path = args.shellscripts_linters.resolve()

    os.chdir(default_dir)

    validate_bazel_groups(generate_report=True, fix=not args.check)

    return (
        0
        if run_rules_lint(args.rules_lint_format, args.rules_lint_format_check, args.check)
        and run_shellscripts_linters(shellscripts_linters_path, args.check)
        and run_prettier(prettier_path, args.check)
        else 1
    )


if __name__ == "__main__":
    exit(main())
