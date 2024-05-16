import argparse
import os
import pathlib
import subprocess


def run_prettier(prettier: pathlib.Path, check: bool) -> int:
    try:
        command = [prettier, "."]
        if check:
            command.append("--check")
        else:
            command.append("--write")
        print(f"Running command: '{command}'")
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError:
        print("Found formatting errors. Run 'bazel run //:format' to fix")
        print("*** IF BAZEL IS NOT INSTALLED, RUN THE FOLLOWING: ***\n")
        print("python buildscripts/install_bazel.py")
        return 1

    if check:
        print("No formatting errors")
    return 0


def main() -> int:
    # If we are running in bazel, default the directory to the workspace
    default_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not default_dir:
        print("This script must be run though bazel. Please run 'bazel run //:format' instead")
        print("*** IF BAZEL IS NOT INSTALLED, RUN THE FOLLOWING: ***\n")
        print("python buildscripts/install_bazel.py")
        return 1

    parser = argparse.ArgumentParser(prog='Format',
                                     description='This script formats code in mongodb')

    parser.add_argument("--check", help="Run in check mode", default=False, action="store_true")
    parser.add_argument("--prettier", help="Set the path to prettier", required=True,
                        type=pathlib.Path)

    args = parser.parse_args()
    prettier_path: pathlib.Path = args.prettier.resolve()

    os.chdir(default_dir)
    return run_prettier(prettier_path, args.check)


if __name__ == "__main__":
    exit(main())
