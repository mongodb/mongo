#!/opt/mongodbtoolchain/v4/bin/python3
"""
Wraps clang tidy to include our custom checks.

This script acts as a wrapper for the `clang-tidy` tool, allowing it to include custom checks
defined in a shared object file (`libmongo_tidy_checks.so`). Additionally, it filters the files
to be checked, ensuring that only files within the `src/mongo` directory are processed, excluding
those within `src/mongo/db/modules/enterprise/src/streams/third_party`.

Input:
- The script expects command-line arguments that are passed to `clang-tidy`.
- These arguments can include file paths, options, and other parameters supported by `clang-tidy`.

Output:
- The script runs `clang-tidy` on the specified files and outputs the results.
- If no valid `.cpp` files are found, or if all `.cpp` files are located in the excluded directories,
  the script skips running `clang-tidy`.
- Standard output and error from the `clang-tidy` process are captured and printed.

Expected Format:
- command line example: buildscripts/clang_tidy_vscode.py /path/to/file/filename1  --export-fixes=-
- buildscripts/clang_tidy_vscode.py /path/to/file/filename1  /path/to/file/filename2 --export-fixes=-
"""

# TODO: if https://github.com/notskm/vscode-clang-tidy/pull/77#issuecomment-1422910143 is resolved then this script can be removed

import os
import subprocess
import sys

CHECKS_SO = "build/install/lib/libmongo_tidy_checks.so"


def main():
    clang_tidy_args = ["/opt/mongodbtoolchain/v4/bin/clang-tidy"]
    if os.path.isfile(CHECKS_SO):
        clang_tidy_args += [f"-load={CHECKS_SO}"]

    # Filter out non src/mongo files for clang tidy checks
    files_to_check = []
    other_args = []
    for arg in sys.argv[1:]:
        if os.path.isfile(arg):
            source_relative_path = os.path.relpath(arg, os.path.dirname(os.path.dirname(__file__)))
            if (
                arg.endswith(".cpp")
                and source_relative_path.startswith("src/mongo")
                # TODO: SERVER-79076 remove this condition when resolved
                and not source_relative_path.startswith(
                    "src/mongo/db/modules/enterprise/src/streams/third_party"
                )
            ):
                files_to_check.append(arg)
        else:
            other_args.append(arg)

    # No files to check in src/mongo. Skipping clang-tidy
    if not files_to_check:
        return 0

    clang_tidy_args += files_to_check + other_args

    proc = subprocess.run(clang_tidy_args, capture_output=True)
    # Write to output buffer here because that is how to copy directly from stdin to stdout without making assumptions about encoding
    sys.stdout.buffer.write(proc.stdout)
    sys.stderr.buffer.write(proc.stderr)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
