#!/usr/bin/env python3
"""Check that newly defined commands in new files are not legacy command types."""

import os
import re
import sys
from typing import List, Tuple

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts.linter.filediff import gather_changed_files_with_lines

LEGACY_TYPES = [
    "public Command {",
    "public BasicCommandWithReplyBuilderInterface",
    "public BasicCommand",
    "public BasicCommandWithRequestParser",
    "public ErrmsgCommandDeprecated",
]


def check_file_for_legacy_type(modified_lines: List[Tuple[int, str]]) -> bool:
    """Return false if a file defines a legacy command."""

    file_has_legacy_type = False

    for i in range(len(modified_lines)):
        modified_line = modified_lines[i][1]
        if any(legacy_type in modified_line for legacy_type in LEGACY_TYPES):
            print(f"On line {i}: {modified_line.strip()}")
            file_has_legacy_type = True

    return file_has_legacy_type


FILES_RE = re.compile("\\.(h|hpp|ipp|cpp|js)$")


def is_interesting_file(file_name):
    """Return true if this file should be checked."""
    return (
        file_name.startswith("src")
        and not file_name.startswith("src/third_party/")
        and not file_name.startswith("src/mongo/gotools/")
        and not file_name.startswith("src/mongo/db/modules/enterprise/src/streams/third_party")
        and not file_name.startswith("src/streams/third_party")
    ) and FILES_RE.search(file_name)


def main():
    """Search for newly defined commands in the server code and ensure they are not legacy types."""
    default_dir = os.getenv("BUILD_WORKSPACE_DIRECTORY", ".")
    os.chdir(default_dir)

    files = gather_changed_files_with_lines(is_interesting_file)
    found_legacy_command = False

    for file in files:
        hasLegacyCommandDefinition = check_file_for_legacy_type(files[file])

        if hasLegacyCommandDefinition:
            found_legacy_command = True
            print(
                file
                + " contains a legacy command type definition. Please define a TypedCommand instead."
            )

    if found_legacy_command:
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
