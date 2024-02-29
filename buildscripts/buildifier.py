import argparse
import json
import os
import pathlib
import platform
import subprocess

from simple_report import put_report, try_combine_reports, make_report

mongo_dir = pathlib.Path(__file__).parents[1]


def find_all_failed(bin_path: str) -> list[str]:
    # TODO(SERVER-81039): Remove once third_party libs can be compiled from the root directory.
    ignored_paths = []
    with open(os.path.join(mongo_dir, ".bazelignore"), "r") as file:
        for line in file.readlines():
            contents = line.split("#")[0].strip()
            if contents:
                ignored_paths.append(contents)

    process = subprocess.run([bin_path, "--format=json", "--mode=check", "-r", "./"], check=True,
                             capture_output=True)
    buildifier_results = json.loads(process.stdout)
    if buildifier_results["success"]:
        return []

    return [
        result["filename"] for result in buildifier_results["files"]
        if (not result["formatted"] and \
            not any(result["filename"].startswith(ignored_path) for ignored_path in ignored_paths))
    ]


def lint_all(bin_path: str, generate_report: bool):
    files = find_all_failed(bin_path)
    lint(bin_path, files, generate_report)


def fix_all(bin_path: str):
    files = find_all_failed(bin_path)
    fix(bin_path, files)


def lint(bin_path: str, files: list[str], generate_report: bool):
    for file in files:
        process = subprocess.run([bin_path, "--format=json", "--mode=check", file], check=True,
                                 capture_output=True)
        result = json.loads(process.stdout)
        if result["success"]:
            continue
        # This purposefully gives a exit code of 4 when there is a diff
        process = subprocess.run([bin_path, "--mode=diff", file], capture_output=True,
                                 encoding='utf-8')
        if process.returncode not in (0, 4):
            raise RuntimeError()
        diff = process.stdout
        print(f"{file} has linting errors")
        print(diff)

        if generate_report:
            header = (
                "There are linting errors in this file, fix them with one of the following commands:\n"
                "python3 buildscripts/buildifier.py fix-all\n"
                f"python3 buildscripts/buildifier.py fix {file}\n\n")
            report = make_report(f"{file} warnings", json.dumps(result, indent=2), 1)
            try_combine_reports(report)
            put_report(report)
            report = make_report(f"{file} diff", header + diff, 1)
            try_combine_reports(report)
            put_report(report)
    print("Done linting files")


def fix(bin_path: str, files: list[str]):
    for file in files:
        subprocess.run([bin_path, "--mode=fix", file], check=True)
    print("Done fixing files")


def main():
    parser = argparse.ArgumentParser(description='buildifier wrapper')
    parser.add_argument(
        "--binary-dir", "-b", type=str,
        help="Path to the buildifier binary, defaults to looking in the current directory.",
        default="")
    parser.add_argument(
        "--generate-report", action="store_true",
        help="Whether or not a report of the lint errors should be generated for evergreen.",
        default=False)
    parser.set_defaults(subcommand=None)

    sub = parser.add_subparsers(title="buildifier subcommands", help="sub-command help")

    lint_all_parser = sub.add_parser("lint-all", help="Lint all files")
    lint_all_parser.set_defaults(subcommand="lint-all")

    fix_all_parser = sub.add_parser("fix-all", help="Fix all files")
    fix_all_parser.set_defaults(subcommand="fix-all")

    lint_parser = sub.add_parser("lint", help="Lint specified list of files")
    lint_parser.add_argument("files", nargs="+")
    lint_parser.set_defaults(subcommand="lint")

    lint_parser = sub.add_parser("fix", help="Fix specified list of files")
    lint_parser.add_argument("files", nargs="+")
    lint_parser.set_defaults(subcommand="fix")

    args = parser.parse_args()
    assert os.path.abspath(os.curdir) == str(
        mongo_dir.absolute()), "buildifier.py must be run from the root of the mongo repo"
    binary_name = "buildifier.exe" if platform.system() == "Windows" else "buildifier"
    if args.binary_dir:
        binary_path = os.path.join(args.binary_dir, binary_name)
    else:
        binary_path = os.path.join(os.curdir, binary_name)

    subcommand = args.subcommand
    if subcommand == "lint-all":
        lint_all(binary_path, args.generate_report)
    elif subcommand == "fix-all":
        fix_all(binary_path)
    elif subcommand == "lint":
        lint(binary_path, args.files, args.generate_report)
    elif subcommand == "fix":
        fix(binary_path, args.files)
    else:
        # we purposefully do not use sub.choices.keys() so it does not print as a dict_keys object
        choices = [key for key in sub.choices]
        raise RuntimeError(f"One of the following subcommands must be specified: {choices}")


if __name__ == "__main__":
    main()
