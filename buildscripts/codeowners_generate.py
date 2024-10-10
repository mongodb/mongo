import argparse
import difflib
import glob
import os
import pathlib
import subprocess
import sys
from functools import lru_cache

import yaml

OWNERS_FILE_NAME = "OWNERS.yml"


def add_pattern(output_lines: list[str], pattern: str, owners: set[str]) -> None:
    if owners:
        output_lines.append(f"{pattern} {' '.join(sorted(owners))}")
    else:
        output_lines.append(pattern)


def add_owner_line(output_lines: list[str], directory: str, pattern: str, owners: set[str]) -> None:
    # ensure the path is correct and consistent on all platforms
    directory = pathlib.PurePath(directory).as_posix()

    if directory == ".":
        # we are in the root dir and can directly pass the pattern
        parsed_pattern = pattern
    elif not pattern:
        # If there is no pattern add the directory as the pattern.
        parsed_pattern = f"/{directory}/"
    elif "/" in pattern:
        # if the pattern contains a slash the pattern should be treated as relative to the
        # directory it came from.
        if pattern.startswith("/"):
            parsed_pattern = f"/{directory}{pattern}"
        else:
            parsed_pattern = f"/{directory}/{pattern}"
    else:
        parsed_pattern = f"/{directory}/**/{pattern}"

    test_pattern = (
        f".{parsed_pattern}" if parsed_pattern.startswith("/") else f"./**/{parsed_pattern}"
    )

    # ensure at least one file patches the pattern.
    first_file_found = glob.iglob(test_pattern, recursive=True)
    if all(False for _ in first_file_found):
        raise (RuntimeError(f"Can not find any files that match pattern: `{pattern}`"))

    add_pattern(output_lines, parsed_pattern, owners)


@lru_cache(maxsize=None)
def process_alias_import(path: str) -> dict[str, list[str]]:
    if not path.startswith("//"):
        raise RuntimeError(
            f"Alias file paths must start with // and be relative to the repo root: {path}"
        )

    # remove // from beginning of path
    parsed_path = path[2::]

    if not os.path.exists(parsed_path):
        raise RuntimeError(f"Could not find alias file {path}")

    with open(parsed_path, "r") as file:
        contents = yaml.safe_load(file)
        assert "version" in contents, f"Version not found in {path}"
        assert "aliases" in contents, f"Alias not found in {path}"
        assert contents["version"] == "1.0.0", f"Invalid version in {path}"
        return contents["aliases"]


def process_owners_file(output_lines: list[str], directory: str) -> None:
    owners_file_path = os.path.join(directory, OWNERS_FILE_NAME)
    if not os.path.exists(owners_file_path):
        return
    print(f"parsing: {owners_file_path}")
    output_lines.append(f"# The following patterns are parsed from {owners_file_path}")

    with open(owners_file_path, "r") as file:
        contents = yaml.safe_load(file)
        assert "version" in contents, f"Version not found in {owners_file_path}"
        assert contents["version"] == "1.0.0", f"Invalid version in {owners_file_path}"
        no_parent_owners = False
        if "options" in contents:
            options = contents["options"]
            no_parent_owners = "no_parent_owners" in options and options["no_parent_owners"]

        if no_parent_owners:
            # Specfying no owners will ensure that no file in this directory has an owner unless it
            # matches one of the later patterns in the file.
            add_owner_line(output_lines, directory, pattern="*", owners=None)

        aliases = {}
        if "aliases" in contents:
            for alias_file in contents["aliases"]:
                aliases.update(process_alias_import(alias_file))
        if "filters" in contents:
            filters = contents["filters"]
            for _filter in filters:
                assert (
                    "approvers" in _filter
                ), f"Filter in {owners_file_path} does not have approvers."
                approvers = _filter["approvers"]
                del _filter["approvers"]
                if "metadata" in _filter:
                    del _filter["metadata"]

                # the last key remaining should be the pattern for the filter
                assert len(_filter) == 1, f"Filter in {owners_file_path} has incorrect values."
                pattern = next(iter(_filter))
                owners: set[str] = set()

                def process_owner(owner: str):
                    if "@" in owner:
                        # approver is email, just add as is
                        if not owner.endswith("@mongodb.com"):
                            raise RuntimeError("Any emails specified must be a mongodb.com email.")
                        owners.add(owner)
                    else:
                        # approver is github username, need to prefix with @
                        owners.add(f"@{owner}")

                NOOWNERS_NAME = "NOOWNERS-DO-NOT-USE-DEPRECATED-2024-07-01"
                if NOOWNERS_NAME in approvers:
                    assert (
                        len(approvers) == 1
                    ), f"{NOOWNERS_NAME} must be the only approver when it is used."
                else:
                    for approver in approvers:
                        if approver in aliases:
                            for member in aliases[approver]:
                                process_owner(member)
                        else:
                            process_owner(approver)
                    # Add the auto revert bot
                    process_owner("svc-auto-approve-bot")

                add_owner_line(output_lines, directory, pattern, owners)
    output_lines.append("")


# Order matters, we need to always add the contents of the root directory to codeowners first
# and work our way to the outside directories in that order.
def process_dir(output_lines: list[str], directory: str) -> None:
    process_owners_file(output_lines, directory)
    for item in sorted(os.listdir(directory)):
        path = os.path.join(directory, item)
        if not os.path.isdir(path) or os.path.islink(path):
            continue

        process_dir(output_lines, path)


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

    print("If you are seeing this message in CI you likely need to run `bazel run //:codeowners`")
    print("*** IF BAZEL IS NOT INSTALLED, RUN THE FOLLOWING: ***\n")
    print("python buildscripts/install_bazel.py")


def main():
    # If we are running in bazel, default the directory to the workspace
    default_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not default_dir:
        process = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True
        )
        default_dir = process.stdout.strip()

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

    args = parser.parse_args()
    os.chdir(args.repo_dir)

    # The lines to write to the CODEOWNERS file
    output_lines = [
        "# This is a generated file do not make changes to this file.",
        "# This is generated from various OWNERS.yml files across the repo.",
        "# To regenerate this file run `bazel run //:codeowners`",
        "# If bazel is not installed, run the following:",
        "#  python buildscripts/install_bazel.py",
        "# The documentation for the OWNERS.yml files can be found here:",
        "# https://github.com/10gen/mongo/blob/master/docs/owners/owners_format.md",
        "",
    ]

    print(f"Scanning for OWNERS.yml files in {os.path.abspath(os.curdir)}")
    try:
        process_dir(output_lines, "./")

        # TODO(SERVER-93711) remove exemptions after the Bazel migration is complete.
        output_lines.append(
            "# The following patterns are added by the generator script as exemptions during the Bazel migration"
        )
        add_owner_line(output_lines, "./", "**/SConscript", set())
        add_owner_line(output_lines, "./", "**/BUILD.bazel", set())
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

    new_contents = "\n".join(output_lines)
    if check:
        if new_contents != old_contents:
            print_diff_and_instructions(old_contents, new_contents)
            return 1

        print("CODEOWNERS file is up to date")
        return 0

    with open(output_file, "w") as file:
        file.write(new_contents)
        print(f"Successfully wrote to the CODEOWNERS file at: {os.path.abspath(output_file)}")

    return 0


if __name__ == "__main__":
    exit(main())
