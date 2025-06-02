import argparse
import difflib
import os
import subprocess
import sys
import tempfile
from functools import cache
from typing import Dict, List, Optional, Set

import yaml
from codeowners.parsers import owners_v1, owners_v2
from codeowners.validate_codeowners import run_validator
from utils import evergreen_git

OWNERS_FILE_NAMES = ("OWNERS.yml", "OWNERS.yaml")
parsers = {
    "1.0.0": owners_v1.OwnersParserV1(),
    "2.0.0": owners_v2.OwnersParserV2(),
}


class FileNode:
    def __init__(self, directory: str):
        self.dirs: Dict[str, FileNode] = {}
        self.owners_file: Optional[str] = None
        self.directory = directory


def add_file_to_tree(root_node: FileNode, file_parts: List[str]):
    current_node = root_node
    for i, dir in enumerate(file_parts[:-1]):
        node_dirs = current_node.dirs
        if dir not in node_dirs:
            directory = "/".join(file_parts[: i + 1])
            node_dirs[dir] = FileNode(f"./{directory}")

        current_node = node_dirs[dir]

    assert (
        current_node.owners_file is None
    ), f"{'/'.join(file_parts[:-1])} there are two OWNERS files in this directory"
    current_node.owners_file = file_parts[-1]


def build_tree(files: List[str]) -> FileNode:
    root_node = FileNode("./")
    for file in files:
        file_parts = file.split("/")
        file_name = file_parts[-1]
        if file_name not in OWNERS_FILE_NAMES:
            continue
        add_file_to_tree(root_node, file_parts)

    return root_node


def process_owners_file(output_lines: list[str], node: FileNode) -> None:
    directory = node.directory
    file_name = node.owners_file
    if not file_name:
        return
    owners_file_path = os.path.join(directory, file_name)
    print(f"parsing: {owners_file_path}")
    output_lines.append(f"# The following patterns are parsed from {owners_file_path}")

    with open(owners_file_path, "r") as file:
        contents = yaml.safe_load(file)
        assert "version" in contents, f"Version not found in {owners_file_path}"
        assert contents["version"] in parsers, f"Unsupported version in {owners_file_path}"
        parser = parsers[contents["version"]]
        owners_lines = parser.parse(directory, owners_file_path, contents)
        output_lines.extend(owners_lines)
    output_lines.append("")


# Order matters, we need to always add the contents of the root directory to codeowners first
# and work our way to the outside directories in that order.
def process_dir(output_lines: list[str], node: FileNode) -> None:
    process_owners_file(output_lines, node)
    for directory in sorted(node.dirs.keys()):
        process_dir(output_lines, node.dirs[directory])


def print_diff_and_instructions(old_codeowners_contents, new_codeowners_contents):
    print("ERROR: New contents of codeowners file does not match old contents.")
    print("\nDifferences between old and new contents:")
    diff = difflib.unified_diff(
        old_codeowners_contents.splitlines(keepends=True),
        new_codeowners_contents.splitlines(keepends=True),
        fromfile="Old CODEOWNERS",
        tofile="New CODEOWNERS",
    )
    sys.stdout.writelines(diff)

    print("If you are seeing this message in CI you likely need to run `bazel run codeowners`")


def validate_generated_codeowners(validator_path: str) -> int:
    """Validate the generated CODEOWNERS file.

    Returns:
        int: 0 if validation succeeds, non-zero otherwise.
    """
    print("\nValidating generated CODEOWNERS file...")
    try:
        validation_result = run_validator(validator_path)
        if validation_result != 0:
            print("CODEOWNERS validation failed!", file=sys.stderr)
            return validation_result
        print("CODEOWNERS validation successful!")
        return 0
    except Exception as exc:
        print(f"Error during CODEOWNERS validation: {str(exc)}", file=sys.stderr)
        return 1


