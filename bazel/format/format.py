import argparse
import os
import pathlib
import subprocess

from buildscripts import download_buildifier
from buildscripts.buildifier import fix_all, lint_all


def run_shellscripts_linters(shellscripts_linters: pathlib.Path, check: bool) -> bool:
    try:
        env = os.environ.copy()
        if "CI" in os.environ:
            env["PATH"] = f"/opt/shfmt/v3.2.4/bin{os.pathsep}{env['PATH']}"

        command = [str(shellscripts_linters)]
        if not check:
            command.append("fix")
        print(f"Running command: '{' '.join(command)}'")
        subprocess.run(command, check=True, env=env)
    except subprocess.CalledProcessError:
        print("Found shell script formatting errors. Run 'bazel run //:format' to fix")
        print("*** IF BAZEL IS NOT INSTALLED, RUN THE FOLLOWING: ***\n")
        print("python buildscripts/install_bazel.py")
        return False
    return True


def run_buildifier(check: bool) -> bool:
    binary_path = os.path.join(os.curdir, "buildifier")
    if not os.path.exists(binary_path):
        download_buildifier.download(download_location=os.curdir)
    if check:
        return lint_all(binary_path, generate_report=True)
    else:
        fix_all(binary_path)
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
        command = [str(prettier), "--cache", "."] + list(force_exclude_dirs)
        if check:
            command.append("--check")
        else:
            command.append("--write")
        print(f"Running command: '{' '.join(command)}'")
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

    args = parser.parse_args()
    prettier_path: pathlib.Path = args.prettier.resolve()
    shellscripts_linters_path: pathlib.Path = args.shellscripts_linters.resolve()

    os.chdir(default_dir)

    return (
        0
        if run_shellscripts_linters(shellscripts_linters_path, args.check)
        and run_prettier(prettier_path, args.check)
        and run_buildifier(args.check)
        else 1
    )


if __name__ == "__main__":
    exit(main())