@cache
def get_unowned_files(codeowners_binary_path: str, codeowners_file: str = None) -> Set[str]:
    temp_output_file = tempfile.NamedTemporaryFile(delete=False, suffix=".txt")
    temp_output_file.close()
    codeowners_file_arg = ""
    if codeowners_file:
        codeowners_file_arg = f"--file {codeowners_file}"
    # This file can be bigger than the allowed subprocess buffer so we redirect output into a file
    command = f"{codeowners_binary_path} --unowned --tracked {codeowners_file_arg} > {temp_output_file.name}"
    process = subprocess.run(command, shell=True, stderr=subprocess.PIPE, text=True)

    if process.returncode != 0:
        print(process.stderr)
        raise RuntimeError("Error while trying to find unowned files")

    unowned_files = set()
    with open(temp_output_file.name, "r") as file:
        for line in file.read().split("\n"):
            if not line:
                continue
            parts = line.split()
            file_name = parts[0].strip()
            unowned_files.add(file_name)

    return unowned_files


def check_new_files(codeowners_binary_path: str, expansions_file: str, branch: str) -> int:
    new_files = evergreen_git.get_new_files(expansions_file, branch)
    if not new_files:
        print("No new files were detected.")
        return 0
    print(f"The following new files were detected: {new_files}")

    unowned_files = get_unowned_files(codeowners_binary_path)

    unowned_new_files = []
    for file in new_files:
        if file in unowned_files:
            unowned_new_files.append(file)

    if unowned_new_files:
        print("The following new files are unowned:", file=sys.stderr)
        for file in unowned_new_files:
            print(f"- {file}", file=sys.stderr)
        print(
            "New files are required to have code owners. See http://go/codeowners-ug",
            file=sys.stderr,
        )
        return 1

    print("There are no new files added that are unowned.")
    return 0


def check_orphaned_files(
    codeowners_binary_path: str, expansions_file: str, branch: str, codeowners_file: str
) -> int:
    # This compares the new codeowners file with the old codeowners file on the same working tree
    # This tells us which coverage is lost between codeowners file changes
    current_unowned_files = get_unowned_files(codeowners_binary_path)
    base_revision = evergreen_git.get_diff_revision(expansions_file, branch)
    previous_codeowners_file_contents = evergreen_git.get_file_at_revision(
        codeowners_file, base_revision
    )
    if previous_codeowners_file_contents is None:
        return 0
    temp_codeowners_file = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".txt")
    temp_codeowners_file.write(previous_codeowners_file_contents)
    temp_codeowners_file.close()
    old_unowned_files = get_unowned_files(codeowners_binary_path, temp_codeowners_file.name)

    unowned_files_difference = current_unowned_files - old_unowned_files
    if not unowned_files_difference:
        print("No files have lost ownership with these changes.")
        return 0

    print("The following files lost ownership with CODEOWNERS changes:", file=sys.stderr)
    for file in sorted(unowned_files_difference):
        print(f"- {file}", file=sys.stderr)

    return 1


def post_generation_checks(
    validator_path: str,
    should_run_validation: bool,
    codeowners_binary_path: str,
    should_check_new_files: bool,
    expansions_file: str,
    branch: str,
    codeowners_file_path: str,
) -> int:
    status = 0
    if should_run_validation:
        status |= validate_generated_codeowners(validator_path)
    if should_check_new_files:
        status |= check_new_files(codeowners_binary_path, expansions_file, branch)
        status |= check_orphaned_files(
            codeowners_binary_path, expansions_file, branch, codeowners_file_path
        )

    return status


def main():
    # If we are running in bazel, default the directory to the workspace
    default_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not default_dir:
        process = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True
        )
        default_dir = process.stdout.strip()

    codeowners_validator_path = os.environ.get("CODEOWNERS_VALIDATOR_PATH")
    if not codeowners_validator_path:
        raise RuntimeError("no CODEOWNERS_VALIDATOR_PATH env var found")

    codeowners_validator_path = os.path.abspath(codeowners_validator_path)

    codeowners_binary_path = os.environ.get("CODEOWNERS_BINARY_PATH")
    if not codeowners_binary_path:
        raise RuntimeError("no CODEOWNERS_BINARY_PATH env var found")

    codeowners_binary_path = os.path.abspath(codeowners_binary_path)

    parser = argparse.ArgumentParser(
        prog="GenerateCodeowners",
        description="This generates a CODEOWNERS file based off of our OWNERS.yml files. "
        "Whenever changes are made to the OWNERS.yml files in the repo this script "
        "should be run.",
    )

    parser.add_argument(
        "--output-file",
        help="Path of the CODEOWNERS file to be generated.",
        default=os.path.join(".github", "CODEOWNERS"),
    )
    parser.add_argument(
        "--repo-dir", help="Root of the repo to scan for OWNER files.", default=default_dir
    )
    parser.add_argument(
        "--check",
        help="When set, program exits 1 when the CODEOWNERS content changes. This will skip generation",
        default=False,
        action="store_true",
    )
    parser.add_argument(
        "--run-validation",
        help="When set, validation will be run against the resulting CODEOWNERS file.",
        default=True,
        action="store_false",
    )
    parser.add_argument(
        "--check-new-files",
        help="When set, this script will check new files to ensure they are owned.",
        default=True,
        action="store_false",
    )
    parser.add_argument(
        "--expansions-file",
        help="When set, implements CI specific logic around getting new files in a specific patch.",
        default=None,
        action="store",
    )
    parser.add_argument(
        "--branch",
        help="Helps the script understand what branch to compare against to see what new files are added when run locally. Defaults to master or main.",
        default=None,
        action="store",
    )

    args = parser.parse_args()
    os.chdir(args.repo_dir)

    # The lines to write to the CODEOWNERS file
    output_lines = [
        "# This is a generated file do not make changes to this file.",
        "# This is generated from various OWNERS.yml files across the repo.",
        "# To regenerate this file run `bazel run codeowners`",
        "# The documentation for the OWNERS.yml files can be found here:",
        "# https://github.com/10gen/mongo/blob/master/docs/owners/owners_format.md",
        "",
    ]

    print(f"Scanning for OWNERS.yml files in {os.path.abspath(os.curdir)}")
    try:
        files = evergreen_git.get_files_to_lint()
        root_node = build_tree(files)
        process_dir(output_lines, root_node)
    except Exception as ex:
        print("An exception was found while generating the CODEOWNERS file.", file=sys.stderr)
        print(
            "Please refer to the docs to see the spec for OWNERS.yml files here :", file=sys.stderr
        )
        print(
            "https://github.com/10gen/mongo/blob/master/docs/owners/owners_format.md",
            file=sys.stderr,
        )
        raise ex

    old_contents = ""
    check = args.check
    output_file = args.output_file
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    if check and os.path.exists(output_file):
        with open(output_file, "r") as file:
            old_contents = file.read()

    # prioritize env var for check new file configuration
    should_check_new_files = os.environ.get("CODEOWNERS_CHECK_NEW_FILES", None)
    if should_check_new_files is not None:
        if should_check_new_files.lower() == "false":
            should_check_new_files = False
        elif should_check_new_files.lower() == "true":
            should_check_new_files = True
        else:
            raise RuntimeError(
                f"Invalid value for CODEOWNERS_CHECK_NEW_FILES: {should_check_new_files}"
            )
    else:
        should_check_new_files = args.check_new_files

    new_contents = "\n".join(output_lines)
    if check:
        if new_contents != old_contents:
            print_diff_and_instructions(old_contents, new_contents)
            return 1

        print("CODEOWNERS file is up to date")
        return post_generation_checks(
            codeowners_validator_path,
            args.run_validation,
            codeowners_binary_path,
            should_check_new_files,
            args.expansions_file,
            args.branch,
            output_file,
        )

    with open(output_file, "w") as file:
        file.write(new_contents)
        print(f"Successfully wrote to the CODEOWNERS file at: {os.path.abspath(output_file)}")

    # Add validation after generating CODEOWNERS file
    return post_generation_checks(
        codeowners_validator_path,
        args.run_validation,
        codeowners_binary_path,
        should_check_new_files,
        args.expansions_file,
        args.branch,
        output_file,
    )


if __name__ == "__main__":
    exit(main())
